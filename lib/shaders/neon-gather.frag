precision highp float;

// EMISSION PRE-PASS (experimental; OptimizedNeonConfig::emissionPrePass).
//
// The 128-sample gather from neon.frag and nothing else, run at `prePassScale`
// and writing the four quantities that gather produces:
//
//   RT0 = vec4(col.rgb,       haloCover)
//   RT1 = vec4(segColHue.rgb, bloomCover)
//
// These four can be computed at reduced resolution because every one of them
// is a convolution and is therefore band-limited by construction - `col` by
// kc, haloCover by kh, bloomCover by bw. Nothing sharp is produced here. The
// filament core, the analytic halo/bloom radial profiles, the cutoffs,
// glowSide and the grading all stay at FULL resolution in
// neon-composite.frag. That split is the whole point, and it is what
// separates this from resolutionScale, which scales the sharp parts down too
// and then spends a blit pass compensating for having done so.
//
// The safe prePassScale is set by the NARROWEST kernel above - kc (which is
// perimeter * COLOR_BLEND_PERIM_FRAC, so it shrinks with the rect) and kh
// (= glowRadius). Below roughly two pre-pass texels per kernel width the
// coverage starts to step at arc ends.
//
// UNITS: everything here is FULL-RES px. The quad is drawn with a full-res
// ortho projection into a smaller viewport, so vPos interpolates to the same
// rect-local pixel coordinates at any prePassScale - there is no
// uResolutionScale anywhere in this shader.

in vec2 vPos;
layout(location = 0) out vec4 outHueHalo;   // rgb = base hue,    a = haloCover
layout(location = 1) out vec4 outSegBloom;  // rgb = segment hue, a = bloomCover

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
// same layout convention as uSegmentLUT. Sampled only when the winning arc's
// hasStops flag is set; the loser rows are never read, so leaving them stale
// is fine.
uniform sampler2D uArcLUT;

// 1-row 2D LUT (REPEAT-wrapped) holding the precomputed colour ring.
// Replaces the in-shader sampleStops loop + HSV blend on the hot path.
// GLES 3.0 does not support sampler1D, so we use a 1-row 2D texture.
uniform sampler2D uGradientLUT;

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform int uNumSamples; ///< Live gather sample count (optimizedNeon.numSamples).

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
    if (r > BAND_SHARP_CORNER_EPSILON) { return d - cut; }
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

