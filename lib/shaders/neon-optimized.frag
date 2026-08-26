precision highp float;

// Precision: highp (NOT mediump). This renderer's "optimized" wins come from
// the half-res FBO, reduced gather sample count, data-texture sample lookup
// and the baked colour LUT - NOT from mediump. On desktop GLES (ANGLE on
// Windows) mediump maps to fp16, whose 65504 max and ~11-bit mantissa can't
// hold the fragment coordinates: the draw quad extends hundreds of px past
// the rect, so the interpolated vPos and dot(dv, dv) overflow/quantise and
// the gather divides go 0/0 = NaN, rasterising as scattered colour "noise
// dots". highp is mandatory for fragment shaders in GLES 3.0 (#version 300
// es), so this is safe on every 3.0 target; the mediump ALU savings weren't
// worth the precision breakage.

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
// (Far culling lives on the CPU: the Pass-1 quad is sized to rect + glowReach,
//  so there's no per-fragment discard here. See neon-optimized-renderer.cpp.)

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
uniform float uInsideCutoff;          ///< Positive scaled-px distance INSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel * scale CPU-side.
uniform float uInsideCutoffSoftness;  ///< Feather width (scaled px) at the inside cutoff boundary.
uniform float uOutsideCutoff;         ///< Positive scaled-px distance OUTSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel * scale CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width (scaled px) at the outside cutoff boundary.
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).

// FBO px per full-res px (Config::OptimizedNeonConfig::resolutionScale). Every
// pixel-valued uniform above already arrives pre-multiplied by it, so the SDF
// distance `d`, uRectSize and friends are all in FBO space. The tuning
// constants injected from neon-tuning.h are NOT - they are authored in full-res
// px - so anything comparing a raw constant against an FBO-space quantity must
// convert it with this factor. The base NeonRenderer passes 1.0.
uniform float uResolutionScale;

// Distance (in scaled/FBO px, from the rect edge) to the draw quad's edge.
// The emission is faded to zero just before this so a tight quad never shows
// a hard rectangular cutoff where a strong bloom is clipped.
uniform float uQuadMargin;

// Loop sample positions (perimeter points, scaled to FBO space) as a std140
// uniform block. Each entry is (x, y, 0, 0); std140 pads vec2 to 16 bytes so
// we store as vec4 and read .xy. Sized to the shader-side max; the actual
// count in use is uNumSamples (driven by optimizedNeon.numSamples on the CPU
// side), so a smaller slider value really does iterate less.
layout(std140) uniform LoopSamplesBlock
{
    vec4 uLoopSamples[NEON_MAX_LOOP_SAMPLES];
};
uniform int uNumSamples; ///< Actual sample count in use; 1..NEON_MAX_LOOP_SAMPLES.

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes array elements one at a time into the block's UBO
// ("uSegments[0]", ...) at the std140 stride. On desktop GL the block is fed
// from a UBO in neon-optimized-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

// Per-segment gradient atlas (RGBA8, CLAMP both axes). One row per segment,
// each row baked head-to-tail across its span. Sampled only when the
// segment's hasStops flag is set.
uniform sampler2D uSegmentLUT;

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). Overlap resolves winner-take-all;
// see neon.frag for full rationale.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Per-arc gradient atlas (RGBA8, CLAMP both axes). One row per arc, same
// convention as uSegmentLUT. Sampled only when the winning arc has stops.
uniform sampler2D uArcLUT;

// Perimeter emission table from neon-emission.frag: uNumSamples wide (of
// NEON_MAX_LOOP_SAMPLES allocated), 2 tall. Row 0 = (arcColour*arcW, arcW),
// row 1 = (SUM(segColour*bell), SUM(bell)). texelFetch only - adjacent texels
// are unrelated samples. Shared verbatim with neon.frag's consumer.
uniform sampler2D uEmission;

// 1-row 2D LUT (REPEAT-wrapped) holding the precomputed colour ring.
// Replaces the in-shader sampleStops loop + HSV blend on the hot path.
// GLES 3.0 does not support sampler1D, so we use a 1-row 2D texture.
uniform sampler2D uGradientLUT;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
// CLOCKWISE / COUNTER_CLOCKWISE). Replaces the proximity-weighted circular
// mean of the sample angles - near a corner those phases wrap through 2*pi
// right into the arc's start, smearing the whole corner curve to ~0 and
// lighting it for any arc that begins at position 0. The geometric inverse
// reads the nearest perimeter point directly, so corner and edge fragments get
// their true positions. Works in scaled FBO space because it only uses the
// (already scaled) uRectSize / uCornerRadius. See neon.frag for the details.

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

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

