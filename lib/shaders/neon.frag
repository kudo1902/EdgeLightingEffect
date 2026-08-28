precision highp float;

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// All other tuning constants (FILAMENT_*, HALO_*, BLOOM_*, grading, epsilons)
// are injected from lib/include/renderer/neon-tuning.h via @NEON_TUNING@ in
// shaders.h.in - single source of truth shared with the C++ renderer.
//
// (Far culling lives on the CPU: the draw quad is sized to rect + glowReach,
//  so there's no per-fragment discard here. See neon-renderer.cpp.)

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform float uIntensity;
uniform float uTime;
uniform float uHueRotationRate;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Positive px distance INSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel CPU-side so this branch no-ops.
uniform float uInsideCutoffSoftness;  ///< Feather width in px at the inside cutoff boundary.
uniform float uOutsideCutoff;         ///< Positive px distance OUTSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width in px at the outside cutoff boundary.
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).

// Loop sample positions (perimeter points) as a std140 uniform block. Each
// entry is packed as a vec4 (only .xy is meaningful; std140 pads vec2 to a
// 16-byte stride anyway) so the shader reads raw float32 out of the constant
// cache in the gather loop. Fixed size at compile time - see the shared
// NEON_MAX_LOOP_SAMPLES tuning constant.
layout(std140) uniform LoopSamplesBlock
{
    vec4 uLoopSamples[NEON_MAX_LOOP_SAMPLES];
};

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes one element per registered property ("uSegments[0]", ...) into
// the block's UBO at the reflected std140 array stride. On desktop GL the
// block is fed from a UBO in neon-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

// Per-segment gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per
// segment, laid out head-to-tail across the segment's visible span. Sampled
// only when the segment's hasStops flag is set (see uSegments.w above).
uniform sampler2D uSegmentLUT;

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). Overlap resolves winner-take-all:
// per sample, the arc with the largest effective mask (arcInside * intensity)
// contributes its colour and its mask to the emission. Because arcInside is
// smoothstepped 1-sample-wide at each end, adjacent arcs of different colours
// crossfade at the seam rather than snapping.
//
// When uArcCount == 0 the entire perimeter is dark; the default config seeds
// one full-perimeter arc so this only happens if the host wipes the vector.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Per-arc gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per arc,
// same layout convention as uSegmentLUT: a head-to-tail SPAN, baked with
// ColorUtils::SampleSpan, so the end colours hold rather than wrapping round.
//
// Read in TWO places, and the second is easy to miss. The pre-pass samples
// only the WINNING arc's row, for the gather hue. But the pointwise emitCover
// loop in main() samples the row of EVERY arc that covers this fragment, for
// its colour-stop alpha - so a row is live whenever its arc has stops, and the
// non-winning rows cannot be left stale. SpanAtlasLUT::Bake zero-fills the
// whole atlas on every bake (mAtlas.assign, not resize, for exactly this
// reason), which is what currently keeps that true.
uniform sampler2D uArcLUT;

// 1-row 2D LUT (REPEAT-wrapped) holding the precomputed colour ring.
// Replaces the in-shader sampleStops loop + HSV blend on the hot path.
// GLES 3.0 does not support sampler1D, so we use a 1-row 2D texture.
uniform sampler2D uGradientLUT;

// Perimeter emission table from neon-emission.frag: NEON_MAX_LOOP_SAMPLES
// wide, 2 tall, RGBA16F (RGBA8 where the driver refuses float rendering).
//   row 0: .rgb = arcColour * arcW, .a = arcW
//   row 1: .rgb = SUM(segColour * bell), .a = SUM(bell)
// Read with texelFetch at integer sample index - never filtered, since
// neighbouring texels are unrelated perimeter samples. See the gather loop
// and docs/emission-prepass.md.
uniform sampler2D uEmission;

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform float uQuadMargin;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}