// Fractional [0, 1] membership of the gather sample at perimeter position
// @c si in the arc [start, start+length]. Length 0 = empty, length 1 = full
// (start becomes an irrelevant phase); in between it is a wrap-aware range
// over the unit circle.
//
// SCOPE: this shapes the COLOUR GATHER ONLY. It picks the winner-take-all arc
// at each sample and weights that sample's contribution to the hue average, so
// adjacent arcs of different colours crossfade at a seam instead of snapping.
// It does NOT reach brightness or reach any more: `col` divides by the same
// arc-gated weight it accumulates, so this value cancels out of the ratio. The
// visible extent of an arc - filament, halo and bloom alike - comes solely from
// arcCoverContinuous below, whose feather is INWARD and measured in pixels.
//
// Wrap-aware via testing both @p si and @p si + 1 and taking the max. When the
// arc extends past 1.0, a sample near position 0 is physically close to `end`
// through the perimeter loop, and the virtual @p si + 1 test picks that up;
// the same expression handles the non-wrap case because @p si + 1 always falls
// outside a sub-unit arc there. Without it the hue would break discontinuously
// at position 0 for any arc straddling the wrap point.
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    // Feather sits OUTSIDE the arc on each side (the ramp only extends
    // outward), so the sample exactly at `start` / `end` still carries full
    // weight in the hue average.
    //
    // The widths are ASYMMETRIC, and both now buy hue-blend behaviour only:
    //   - HEAD (end side): one full sample, so adjacent samples' fade-in
    //     ranges are contiguous and a growing arc's leading hue hands over
    //     smoothly from one gather point to the next.
    //   - TAIL (start side): a quarter sample - near-hard, so the arc's own
    //     hue takes over immediately at the start rather than being averaged
    //     with whatever precedes it.
    // (Both used to be about how far halo/bloom spilled past the arc ends;
    //  that job moved to arcCoverContinuous when `col` became a pure hue.)
    float fHead = invNumSamples;
    float fTail = 0.25 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - fTail, start, si);
    float g2a = 1.0 - smoothstep(end, end + fHead, si);
    float g1b = smoothstep(start - fTail, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + fHead, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

// Continuous [0,1] coverage of a fragment for the arc [start, start+length],
// used to gate the sharp SDF filament. INWARD FEATHER: the smooth ramps sit
// INSIDE the arc's own perimeter span, so nothing outside the arc gets lit.
// This trades a small visible inset (arc starts at start+fTail and ends at
// start+length-fHead) for two hard-won properties:
//   - No bleed onto adjacent edges past corners: coverage is exactly 0 for
//     any fragment whose perimeter position falls outside [start, start+length].
//   - Smooth, isotropic endpoints: the fade profile is a plain smoothstep in
//     the perimeter parameter, so it reads the same shape whether the endpoint
//     sits on a straight edge or right at a corner.
//
// Feather widths are perimeter fractions (pixel-space widths / current perimeter,
// converted at the call site).
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // wrap to [0, 1): distance past start
    // Cap each feather at a share of the arc's own length. The widths arrive
    // as a fixed pixel span but `length` is a perimeter FRACTION, so on a small
    // rect a short arc can be narrower than the two ramps combined - they then
    // overlap and clip the peak, making the same arc config dimmer on a smaller
    // rect (0.21 vs 1.00 for L = 0.02 at 200x150 vs 800x600). Capping keeps the
    // peak at 1.0 for any length at any size, and leaves long arcs untouched.
    float cap    = length * ARC_FEATHER_MAX_SHARE;
    float fH     = min(fHead, cap);
    float fT     = min(fTail, cap);
    // Tail ramps IN from 0 at rel = 0 (start) to 1 at rel = fT.
    float tailIn = smoothstep(0.0, fT, rel);
    // Head ramps OUT from 1 at rel = length - fH to 0 at rel = length.
    float headIn = 1.0 - smoothstep(length - fH, length, rel);
    // Fragments past `length` in perim get headIn = 0 -> coverage = 0 (no bleed).
    // Fragments behind start wrap to rel near 1 -> also headIn = 0 -> coverage = 0.
    return tailIn * headIn;
}

// ---------------------------------------------------------------------------