// NOTE: arcInside() moved to neon-emission.frag with the rest of the
// per-sample work - it was a pure function of (si, config). See neon.frag.

// Continuous [0,1] arc coverage - INWARD FEATHER. See neon.frag for the full
// rationale; the shape here is identical.
// uArcs[].w is a BITMASK: bit 0 = hasStops, bit 1 = tail abuts another arc,
// bit 2 = head abuts. Packed CPU-side in packLightBlocks; picks each endpoint's
// feather direction below. See neon.frag for the full note.
bool arcHasStops(float flags)  { return mod(flags, 2.0) >= 0.5; }
bool arcTailAbuts(float flags) { return mod(floor(flags * 0.5), 2.0) >= 0.5; }
bool arcHeadAbuts(float flags) { return mod(floor(flags * 0.25), 2.0) >= 0.5; }

float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail,
                         bool tailAbuts, bool headAbuts) {
    if (length >= 1.0 - 1e-6) return 1.0;
    if (length <= 1e-6)       return 0.0;
    float rel = sPos - start;
    rel -= floor(rel);
    // Feather direction is per ENDPOINT. A free endpoint ramps INWARD, so
    // nothing outside the arc is lit - critical at cornerRadius 0, where a
    // sharp corner's whole exterior quadrant shares one sPos and any non-zero
    // coverage there floods the quadrant. An abutting endpoint ramps OUTWARD
    // and reaches 1.0 at the endpoint, so two arcs tile with no seam notch;
    // that bleed lands inside an already-lit neighbour. See neon.frag.
    if (tailAbuts && rel > 0.5 * (1.0 + length)) { rel -= 1.0; }
    // Feathers capped at a share of the arc length so the two ramps cannot
    // overlap and clip the peak on a short arc - the pixel-span feather vs
    // fraction-based length mismatch that made the same arc dimmer on a
    // smaller rect. See neon.frag / neon-tuning.h.
    float cap    = length * ARC_FEATHER_MAX_SHARE;
    float fH     = min(fHead, cap);
    float fT     = min(fTail, cap);
    float tailIn = tailAbuts ? smoothstep(-fT, 0.0, rel)
                             : smoothstep(0.0, fT, rel);
    float headIn = headAbuts ? 1.0 - smoothstep(length, length + fH, rel)
                             : 1.0 - smoothstep(length - fH, length, rel);
    return tailIn * headIn;
}

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far-exterior discard (ad > glowReach) that the
    // standard NeonRenderer uses is intentionally absent here. The Pass-1
    // quad is sized on the CPU to exactly rect + glowReach, so geometry culls
    // the far region instead - friendlier to tile-based mobile GPUs, which
    // pay a hidden-surface-removal penalty for any discard in the shader.
    //
    // The one-sided cuts below stay as discards: they cull a useful half of
    // the band (real work saved) and the quad can't express that shape.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // Hard geometric cutoffs (see neon.frag for the full rationale). Sizes
    // and softness arrive pre-scaled into FBO space to match `d`. Disabled
    // sides collapse to a huge sentinel * scale CPU-side, so these
    // branches no-op.
    float inSoft  = max(uInsideCutoffSoftness,  SIDE_SOFT_EPSILON);
    float outSoft = max(uOutsideCutoffSoftness, SIDE_SOFT_EPSILON);
    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners instead of being rounded by the cut distance.
    float dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, uOutsideCutoff);
    float dIn  = bandInnerDistance(d, uInsideCutoff);
    if (dOut >  outSoft) discard;
    if (dIn  < -inSoft ) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile with exponentially smooth falloff (matches
    // the base NeonRenderer so the two look identical):
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
    // The Gaussian has no power-law tail, so the filament reads as a clean
    // thin line with a naturally smooth roll-off.
    //
    // Peak at ad = 0 is always exactly 1.0.
    //
    // lineGate fades the filament from 0 at lineWidth = 0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth = 0 means "no
    // line" instead of a single-pixel bright dot.
    //
    // Both the sigma floor and the gate threshold are full-res px constants
    // compared against the FBO-space uLineWidth, so they carry
    // uResolutionScale. Without it a half-res pass floors sigma at 1 full-res
    // px (thickening thin lines) and reads lineWidth as half its real value
    // (dimming them) - the optimized output no longer matched the base.
    float minHalf   = FILAMENT_MIN_HALF_WIDTH * uResolutionScale;
    float halfWidth = uLineWidth * 0.5;
    float sigma     = max(halfWidth, max(minHalf, 1e-3));
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
    float lineGate  = clamp(uLineWidth / max(minHalf * 2.0, 1e-3), 0.0, 1.0);

    // Perimeter of the rounded rect, in FBO px (uRectSize is pre-scaled).
    // Used twice below: to size the colour-gather kernel, and to convert the
    // arc feathers from px to perimeter fractions.
    float r    = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;

    // --- Kernel widths ------------------------------------------------
    // kc is the COLOUR gather weight, sized as a fraction of the perimeter
    // because the gradient LUT it filters is indexed by perimeter fraction;
    // kh / bw are the EMISSION widths and take raw glowRadius, because the halo
    // and bloom are evaluated analytically from the SDF distance below and a
    // closed form cannot bead. See neon.frag for the full rationale.
    //
    // COLOR_BLEND_PERIM_FRAC is unit-free and `peri` is already FBO px, so
    // unlike FILAMENT_MIN_HALF_WIDTH this needs no uResolutionScale correction.
    // Deriving it from the shared NEON_MAX_LOOP_SAMPLES fraction rather than
    // from uNumSamples is also what keeps the two renderers in agreement: an
    // earlier sampleSpacing floor divided by the live sample count, so it moved
    // with the perf slider and landed 2x wider than the base renderer's.
    float kc  = max(peri * COLOR_BLEND_PERIM_FRAC, EMISSION_MIN_WIDTH);
    float kc2 = kc * kc;
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);

    // --- Colour gather -----------------------------------------------------
    // Gathers COLOUR ONLY; halo/bloom intensities are closed-form below.
    vec3  acc       = vec3(0.0); // base colour x arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour x bell x gather weight
    float wsumLit   = 0.0; // SUM ARC-GATED g     - normalises `col` (see neon.frag)
    float wsumSegW  = 0.0; // SUM SEGMENT bell*g  - normalises the segment hue

    // uNumSamples is dynamic so the perf slider actually reduces work; the
    // upper bound stays baked in the UBO array size so GL still knows the
    // maximum register pressure. The emission table is baked at the SAME
    // count (the renderer passes optimizedNeon.numSamples to the pre-pass), so
    // texel i here is sample i there.
    int n = uNumSamples;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kc2);

        // Per-sample emission from neon-emission.frag. Row 0 = the arc term
        // premultiplied by its own weight arcW, plus arcW for the denominator;
        // row 1 = the summed segment term. The arc scan and the segment loop
        // that used to run here are fragment-invariant and moved there. See
        // neon.frag and docs/emission-prepass.md.
        vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);
        vec4 e1 = texelFetch(uEmission, ivec2(i, 1), 0);

        acc      += e0.rgb * g;
        wsumLit  += e0.a   * g;
        segAcc   += e1.rgb * g;
        wsumSegW += e1.a   * g;
    }

    // Both pure hues of unit magnitude; magnitudes attach below from the
    // pointwise coverages. See neon.frag.
    vec3 col       = acc    / max(wsumLit,  WSUM_EPSILON); // base perimeter hue
    vec3 segColHue = segAcc / max(wsumSegW, WSUM_EPSILON); // segment hue

    // Continuous arc coverage for the filament: gate by the arc read at this
    // fragment's own perimeter position, recovered GEOMETRICALLY from vPos
    // (inverse of the CPU's GetPointOnRectangle) so a slow tracer's head moves
    // smoothly instead of stepping across the gather points. Feathers point
    // OUTWARD so the filament is lit at the arc start (no dark lead-in) and
    // reaches its end. The exact position also keeps the corner curve an arc
    // starting at 0 sits right after dark - the old circular-mean smear lit
    // the whole corner. See neon.frag for full rationale.
    float sPos = perimeterPosition(vPos);
    // Inward feathers: convert pixel widths to perimeter fractions. `peri` is
    // computed above the gather and is in FBO px (uRectSize is pre-scaled)
    // while HEAD/TAIL_FEATHER_PX are full-res px, so the constants carry
    // uResolutionScale - otherwise the filament's inset at each arc end grows
    // as 1/resolutionScale and the arc ends up visibly shorter than the base
    // renderer's, leaving a stretch of bare halo past the filament head/tail.
    float headF  = HEAD_FEATHER_PX * uResolutionScale / peri;
    float tailF  = TAIL_FEATHER_PX * uResolutionScale / peri;
    // One coverage, folding per-arc intensity in, driving the filament as well
    // as the halo and bloom: `col` is gated-normalised above, so intensity
    // cancels out of it and emitCover is what carries it now. See neon.frag.
    // Colour-stop alpha scales the emission magnitude, not the hue (the gated
    // normalisation would divide it back out of `col`), and is read POINTWISE
    // rather than gathered - a gathered alpha smears across the whole ring
    // through the weight's 1/d^2 tails. See neon.frag for the measurements.
    float baseAlphaPt = texture(uGradientLUT,
                                vec2(sPos - uTime * uHueRotationRate, 0.5)).a;
    // Winner-take-all - plain max() works again now that an abutting
    // endpoint reaches a full 1.0. See neon.frag.
    float emitCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF,
                                     arcTailAbuts(arc.w), arcHeadAbuts(arc.w));
        if (c <= 0.0) continue;
        float aA;
        if (arcHasStops(arc.w)) {
            // No hue-rotation term: uArc is arc-local, not perimeter space.
            // See neon.frag.
            float rowY = (float(a) + 0.5) / float(MAX_ARCS);
            float uArc = (sPos - arc.x) / max(arc.y, 1e-4);
            aA         = texture(uArcLUT, vec2(uArc, rowY)).a;
        } else {
            aA = baseAlphaPt;
        }
        emitCover = max(emitCover, c * arc.z * aA);
    }

    // Pointwise segment coverage at this fragment's perimeter position - the
    // segments' whole magnitude now (boost * bell off the analytic gaussian),
    // so it inherits neither the gather's sample stepping nor the far-side
    // dilution that made a segment dimmer on a small rect. Segments carry
    // their own filament/halo/bloom where no arc covers.
    float segCoverPt = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);
        float e   = rel * seg.y;
        // Per-segment alpha, pointwise; stop-less segments inherit the base
        // gradient's alpha. See neon.frag.
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

    // Magnitude attached to the segment hue. Unclamped: boost above 1 must
    // still brighten, as it did when the gather's `bell` carried it.
    vec3 segCol = segColHue * segCoverPt;

    // Filament gate from the same two pointwise coverages. See neon.frag.
    float filamentGate = max(smoothstep(0.5, 1.0, min(segCoverPt, 1.0)), emitCover);

    // --- Analytic halo + bloom --------------------------------------------
    // Closed form of the removed sums; peaks at ad = 0 match the gather's
    // exactly, so HALO_NORM_FACTOR / BLOOM_NORM_FACTOR keep their calibration.
    // Pure functions of the SDF distance: no beading, no sample spacing, and
    // therefore no dependence on rect size OR on the numSamples slider. See
    // neon.frag for the derivation and the corner-brightness caveat.
    float halo  = HALO_NORM_FACTOR  * 2.0 * kh * kh / (ad * ad + kh * kh);
    float bloom = BLOOM_NORM_FACTOR * PI * bw / sqrt(ad * ad + bw * bw);

    // Pedestal-subtract the bloom so it reaches exactly zero at the Pass-1
    // quad's edge, renormalised to keep the ad = 0 peak. `reach` recomputes the
    // CPU's uncapped quad-sizing formula from glowRadius / bloomStrength /
    // intensity - no rect size in it. uGlowRadius is already in FBO px and the
    // factor is dimensionless, so `reach` lands in FBO px alongside `ad` with
    // no uResolutionScale needed. See neon.frag for the full rationale.
    // Second term = the filament-reach floor setupGeometry applies. `sigma` is
    // already in FBO px (it carries uResolutionScale via minHalf), so the
    // product needs no further conversion. See neon.frag.
    float reach     = max(uGlowRadius * GLOW_REACH_RADIUS_FACTOR *
                          (1.0 + uBloomStrength * uIntensity),
                          sigma * reachSigmas);
    float bloomPeak = BLOOM_NORM_FACTOR * PI;
    float bloomPed  = BLOOM_NORM_FACTOR * PI * bw / sqrt(reach * reach + bw * bw);
    bloom = max(bloom - bloomPed, 0.0) * (bloomPeak / max(bloomPeak - bloomPed, 1e-6));

    // glowRadius == 0 -> filament only. An analytic profile at radius 0 is a
    // sub-pixel spike of full height, so both layers fade in over
    // glowRadius = [0, GLOW_GATE_FADE_PX]. uGlowRadius reaches this shader in
    // FBO px, so the full-res constant carries uResolutionScale - same
    // convention as FILAMENT_MIN_HALF_WIDTH.
    float glowGate = clamp(uGlowRadius / (GLOW_GATE_FADE_PX * uResolutionScale), 0.0, 1.0);

    // Base gates on uIntensity; segments are independent (stay lit at intensity 0).
    //
    // EACH SOURCE CARRIES ITS OWN COVERAGE - the arc takes emitCover, the
    // segment keeps the gates it already had. `col` is gated-normalised in the
    // gather, so it is a unit-magnitude hue everywhere and cannot be summed
    // into a shared gate: the segment's gate would then light the arc's hue on
    // a stretch no arc covers. See neon.frag for the full write-up and the
    // measured case. With an arc covering, both terms reduce to the previous
    // expression exactly.
    vec3 arcCol = col * uIntensity;

    vec3 emitFil  = arcCol * emitCover + segCol * filamentGate;
    vec3 emitGlow = arcCol * emitCover + segCol * emitCoverAll;

    vec3 result  = emitFil  * core  * FILAMENT_GAIN  * lineGate;
    result      += emitGlow * halo  * HALO_GAIN      * glowGate;
    result      += emitGlow * bloom * uBloomStrength * glowGate;

    // --- One-sided cut ---
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Hard cutoff soft masks: fade the emission over the per-side
    // softness on each side of the [-uInsideCutoff, +uOutsideCutoff] band so
    // bloom/halo never punch past the stated reach. Mirrors neon.frag.
    // Disabled sides push their boundary to a huge value so the smoothstep
    // naturally evaluates to a pass-through 1.0.
    // In the band means: outside the shrunk rect (dIn >= 0) and inside the
    // grown rect (dOut <= 0). See bandOuterDistance / bandInnerDistance.
    result *= smoothstep(-inSoft, inSoft, dIn);
    result *= 1.0 - smoothstep(-outSoft, outSoft, dOut);

    // --- Quad-edge fade: the Pass-1 quad ends uQuadMargin past the rect ON
    // EACH AXIS (all in scaled/FBO space). Fade the emission to zero over the
    // last stretch so a strong bloom never shows a hard rectangular cutoff
    // where the quad clips it - mirrors the base NeonRenderer so the two match.
    // Interior pixels sit far inside the quad.
    //
    // Measured PER-AXIS via dQuad, not from the Euclidean d: the fade hides the
    // quad and the quad is a rectangle, so a d-keyed ramp rounds off the
    // corners. At cornerRadius 0 that erased the outer part of the band's own
    // square corner (bandOuterDistance is per-axis there, and reaches
    // sqrt(2) * outsideCutoff while uQuadMargin is clamped to cutoff +
    // softness + 1), leaving black-rect.frag's fullscreen, unfaded fill bulging
    // past the glow. dQuad is 0 on the quad edge and negative inside; on a
    // straight edge dQuad == d - uQuadMargin, so that stretch is unchanged.
    // See neon.frag for the measurements.
    //
    // The ramp start is unchanged: it moves out to the outside-cutoff boundary
    // when that boundary falls inside the quad, so it cannot dim the band's
    // outer edge ahead of the cutoff mask - an asymmetry that hits only the
    // exterior, since the interior half of a symmetric band never reaches the
    // quad. Otherwise (cutoff disabled, or beyond the quad - the case this fade
    // exists for) the unfloored start stands; flooring there would collapse the
    // ramp to a hard step and invert the smoothstep.
    // QUAD_FADE_START_FRAC is a FRACTION of the margin, so unlike the px
    // constants it needs no uResolutionScale - and uQuadMargin, uOutsideCutoff,
    // outSoft and halfSize are all already in FBO px. See neon-tuning.h.
    float fadeFloor = uQuadMargin * QUAD_FADE_START_FRAC;
    float cutEdge   = uOutsideCutoff + outSoft;
    float fadeStart = (cutEdge < uQuadMargin) ? max(fadeFloor, cutEdge) : fadeFloor;
    float dQuad     = sdRoundBox(vPos, halfSize + vec2(uQuadMargin), 0.0);
    result *= 1.0 - smoothstep(-(uQuadMargin - fadeStart), 0.0, dQuad);

    // --- Grade --------------------------------------------------------
    // Hue-preserving Reinhard (see neon.frag for the rationale).
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output (coverage = brightest channel). Rendered into
    // the cleared-transparent half-res FBO with GL_ONE,GL_ONE_MINUS_SRC_ALPHA so
    // the FBO holds premultiplied colour + coverage, which the blit then
    // composites over background objects (core occludes, halo/bloom add).
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