// Exact per-fragment perimeter position: maps this fragment's local-space point
// back to its arc-length parameter t in [0, 1), matching the CPU's
// GeometryUtils::GetPointOnRectangle for BOTH windings (uWinding = 0/1 for
// CLOCKWISE / COUNTER_CLOCKWISE). It replaces the proximity-weighted circular
// mean of the sample angles: near a corner the corner samples' phases wrap
// through 2*pi right into the arc's start, so the mean smears the whole corner
// curve to ~0 and the filament gate lights a corner that an arc starting at 0
// should leave dark. The geometric inverse reads the nearest perimeter point
// directly, so corner and edge fragments get their true positions.
float perimeterPosition(vec2 p) {
    float halfW  = uRectSize.x * 0.5;
    float halfH  = uRectSize.y * 0.5;
    float r      = clamp(uCornerRadius, 0.0, min(halfW, halfH));
    float halfWs = halfW - r;
    float halfHs = halfH - r;
    float ws     = uRectSize.x - 2.0 * r;
    float hs     = uRectSize.y - 2.0 * r;
    float arcLen = PI * r * 0.5;
    float peri   = 2.0 * ws + 2.0 * hs + 4.0 * arcLen;

    // Closest point on the rounded-rect perimeter (inverse rounded-box SDF).
    vec2  b  = vec2(halfWs, halfHs);
    vec2  c  = clamp(p, -b, b);
    vec2  d  = p - c;
    float dl = length(d);
    vec2  cp;
    if (dl > 1e-6)
    {
        cp = c + d * (r / dl);
    }
    else
    {
        // Inside the inner box: project straight along the dominant axis to
        // the nearest edge.
        vec2  e  = b - abs(p);
        float sx = (p.x >= 0.0) ? 1.0 : -1.0;
        float sy = (p.y >= 0.0) ? 1.0 : -1.0;
        cp = (e.x < e.y) ? vec2(sx * halfW, p.y) : vec2(p.x, sy * halfH);
    }

    float ax = abs(cp.x);
    float ay = abs(cp.y);

    // Canonical segment id (0..7 in CW order: top, TR, right, BR, bottom, BL,
    // left, TL) and the traversal progress u in [0, 1] measured in the CW
    // direction. CCW runs the same geometric core with mirrored progress
    // (1 - u) and a CCW segment layout, so both windings stay exact.
    int   seg;
    float u;
    if (ax > halfWs && ay > halfHs)
    {
        // Corner arc. The angle of the offset from the corner centre (radius r)
        // gives the fraction across the quarter-arc.
        float sx = (cp.x >= 0.0) ? 1.0 : -1.0;
        float sy = (cp.y >= 0.0) ? 1.0 : -1.0;
        float th = atan(cp.y - sy * halfHs, cp.x - sx * halfWs);
        if (sx > 0.0 && sy > 0.0)
        {
            seg = 1;                                     // top-right: theta 0..pi/2
            u   = (HALF_PI - th) / HALF_PI;
        }
        else if (sx > 0.0)
        {
            seg = 3;                                     // bottom-right: theta -pi/2..0
            u   = -th / HALF_PI;
        }
        else if (sy < 0.0)
        {
            seg = 5;                                     // bottom-left: theta -pi/2..-pi
            if (th > 0.0) th -= TWO_PI;                  // atan2 hands the left tangency back as +pi
            u = (-HALF_PI - th) / HALF_PI;
        }
        else
        {
            seg = 7;                                     // top-left: theta pi/2..pi
            u   = (PI - th) / HALF_PI;
        }
    }
    else if (ay >= halfHs)
    {
        if (cp.y > 0.0)
        {
            seg = 0;                                     // top edge: left to right
            u   = (cp.x + halfWs) / ws;
        }
        else
        {
            seg = 4;                                     // bottom edge: right to left
            u   = (halfWs - cp.x) / ws;
        }
    }
    else if (ax >= halfWs)
    {
        if (cp.x > 0.0)
        {
            seg = 2;                                     // right edge: top to bottom
            u   = (halfHs - cp.y) / hs;
        }
        else
        {
            seg = 6;                                     // left edge: bottom to top
            u   = (cp.y + halfHs) / hs;
        }
    }
    else
    {
        seg = 0;                                         // degenerate - never hit for r > 0
        u   = 0.0;
    }

    float base;
    float len;
    if (uWinding == 0)
    {
        // Segment starts (cumulative) in CW order: top, TR, right, BR, bottom,
        // BL, left, TL.
        switch (seg)
        {
        case 0: base = 0.0;                                   len = ws;     break;
        case 1: base = ws;                                    len = arcLen; break;
        case 2: base = ws + arcLen;                           len = hs;     break;
        case 3: base = ws + arcLen + hs;                      len = arcLen; break;
        case 4: base = ws + 2.0 * arcLen + hs;                len = ws;     break;
        case 5: base = ws + 2.0 * arcLen + hs + ws;           len = arcLen; break;
        case 6: base = ws + 3.0 * arcLen + hs + ws;           len = hs;     break;
        default: base = peri - arcLen;                        len = arcLen; break;
        }
        return (base + len * u) / peri;
    }

    // Segment starts in CCW order: left, BL, bottom, BR, right, TR, top, TL.
    switch (seg)
    {
    case 0: base = 2.0 * hs + 3.0 * arcLen + ws;              len = ws;     break;
    case 1: base = 2.0 * hs + 2.0 * arcLen + ws;              len = arcLen; break;
    case 2: base = hs + 2.0 * arcLen + ws;                    len = hs;     break;
    case 3: base = hs + arcLen + ws;                          len = arcLen; break;
    case 4: base = hs + arcLen;                               len = ws;     break;
    case 5: base = hs;                                        len = arcLen; break;
    case 6: base = 0.0;                                       len = hs;     break;
    default: base = 2.0 * hs + 3.0 * arcLen + 2.0 * ws;       len = arcLen; break;
    }
    return (base + len * (1.0 - u)) / peri;
}

// NOTE: arcInside() used to live here. It shaped the colour gather only -
// picking the winner-take-all arc per sample and weighting that sample in
// the hue average - so it was a pure function of (si, config) and moved to
// neon-emission.frag with the rest of the per-sample work. The visible
// extent of an arc (filament, halo, bloom) never came from it; that is
// arcCoverContinuous below, whose feather is INWARD and in pixels.