void main() {    // Perimeter of the rounded rect, in px. Used twice below: to size the
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
    float kh2 = kh * kh;
    float bw2 = bw * bw;

    // Inward arc feathers as perimeter fractions. Computed HERE, above the
    // gather, because the coverage gather below reads arcCoverContinuous at
    // every sample; the pointwise read further down uses the same two values.
    // Both sides are full-res px here, so the constants are used raw;
    // neon-optimized.frag scales them by uResolutionScale because its `peri` is
    // in FBO px - keep the two in step when tuning them.
    float headF = HEAD_FEATHER_PX / peri;
    float tailF = TAIL_FEATHER_PX / peri;

    // --- Colour gather -----------------------------------------------------
    // This loop now gathers COLOUR ONLY - the halo and bloom intensities are
    // computed in closed form after it. Per iteration: 1 UBO read for the
    // sample position, 1 sub, 1 dot, 1 reciprocal, 1 gradient-LUT lookup, plus
    // one exp() per active segment boost (skipped entirely when
    // uSegmentCount == 0). No pow(), no in-shader stops walk, no HSV math.
    // Sweep advance is folded into the GL_REPEAT-wrapped LUT - no fract().
    vec3  acc       = vec3(0.0); // base colour * arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour * bell * gather weight
    float wsumLit   = 0.0; // sum ARC-GATED g     - normalises `col` (see below)
    float wsumSegW  = 0.0; // sum SEGMENT bell*g  - normalises the segment hue

    // --- Coverage gather (see the note above the halo/bloom composition) ---
    // The halo and bloom MAGNITUDES are closed forms of the SDF distance, but
    // their coverage cannot be: read pointwise at this fragment's own nearest
    // perimeter position it produces a hard cut along the normal at each arc
    // end, which the radial profile then extrudes into a slab with straight
    // sides. Coverage has to diffuse laterally with distance the way the old
    // per-sample sum did, so it is gathered here and NORMALISED by its own
    // weight - a 0..1 fraction, carrying no sample spacing and therefore no
    // rect-size dependence, unlike the intensity sums this replaced.
    //
    // Two kernels because the two layers have different reaches: smoothing the
    // narrow halo with the 6x-wider bloom kernel would bleed it far past its
    // own falloff. A full arc gives coverS = 1 at every sample, so both ratios
    // are exactly 1 and that case is unchanged.
    float haloCovAcc  = 0.0;
    float haloCovW    = 0.0;
    float bloomCovAcc = 0.0;
    float bloomCovW   = 0.0;

    // Dynamic bound so the perf slider reduces real work; the UBO array stays
    // sized to NEON_MAX_LOOP_SAMPLES. Not const - it comes from a uniform.
    int numSamples = clamp(uNumSamples, 1, NEON_MAX_LOOP_SAMPLES);
    float invNumSamples = 1.0 / float(numSamples);

    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (sample index i advances in the winding direction; the
    // REPEAT-wrapped LUT handles the resulting negative coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = invNumSamples;
    float si   = 0.0; // sample's normalised perimeter position

    for (int i = 0; i < numSamples; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        // One vector reciprocal for all three kernels (colour, halo, bloom)
        // instead of three scalar divides - measured ~1.5% off the whole pass.
        vec3  wv  = 1.0 / (vec3(dd) + vec3(kc2, kh2, bw2));
        float g   = wv.x;

        // Arc winner-take-all: find the arc with the largest effective mask
        // (arcInside * intensity) at this sample. That arc owns both the
        // emission mask (arcW) and the colour at this sample. Because
        // arcInside is smoothstepped one-sample-wide at each end, adjacent
        // arcs of different colours crossfade smoothly at the seam.
        float bestMask = 0.0;
        int   bestIdx  = -1;
        // coverS is the EMISSION coverage at this sample, and it deliberately
        // uses arcCoverContinuous rather than the arcInside mask beside it.
        // arcInside's feather is one sample wide and sits OUTSIDE the arc - fine
        // for crossfading hues, but as an emission mask it quantises a growing
        // arc's head to the gather points and spills the tail into the corner
        // BEFORE the arc starts. arcCoverContinuous is px-based and inward, so
        // the gathered coverage keeps the same ends the filament has.
        float coverS = 0.0;
        for (int a = 0; a < uArcCount; a++) {
            vec4  arc  = uArcs[a];
            float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;
            if (mask > bestMask) {
                bestMask = mask;
                bestIdx  = a;
            }
            coverS = max(coverS,
                         arcCoverContinuous(si, arc.x, arc.y, headF, tailF) * arc.z);
        }
        float arcW = bestMask;
        float lg   = g * arcW;

        // Winner's colour at this sample. Two cases:
        //  - hasStops: the arc has its own gradient. Sample it in ARC-LOCAL
        //    space so position 0 is the arc's start and position 1 is its end.
        //    The uTime term still scrolls the LUT (REPEAT wrap) at the global
        //    hueRotationRate so hue rotation reads as the gradient marching
        //    through the arc window, not as position offsetting.
        //  - empty stops: fall back to the base gradient IN PERIMETER SPACE
        //    (ti = perimeter position + time offset) so the arc stays visually
        //    continuous with the rest of the perimeter.
        // bestIdx < 0 means every arc had 0 mask here - contributes nothing.
        vec3 baseColI;
        vec3 segFallback;
        if (bestIdx >= 0) {
            vec4 winner = uArcs[bestIdx];
            if (winner.w > 0.5) {
                float rowY  = (float(bestIdx) + 0.5) / float(MAX_ARCS);
                float uArc  = (si - winner.x) / max(winner.y, 1e-4);
                uArc       -= uTime * uHueRotationRate; // match base sign convention
                baseColI    = texture(uArcLUT, vec2(uArc, rowY)).rgb;
            } else {
                baseColI = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
            }
            // Stop-less segments over an arc inherit that arc's colour.
            segFallback = baseColI;
        } else {
            // No arc covers this sample: the arc emission is black, but a
            // stop-less segment still lights here, inheriting the base
            // perimeter gradient so the whole ring stays hue-continuous.
            baseColI    = vec3(0.0);
            segFallback = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
        }
        acc     += baseColI * lg;
        // GATED normalisation, and it is the point. Dividing by the same weight
        // the numerator was gathered with makes `col` a pure hue of unit
        // magnitude: it carries no coverage and no per-arc intensity, both of
        // which cancel. Those reach the emission solely through emitCover /
        // filamentGate below, which are px-based and size-invariant. segAcc /
        // wsumSegW does the identical thing for the segment hue.
        //
        // Both used to divide by an UNGATED sum over every sample, so an unlit
        // far side of the ring dragged the lit colour toward black by roughly
        // kc / rectHeight. With kc pinned to a fixed px span that ratio grew as
        // the rect shrank: a quarter-perimeter arc measured 0.79 of full
        // brightness at 200x150 against 0.97 at 1920x1080. Gated normalisation
        // is exactly 1.0 at every size. Nothing is lost because these no longer
        // need to encode coverage - they did back when the gather also produced
        // the emission, but the analytic halo/bloom and the pointwise coverages
        // replaced that.
        wsumLit += lg;

        // --- Travelling segments (independent additive lights) ---
        // Gathered with the raw proximity weight `g`, NOT the arc-gated `lg`,
        // so a segment lights even on perimeter stretches no arc covers.
        // Composed outside uIntensity so segments stay lit even at intensity 0.
        // Skipped whole-loop when uSegmentCount == 0.
        //
        // The gather produces the segment HUE only - same split as the arcs.
        // Its magnitude (boost * bell) comes from segCoverPt, evaluated
        // pointwise at this fragment's own perimeter position further down.
        float segCovS = 0.0; // this sample's segment coverage, for the gather below
        for (int s = 0; s < uSegmentCount; s++) {
            vec4  seg     = uSegments[s];
            // Signed wrap-distance along the perimeter in [-0.5, 0.5]. The
            // shader uses it for both the bell weight (magnitude) and the
            // head-to-tail sampling within the segment's own gradient (sign).
            float rel     = si - seg.x;
            rel          -= floor(rel + 0.5);            // wrap to [-0.5, 0.5]
            float e       = rel * seg.y;                 // normalise by invSigma
            float bell    = seg.z * exp(-e * e);         // boost * gaussian
            segCovS      += bell;                        // matches segCoverPt's definition
            if (bell < 0.005) continue;                  // cheap early-out for distant fragments/segments

            // Colour: own stops from row `s` of uSegmentLUT if hasStops set,
            // else inherit segFallback (the arc's colour where an arc covers,
            // the base gradient where none does).
            vec3 segColor;
            if (seg.w > 0.5) {
                // tLocal: 0 at seg head (rel = -1/invSigma), 1 at seg tail. e
                // already normalises rel by invSigma, so clamp/rescale.
                float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
                float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
                segColor     = texture(uSegmentLUT, vec2(tLocal, rowY)).rgb;
            } else {
                segColor = segFallback;
            }
            segAcc   += segColor * bell * g;
            wsumSegW += bell * g;                        // gated denominator - cancels bell out of the hue
        }

        // Coverage gather. Segments are clamped so stacked ones cannot push the
        // shared halo/bloom reach past a single light's; the arc term already
        // carries per-arc intensity through coverS.
        float coverAll = max(coverS, min(segCovS, 1.0));
        haloCovAcc  += coverAll * wv.y;
        haloCovW    += wv.y;
        bloomCovAcc += coverAll * wv.z;
        bloomCovW   += wv.z;

        ti  += dti;
        si  += dti;
    }

    // Normalised 0..1 coverage fractions. Numerator and denominator run over
    // the same samples with the same weights, so the sample density cancels
    // exactly - this is what keeps them free of the sampleSpacing dependence
    // the old intensity sums had, and therefore size-invariant.
    float haloCover  = haloCovAcc  / max(haloCovW,  WSUM_EPSILON);
    float bloomCover = bloomCovAcc / max(bloomCovW, WSUM_EPSILON);

    // Both are pure hues of unit magnitude now; the magnitudes are attached
    // below from the pointwise coverages.
    vec3 col       = acc    / max(wsumLit,  WSUM_EPSILON); // base perimeter hue
    vec3 segColHue = segAcc / max(wsumSegW, WSUM_EPSILON); // segment hue

    outHueHalo  = vec4(col,       haloCover);
    outSegBloom = vec4(segColHue, bloomCover);
}