// uArcs[].w is a BITMASK, not a bool - packed CPU-side in packLightBlocks:
//   bit 0 (1) - the arc has its own colour stops (read row `a` of uArcLUT)
//   bit 1 (2) - another arc covers the perimeter immediately BEFORE its start
//   bit 2 (4) - another arc covers the perimeter immediately AFTER its end
// The abutment bits pick each endpoint's feather direction in
// arcCoverContinuous. They are a pure function of the arc set, so they are
// resolved once per frame on the CPU rather than rediscovered per fragment by
// an O(arcs^2) scan. Values are 0..7, all exact in a float.
bool arcHasStops(float flags)  { return mod(flags, 2.0) >= 0.5; }
bool arcTailAbuts(float flags) { return mod(floor(flags * 0.5), 2.0) >= 0.5; }
bool arcHeadAbuts(float flags) { return mod(floor(flags * 0.25), 2.0) >= 0.5; }

// Continuous [0,1] coverage of a fragment for the arc [start, start+length],
// used to gate the sharp SDF filament.
//
// The feather direction is decided PER ENDPOINT, from whether another arc
// takes over there (the @c tailAbuts / @c headAbuts flags, computed on the CPU
// and packed into uArcs[].w - see packLightBlocks):
//
//   FREE endpoint     -> ramp INWARD. Coverage is exactly 0 at the endpoint, so
//                        nothing outside the arc's own span is ever lit.
//   ABUTTING endpoint -> ramp OUTWARD, past the endpoint into the neighbour's
//                        span. Coverage is 1 at the endpoint, so max() over the
//                        two arcs hands over at full brightness with no notch,
//                        and the overlap makes the handover smooth rather than
//                        a hard step when the two carry different intensities.
//
// Both halves are needed, and a straddling feather (0.5 at every endpoint) is
// NOT an acceptable shortcut for either:
//
//   - Feathering inward everywhere is what produced the dark seam notch: two
//     arcs tiling the ring ({0, 0.5} and {0.5, 0.5}) both evaluated to exactly
//     0 at their shared seam, and no combining operator recovers a signal from
//     (0, 0). The notch spanned fTail + fHead = 28 px and was punched radially
//     outward through the halo and bloom, which emitCover also scales.
//
//   - Feathering outward everywhere - including at free endpoints - breaks
//     worse, and specifically at cornerRadius 0. The inverse-SDF map is
//     DEGENERATE at a sharp corner: the entire 90-degree exterior wedge has the
//     corner as its nearest perimeter point, so every fragment in that quadrant
//     shares ONE sPos. Coverage is a function of sPos alone, so any non-zero
//     value at a corner is painted across the whole quadrant. An arc starting
//     at a corner (start = 0 on a square rect, the default corner) lit its
//     entire top-left exterior quadrant at half brightness, bounded by two hard
//     edges where the neighbouring fragments mapped to uncovered perimeter.
//     A 7 px bleed in perimeter space is not a 7 px bleed on screen.
//
// Deciding per endpoint keeps the outward ramp exactly where it is safe: an
// abutting endpoint's "bleed" lands inside a neighbour that is already lit, so
// it cannot reach unlit geometry however degenerate the map is there.
//
// Feather widths are perimeter fractions (pixel-space widths / current perimeter,
// converted at the call site).
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail,
                         bool tailAbuts, bool headAbuts) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // wrap to [0, 1): distance past start
    // An outward tail ramp reaches BEHIND the start, so those fragments need a
    // small NEGATIVE rel rather than one wrapped up to near 1. Split the gap
    // between the arc's head and its own start down the middle: past that
    // midpoint a fragment is approaching the start, not trailing the head.
    // Only for an outward tail - an inward one never reads rel < 0.
    if (tailAbuts && rel > 0.5 * (1.0 + length)) { rel -= 1.0; }
    // Cap each feather at a share of the arc's own length. The widths arrive
    // as a fixed pixel span but `length` is a perimeter FRACTION, so on a small
    // rect a short arc can be narrower than the two ramps combined - they then
    // overlap and clip the peak, making the same arc config dimmer on a smaller
    // rect (0.21 vs 1.00 for L = 0.02 at 200x150 vs 800x600). Capping keeps the
    // peak at 1.0 for any length at any size, and leaves long arcs untouched.
    float cap    = length * ARC_FEATHER_MAX_SHARE;
    float fH     = min(fHead, cap);
    float fT     = min(fTail, cap);
    // Tail: inward ramps 0 -> 1 over [start, start + fT]; outward ramps over
    // [start - fT, start], reaching 1 AT the start.
    float tailIn = tailAbuts ? smoothstep(-fT, 0.0, rel)
                             : smoothstep(0.0, fT, rel);
    // Head: inward falls 1 -> 0 over [end - fH, end]; outward holds 1 to the
    // end and falls over [end, end + fH].
    float headIn = headAbuts ? 1.0 - smoothstep(length, length + fH, rel)
                             : 1.0 - smoothstep(length - fH, length, rel);
    return tailIn * headIn;
}

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far exterior is culled on the CPU - the draw quad is
    // sized to rect + glowReach in NeonRenderer::setupGeometry, so geometry culls
    // the far region instead of a per-fragment discard (tiler-friendly).
    // The one-sided cuts below stay as discards: they cull a useful half-band
    // the quad can't express.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // Hard geometric cutoffs. The band is [-uInsideCutoff, +uOutsideCutoff]
    // with a per-side softness feather straddling each boundary; anything
    // past the feather is culled here so bloom/halo can't leak beyond the
    // artist's stated reach even if uGlowRadius says otherwise. Softness is
    // decoupled from uGlowSideSoftness so the one-sided cut at d=0 can stay
    // razor-sharp while the cutoff joins fade smoothly, and per-side so the
    // interior and exterior can taper at different rates. Disabled sides
    // arrive with size = a huge sentinel, so these branches no-op.
    float inSoft  = max(uInsideCutoffSoftness,  SIDE_SOFT_EPSILON);
    float outSoft = max(uOutsideCutoffSoftness, SIDE_SOFT_EPSILON);
    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners instead of being rounded by the cut distance.
    float dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, uOutsideCutoff);
    float dIn  = bandInnerDistance(d, uInsideCutoff);
    if (dOut >  outSoft) discard;
    if (dIn  < -inSoft ) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile with exponentially smooth falloff:
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 -> N = 1   (Laplace - heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 -> N = 2   (Gaussian - pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 -> N = 4   (platykurtic - flatter top, sharper shoulder)
    //   uFilamentFalloff = 5.0 -> N = 10  (near-rectangular)
    //
    // The Gaussian has no power-law tail (unlike the old super-Lorentzian),
    // so the filament reads as a clean thin line with a naturally smooth
    // roll-off - no heavy glow bleed far from the line axis.
    //
    // Peak at ad = 0 is always exactly 1.0.
    //
    // lineGate fades the filament from 0 at lineWidth = 0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth = 0 means "no
    // line" instead of a single-pixel bright dot.
    //
    // FILAMENT_MIN_HALF_WIDTH is used raw: uLineWidth is already in the same
    // full-res px. neon-optimized.frag's copy of this block multiplies it by
    // uResolutionScale because uLineWidth reaches it in FBO px - keep the two
    // in step when tuning the constant.
    float halfWidth = uLineWidth * 0.5;
    float sigma     = max(halfWidth, FILAMENT_MIN_HALF_WIDTH);
    float N         = 2.0 * max(uFilamentFalloff, 1e-3);
    float core      = exp2(-pow(ad / sigma, N));

    // Filament reach, in sigmas, for THIS falloff - see neon-tuning.h. Also
    // sizes the draw quad CPU-side; the two must stay in step.
    float reachSigmas = clamp(pow(log2(FILAMENT_GAIN / FILAMENT_CUTOFF), 1.0 / N),
                              FILAMENT_REACH_MIN_SIGMAS, FILAMENT_REACH_MAX_SIGMAS);
    // Pedestal-subtract the core so it reaches exactly zero at that reach,
    // renormalised to keep the ad = 0 peak at 1.0. Where the reach formula is
    // honoured the pedestal is ~1.7e-4 and invisible; where MAX_SIGMAS clamps
    // it (soft falloff, whose tail would otherwise run for hundreds of sigmas)
    // this is what makes the glow end smoothly and, crucially, SYMMETRICALLY.
    // Without it the interior kept the full tail while the exterior was cut at
    // the quad edge.
    float corePed   = exp2(-pow(reachSigmas, N));
    core            = max(core - corePed, 0.0) / max(1.0 - corePed, 1e-6);
    float lineGate  = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

    // Perimeter of the rounded rect, in px. Used twice below: to size the
    // colour-gather kernel, and to convert the arc feathers from px to
    // perimeter fractions.
    float r    = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;

    // --- Kernel widths ------------------------------------------------
    // Two separate kernels, and the split is the point:
    //
    //  - kc is the COLOUR gather weight, and it is the one length here that is
    //    NOT in pixels. The blend is a 128-sample sum over the gradient LUT,
    //    which is indexed by perimeter FRACTION, so a pixel-sized kernel spans
    //    a different slice of the gradient on every geometry - the same stops
    //    render washed out small and crisp large. Sizing it as a fraction of
    //    the perimeter makes the gradient read identically at any size, and
    //    pins the kernel at a constant 1.13 sample spacings so it cannot bead
    //    at any size either. Not coupled to glowRadius: the gather is colour
    //    only, so a wide glow has no business desaturating the ring. See
    //    neon-tuning.h for the measurements behind this.
    //
    //  - kh / bw are the EMISSION widths: raw glowRadius, no floor. The halo
    //    and bloom are evaluated analytically from the SDF distance further
    //    down, and a closed form cannot bead however far apart the gather
    //    samples are, so glowRadius is proportional across its entire range.
    float kc  = max(peri * COLOR_BLEND_PERIM_FRAC, EMISSION_MIN_WIDTH);
    float kc2 = kc * kc;
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);

    // --- Colour gather -----------------------------------------------------
    // This loop gathers COLOUR ONLY - the halo and bloom intensities are
    // computed in closed form after it, and every per-sample colour / mask
    // term is precomputed by neon-emission.frag into uEmission.
    //
    // Per iteration: 1 UBO read for the sample position, 1 sub, 1 dot, 1
    // reciprocal, and 2 texelFetches. What used to live here - the arc
    // winner-take-all scan over uArcCount, the segment loop over
    // uSegmentCount, and one to two FILTERED LUT fetches - was a pure function
    // of (si, uTime, config), so it did not belong in a loop that runs once
    // per fragment. Hoisting it also removed two dynamic inner loops (which
    // blocked unrolling), a serial reduction (`if (mask > bestMask)`), and the
    // loop-carried `si += dti` chain.
    //
    // See docs/emission-prepass.md for the packing and the invariant that
    // keeps the split honest.
    vec3  acc       = vec3(0.0); // base colour x arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour x bell x gather weight
    float wsumLit   = 0.0; // SUM ARC-GATED g     - normalises `col` (see below)
    float wsumSegW  = 0.0; // SUM SEGMENT bell*g  - normalises the segment hue

    // Compile-time constant loop bound: matches the LoopSamplesBlock UBO size
    // and the C++-side NEON_MAX_LOOP_SAMPLES so the compiler can unroll if it
    // wants to.
    const int numSamples = NEON_MAX_LOOP_SAMPLES;

    for (int i = 0; i < numSamples; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kc2);

        // Both rows of the emission table for this sample. Row 0 carries the
        // arc term already premultiplied by its own gather weight arcW, plus
        // arcW itself for the denominator; row 1 does the same for the summed
        // segment term. texelFetch (not texture): integer sample index, no
        // filtering, no wrap math, no LOD derivatives.
        vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);
        vec4 e1 = texelFetch(uEmission, ivec2(i, 1), 0);

        // GATED normalisation, and it is the point. Dividing by the same
        // weight the numerator was gathered with makes `col` a pure hue of
        // unit magnitude: it carries no coverage and no per-arc intensity,
        // both of which cancel. Those reach the emission solely through
        // emitCover / filamentGate below, which are px-based and
        // size-invariant. segAcc / wsumSegW does the identical thing for the
        // segment hue.
        //
        // Both used to divide by an UNGATED sum over every sample, so an unlit
        // far side of the ring dragged the lit colour toward black by roughly
        // kc / rectHeight. With kc pinned to a fixed px span that ratio grew as
        // the rect shrank: a quarter-perimeter arc measured 0.79 of full
        // brightness at 200x150 against 0.97 at 1920x1080. Gated normalisation
        // is exactly 1.0 at every size.
        //
        // e0.rgb is baseColI * arcW and e0.a is arcW, so these two lines are
        // exactly the old `acc += baseColI * lg` / `wsumLit += lg` with
        // lg = g * arcW.
        acc      += e0.rgb * g;
        wsumLit  += e0.a   * g;

        // Segments are gathered with the raw proximity weight g, NOT the
        // arc-gated one, so a segment lights even on perimeter stretches no
        // arc covers. e1 holds SUM(segColour * bell) and SUM(bell) over every
        // segment, so the old inner loop collapses to one add each.
        segAcc   += e1.rgb * g;
        wsumSegW += e1.a   * g;
    }

    // Both are pure hues of unit magnitude now; the magnitudes are attached
    // below from the pointwise coverages.
    vec3 col       = acc    / max(wsumLit,  WSUM_EPSILON); // base perimeter hue
    vec3 segColHue = segAcc / max(wsumSegW, WSUM_EPSILON); // segment hue

    // --- Continuous coverage, read at this fragment's own position -------
    // Recover the fragment's OWN continuous perimeter position GEOMETRICALLY
    // from vPos (inverse of the CPU's GetPointOnRectangle) and read each arc
    // directly there. Far-from-line fragments get a valid position too, but
    // their filament core ~= 0 so it never shows. The geometric inverse is
    // exact even at corners, unlike the old proximity-weighted circular mean
    // of the sample phases, which smeared the whole corner curve to ~0 and
    // lit it for any arc starting at position 0.
    float sPos = perimeterPosition(vPos);
    // Inward feathers: convert pixel widths to perimeter fractions at the
    // current geometry (`peri` is computed above the gather). Both sides are
    // full-res px here, so the constants are used raw; neon-optimized.frag
    // scales them by uResolutionScale because its `peri` is in FBO px - keep
    // the two in step when tuning them.
    float headF  = HEAD_FEATHER_PX / peri;
    float tailF  = TAIL_FEATHER_PX / peri;
    // ONE arc coverage, folding per-arc intensity in, and it drives the
    // filament as well as the halo and bloom. `col` is gated-normalised above,
    // so intensity cancels out of it and can no longer reach the filament that
    // way - emitCover is what carries it. The scaling stays linear in
    // intensity, exactly as it was when it rode on `col`, and both layers are
    // now shaped by the same px-based (size-invariant) feathers.
    // Colour-stop ALPHA rides here, on the magnitude, for two reasons.
    //
    // It cannot ride on `col`: that sum is divided by the same weight it was
    // gathered with, so any scale folded into it cancels exactly.
    //
    // And it is read POINTWISE, at this fragment's own perimeter position,
    // rather than gathered like the hue - for the same reason emitCover is.
    // The gather weight 1/(dd + kc2) is a Lorentzian with 1/d^2 tails, so a
    // gathered alpha is a ring-wide weighted mean: a half-perimeter faded to
    // 0 still measured 0.44 of full brightness at its own midpoint, dragged
    // up by the opaque far side, while the opaque half was dragged down. The
    // pointwise read is exact at every position and needs no normalisation.
    //
    // Alpha 0 therefore kills the filament, halo and bloom together at that
    // position, and the premultiplied output alpha (peak channel, bottom of
    // main) follows for free, so the background shows through rather than
    // being occluded by a black tube.
    float baseAlphaPt = texture(uGradientLUT,
                                vec2(sPos - uTime * uHueRotationRate, 0.5)).a;
    // Winner-take-all across arcs, as documented for overlap. This can be a
    // plain max() again because arcCoverContinuous now reaches a FULL 1.0 at an
    // abutting endpoint rather than 0 (inward) or 0.5 (straddling), so two arcs
    // tiling the ring hand over at max(w1, w2) with no notch - and because
    // their ramps overlap, the handover stays smooth even when w1 != w2.
    float emitCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF,
                                     arcTailAbuts(arc.w), arcHeadAbuts(arc.w));
        if (c <= 0.0) continue;                           // does not reach here
        // Each arc's own alpha, from the same LUT its colour came from and in
        // the same coordinate space the gather used - arc-local for hasStops,
        // perimeter space otherwise.
        float aA;
        if (arcHasStops(arc.w)) {
            // NO hue-rotation term here, unlike the base-gradient path above.
            // uArc is the arc's OWN head-to-tail coordinate, not a position on
            // the perimeter ring, so there is nothing for a rotation to rotate:
            // subtracting uTime * rate just slid the gradient off one end, and
            // the atlas wrap brought the tail colour back round to butt against
            // the head mid-edge with no geometric feature to hide the seam.
            // Segments never had the term, and this is what makes arcs match
            // them. An arc's gradient moves by moving the arc (Arc::start) or
            // by animating its stops.
            //
            // WRAPPED, exactly as arcCoverContinuous wraps its own rel. An arc
            // may straddle the seam (start 0.8 + length 0.4 is legal - see
            // Arc::start), and coverage already handles that, so a plain
            // sPos - start would go NEGATIVE past the seam and CLAMP_TO_EDGE
            // would pin the whole wrapped remainder to the head colour. The
            // arc stayed lit and lost its gradient.
            //
            // The midpoint split is the other half: an outward tail feather
            // reaches BEHIND the start, and those fragments want a small
            // negative rel (clamping to the head) rather than one wrapped up
            // to near 1 (clamping to the tail). Same threshold as the coverage
            // feather so the two cannot disagree.
            float rowY = (float(a) + 0.5) / float(MAX_ARCS);
            float rel  = sPos - arc.x;
            rel       -= floor(rel);                       // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + arc.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(arc.y, 1e-4);
            aA         = texture(uArcLUT, vec2(uArc, rowY)).a;
        } else {
            aA = baseAlphaPt;
        }
        emitCover = max(emitCover, c * arc.z * aA);
    }

    // Segment coverage at this fragment's own perimeter position. This is the
    // segments' whole magnitude now: boost * bell, straight off the analytic
    // gaussian, so it cannot inherit either the gather's sample stepping or
    // the far-side dilution that used to make a segment dimmer on a small
    // rect. Segments emit where no arc covers, so they carry their own
    // filament/halo/bloom.
    float segCoverPt = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);                     // wrap to [-0.5, 0.5]
        float e   = rel * seg.y;
        // Per-segment alpha, pointwise - see emitCover above. Stop-less
        // segments inherit the base gradient's alpha, mirroring how their
        // colour falls back to segFallback in the gather.
        float sA;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            sA           = texture(uSegmentLUT, vec2(tLocal, rowY)).a;
        } else {
            sA = baseAlphaPt;
        }
        segCoverPt += seg.z * exp(-e * e) * sA;
    }
    float emitCoverAll = max(emitCover, min(segCoverPt, 1.0));

    // Attach the segments' magnitude to their hue. Unclamped on purpose: boost
    // above 1 must still brighten, as it did when the gather's `bell` carried
    // the magnitude. (emitCoverAll's min(.., 1.0) only bounds the shared
    // halo/bloom reach - it is not the segment's brightness.)
    vec3 segCol = segColHue * segCoverPt;

    // Sharp gate for the SDF-derived filament, from the same two pointwise
    // coverages. Both are exact at this fragment's perimeter position, so
    // neither can quantise a slow tracer's head to the gather points nor light
    // the corner preceding an arc's tail - the two bugs the old
    // circular-mean/sample-based gates had.
    float filamentGate = max(smoothstep(0.5, 1.0, min(segCoverPt, 1.0)), emitCover);

    // --- Analytic halo + bloom --------------------------------------------
    // Closed form of the sums this shader used to run over the perimeter
    // samples. For a locally straight emitter of unit density:
    //
    //   sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
    //   sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
    //
    // Peak values at ad = 0 are identical to the gather's (0.86 and 1.005), so
    // the NORM factors keep their calibration. Both are pure functions of the
    // SDF distance, so neither can bead and neither carries any dependence on
    // the sample spacing - which is what frees glowRadius to set the width
    // directly at any rect size.
    //
    // Known difference from the gather: on the concave side of a corner the
    // sum picked up both incident edges, so corners ran hotter (measured ~40%
    // at 25 px inside a 40 px corner with glowRadius 25). A nearest-distance
    // profile cannot reproduce that; the gap closes as glowRadius shrinks
    // relative to cornerRadius and is nil at cornerRadius 0.
    float halo  = HALO_NORM_FACTOR  * 2.0 * kh * kh / (ad * ad + kh * kh);
    float bloom = BLOOM_NORM_FACTOR * PI * bw / sqrt(ad * ad + bw * bw);

    // Pedestal-subtract the bloom so it reaches exactly zero at the draw
    // quad's edge. The 1/ad tail is heavy - at the quad edge it is still ~10%
    // of peak - so without this the quad has to be enormous, or the emission
    // gets visibly chopped. `reach` recomputes the CPU's uncapped quad-sizing
    // formula (see setupGeometry): a pure function of glowRadius, bloomStrength
    // and intensity, so the pedestal is size-invariant even where the outside
    // cutoff clamps the actual quad smaller - that path is masked by the cutoff
    // smoothstep anyway, and feeding it the clamped margin here would subtract
    // a huge pedestal and dim the whole band.
    //
    // Renormalised by peak/(peak - pedestal) so the value at ad = 0 is
    // unchanged and BLOOM_NORM_FACTOR keeps its calibration; the tail is
    // slightly compressed in exchange for going cleanly to zero.
    //
    // The halo needs no pedestal: it falls as 1/ad^2, so at the same distance
    // it is ~2e-4 of peak - already invisible.
    // `sigma` is the filament half-width from the block above, so the second
    // term is the same filament-reach floor setupGeometry applies - without it
    // the two disagree at small glowRadius (and `reach` hits 0 at glowRadius 0,
    // making the pedestal subtract the entire bloom).
    float reach     = max(uGlowRadius * GLOW_REACH_RADIUS_FACTOR *
                          (1.0 + uBloomStrength * uIntensity),
                          sigma * reachSigmas);
    float bloomPeak = BLOOM_NORM_FACTOR * PI;
    float bloomPed  = BLOOM_NORM_FACTOR * PI * bw / sqrt(reach * reach + bw * bw);
    bloom = max(bloom - bloomPed, 0.0) * (bloomPeak / max(bloomPeak - bloomPed, 1e-6));

    // glowRadius == 0 must read as "filament only", but an analytic profile at
    // radius 0 is a sub-pixel spike of FULL height rather than nothing, so
    // both layers fade in over glowRadius = [0, GLOW_GATE_FADE_PX]. Measured
    // against a fixed pixel width: gating against the sampleSpacing-derived
    // floor instead would re-couple brightness to the rect size. (The bloom is
    // gated too now - the gather's spacing floor used to keep it finite here.)
    float glowGate = clamp(uGlowRadius / GLOW_GATE_FADE_PX, 0.0, 1.0);

    // Compose: base arc x intensity + segments (independent of intensity, so
    // a segment stays lit even on a dark arc - the whole point of the
    // additive segment model).
    //
    // EACH SOURCE CARRIES ITS OWN COVERAGE. `col` is gated-normalised up in the
    // gather, which makes it a pure hue of unit magnitude EVERYWHERE on the
    // quad - it no longer decays with distance from the lit arc, because the
    // coverage that used to ride in it moved to emitCover. So it must be
    // multiplied by emitCover here. Summing the two sources first and applying
    // one shared gate (the old `lightCol * filamentGate`) let the segment's
    // gate lift the arc term on a stretch NO arc covers: a blue arc over half
    // the ring plus a red segment on the other half rendered the segment
    // magenta at ~2x brightness, and violet - arc-dominant - on its shoulders.
    //
    // The segment keeps the gates it already had, so nothing about the common
    // "tracer running along a lit arc" case moves: with an arc covering, both
    // emitFil and emitGlow reduce to exactly the old expression. Only the paths
    // where the two coverages DISAGREE change, which is the bug.
    vec3 arcCol = col * uIntensity;

    // filamentGate is the segment's SHARP gate (smoothstep 0.5..1) maxed with
    // emitCover; emitCoverAll is the soft one. Applied to segCol only - the arc
    // takes emitCover directly in both, since for an arc the two gates were
    // just emitCover anyway.
    vec3 emitFil  = arcCol * emitCover + segCol * filamentGate;
    vec3 emitGlow = arcCol * emitCover + segCol * emitCoverAll;

    vec3 result  = emitFil  * core  * FILAMENT_GAIN  * lineGate;
    result      += emitGlow * halo  * HALO_GAIN      * glowGate;
    result      += emitGlow * bloom * uBloomStrength * glowGate;

    // --- One-sided cut: mask the WHOLE emission at the line ----------
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Hard cutoff soft masks: fade the emission over the per-side
    // softness on each side of the [-uInsideCutoff, +uOutsideCutoff] band so
    // bloom/halo never punch past the stated reach. Feather is symmetric
    // around the boundary so the mid-boundary sample sees ~50% weight.
    // Disabled sides push their boundary to a huge sentinel, so the
    // smoothstep naturally evaluates to a pass-through 1.0.
    // In the band means: outside the shrunk rect (dIn >= 0) and inside the
    // grown rect (dOut <= 0). See bandOuterDistance / bandInnerDistance.
    result *= smoothstep(-inSoft, inSoft, dIn);
    result *= 1.0 - smoothstep(-outSoft, outSoft, dOut);

    // --- Quad-edge fade: the draw quad ends uQuadMargin past the rect ON EACH
    // AXIS. Fade the emission to zero over the last stretch so a strong bloom
    // never shows a hard rectangular cutoff where the quad clips it. Interior
    // pixels sit far inside the quad, so they're unaffected.
    //
    // Measured PER-AXIS via dQuad, NOT from the Euclidean d. What this fade
    // hides is the quad, and the quad is a rectangle; keying on d put the ramp
    // on a rounded contour that ate the corners early:
    //
    //   - At cornerRadius 0 the band is per-axis too (see bandOuterDistance),
    //     so it reaches d = sqrt(2) * outsideCutoff at a corner while
    //     setupGeometry clamps uQuadMargin to cutoff + softness + 1. On the
    //     stock 800x600 rect at cutoff 12 the band ran out to d = 16.97 but the
    //     d-keyed ramp was already zero by d = 14, erasing the outer ~30% of
    //     every corner - while black-rect.frag, drawn on a fullscreen quad with
    //     no fade at all, kept its square corner. That is exactly the black
    //     bulge past the glow the r == 0 branch exists to prevent.
    //   - With the cutoff disabled the same rounding chopped the bloom at
    //     d = uQuadMargin though the quad ran on to its own corner, so corners
    //     faded sooner than edges for no reason.
    //
    // dQuad is 0 on the quad edge and negative inside, so the ramp runs over
    // [-(uQuadMargin - fadeStart), 0]. On a straight edge dQuad == d -
    // uQuadMargin, so that stretch is bit-identical to the old expression.
    //
    // fadeStart is unchanged. The ramp must not begin INSIDE the outside
    // cutoff, or it dims the band's outer edge before the cutoff mask above
    // ever gets there - and only on the exterior, because the interior half of
    // a symmetric band never reaches the quad. An opaque-INSIDE vs
    // opaque-OUTSIDE pair sharing an outer rect is what exposes it: at cutoff
    // 12, uQuadMargin is 13, so a bare fraction of the margin started the ramp
    // at 10.4 and left the outermost band pixel at ~0.66 of its mirrored
    // counterpart (15.3 vs 23.3 on the right edge, same ratio on the other
    // three). Flooring the start at the cutoff boundary hands everything up to
    // that point back to the cutoff smoothstep.
    //
    // Only when that boundary actually falls inside the quad, though. A
    // disabled cutoff arrives as a huge sentinel, and the whole point of this
    // fade is the case where uQuadMargin is SMALLER than outsideCutoff - in
    // both, flooring would push the start to or past uQuadMargin. Those keep
    // the unfloored start, so fadeStart < uQuadMargin always holds and the ramp
    // width below is strictly positive (an inverted smoothstep is undefined in
    // GLSL).
    float fadeFloor = uQuadMargin * QUAD_FADE_START_FRAC;
    float cutEdge   = uOutsideCutoff + outSoft;
    float fadeStart = (cutEdge < uQuadMargin) ? max(fadeFloor, cutEdge) : fadeFloor;
    float dQuad     = sdRoundBox(vPos, halfSize + vec2(uQuadMargin), 0.0);
    result *= 1.0 - smoothstep(-(uQuadMargin - fadeStart), 0.0, dQuad);

    // --- Grade --------------------------------------------------------
    // Hue-preserving Reinhard: tonemap the peak channel and scale the
    // others by the same ratio. Per-channel tonemap desaturates warm mixes
    // (orange -> peach) because R saturates while G/B are still linear;
    // scaling by the peak's compression preserves the original R:G:B ratio.
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output so the effect composites over arbitrary
    // background objects instead of only adding light. Coverage = brightest
    // channel: the hot filament core (alpha ~ 1) occludes the background and
    // reads as a solid tube; the dim halo/bloom (alpha ~ 0) stay additive; the
    // dark surround (alpha = 0) leaves the background untouched. Pairs with
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
