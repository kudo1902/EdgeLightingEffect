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
// (Far early-out lives on the CPU: the draw quad is sized to rect + earlyOut,
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
// DALi writes one element per registered property ("uSegments[0]", …) into
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
    // Tail ramps IN from 0 at rel = 0 (start) to 1 at rel = fTail.
    float tailIn = smoothstep(0.0, fTail, rel);
    // Head ramps OUT from 1 at rel = length - fHead to 0 at rel = length.
    float headIn = 1.0 - smoothstep(length - fHead, length, rel);
    // Fragments past `length` in perim get headIn = 0 -> coverage = 0 (no bleed).
    // Fragments behind start wrap to rel near 1 -> also headIn = 0 -> coverage = 0.
    return tailIn * headIn;
}

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far-exterior early-out is handled on the CPU - the draw quad is
    // sized to rect + earlyOut in NeonRenderer::setupGeometry, so geometry culls
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
    if (d >  uOutsideCutoff + outSoft) discard;
    if (d < -uInsideCutoff  - inSoft ) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile with exponentially smooth falloff:
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 → N = 1   (Laplace - heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 → N = 2   (Gaussian - pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 → N = 4   (platykurtic - flatter top, sharper shoulder)
    //   uFilamentFalloff = 5.0 → N = 10  (near-rectangular)
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
    float lineGate  = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

    // --- Kernel widths ------------------------------------------------
    // Two separate kernels, and the split is the point:
    //
    //  - kc is the COLOUR gather weight. The perimeter colour blend is still a
    //    128-sample sum, so it keeps the anti-bead floor - but the floor is a
    //    FIXED pixel span, not a multiple of the sample spacing. Spacing is
    //    perimeter / 128, so the old floor made the colour roll-off at an arc
    //    end track the rect size; COLOR_BLEND_MIN_PX holds it at one width for
    //    every geometry. See neon-tuning.h for the full rationale.
    //
    //  - kh / bw are the EMISSION widths: raw glowRadius, no floor. The halo
    //    and bloom are evaluated analytically from the SDF distance further
    //    down, and a closed form cannot bead however far apart the gather
    //    samples are, so glowRadius is proportional across its entire range.
    float kc  = max(uGlowRadius, COLOR_BLEND_MIN_PX);
    float kc2 = kc * kc;
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);

    // --- Colour gather -----------------------------------------------------
    // This loop now gathers COLOUR ONLY - the halo and bloom intensities are
    // computed in closed form after it. Per iteration: 1 UBO read for the
    // sample position, 1 sub, 1 dot, 1 reciprocal, 1 gradient-LUT lookup, plus
    // one exp() per active segment boost (skipped entirely when
    // uSegmentCount == 0). No pow(), no in-shader stops walk, no HSV math.
    // Sweep advance is folded into the GL_REPEAT-wrapped LUT - no fract().
    vec3  acc       = vec3(0.0); // base colour × arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour × bell × gather weight
    float wsumLit   = 0.0; // ∑ ARC-GATED g     - normalises `col` (see below)
    float wsumSegW  = 0.0; // ∑ SEGMENT bell*g  - normalises the segment hue

    // Compile-time constant loop bound: matches the LoopSamplesBlock UBO size
    // and the C++-side NEON_MAX_LOOP_SAMPLES so the compiler can unroll if it
    // wants to.
    const int numSamples = NEON_MAX_LOOP_SAMPLES;
    const float invNumSamples = 1.0 / float(numSamples);

    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (sample index i advances in the winding direction; the
    // REPEAT-wrapped LUT handles the resulting negative coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = invNumSamples;
    float si   = 0.0; // sample's normalised perimeter position

    for (int i = 0; i < numSamples; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kc2);

        // Arc winner-take-all: find the arc with the largest effective mask
        // (arcInside * intensity) at this sample. That arc owns both the
        // emission mask (arcW) and the colour at this sample. Because
        // arcInside is smoothstepped one-sample-wide at each end, adjacent
        // arcs of different colours crossfade smoothly at the seam.
        float bestMask = 0.0;
        int   bestIdx  = -1;
        for (int a = 0; a < uArcCount; a++) {
            vec4  arc  = uArcs[a];
            float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;
            if (mask > bestMask) {
                bestMask = mask;
                bestIdx  = a;
            }
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
        for (int s = 0; s < uSegmentCount; s++) {
            vec4  seg     = uSegments[s];
            // Signed wrap-distance along the perimeter in [-0.5, 0.5]. The
            // shader uses it for both the bell weight (magnitude) and the
            // head-to-tail sampling within the segment's own gradient (sign).
            float rel     = si - seg.x;
            rel          -= floor(rel + 0.5);            // wrap to [-0.5, 0.5]
            float e       = rel * seg.y;                 // normalise by invSigma
            float bell    = seg.z * exp(-e * e);         // boost * gaussian
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

        ti  += dti;
        si  += dti;
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
    // current geometry. Both sides are full-res px here, so the constants are
    // used raw; neon-optimized.frag scales them by uResolutionScale because
    // its `peri` is in FBO px - keep the two in step when tuning them.
    float r      = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri   = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;
    float headF  = HEAD_FEATHER_PX / peri;
    float tailF  = TAIL_FEATHER_PX / peri;
    // ONE arc coverage, folding per-arc intensity in, and it drives the
    // filament as well as the halo and bloom. `col` is gated-normalised above,
    // so intensity cancels out of it and can no longer reach the filament that
    // way - emitCover is what carries it. The scaling stays linear in
    // intensity, exactly as it was when it rode on `col`, and both layers are
    // now shaped by the same px-based (size-invariant) feathers.
    float emitCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF);
        emitCover = max(emitCover, c * arc.z);
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
        segCoverPt += seg.z * exp(-e * e);
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
    float reach     = max(uGlowRadius * EARLY_OUT_RADIUS_FACTOR *
                          (1.0 + uBloomStrength * uIntensity),
                          sigma * FILAMENT_REACH_SIGMAS);
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

    // Compose: base arc × intensity + segments (independent of intensity, so
    // a segment stays lit even on a dark arc - the whole point of the
    // additive segment model).
    vec3 lightCol = col * uIntensity + segCol;

    vec3 result  = lightCol * core  * FILAMENT_GAIN  * filamentGate * lineGate;
    result      += lightCol * halo  * HALO_GAIN      * glowGate * emitCoverAll;
    result      += lightCol * bloom * uBloomStrength * glowGate * emitCoverAll;

    // --- One-sided cut: mask the WHOLE emission at the line ----------
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Hard cutoff soft masks: fade the emission over the per-side
    // softness on each side of the [-uInsideCutoff, +uOutsideCutoff] band so
    // bloom/halo never punch past the stated reach. Feather is symmetric
    // around the boundary so the mid-boundary sample sees ~50% weight.
    // Disabled sides push their boundary to a huge sentinel, so the
    // smoothstep naturally evaluates to a pass-through 1.0.
    result *= smoothstep(-uInsideCutoff - inSoft,  -uInsideCutoff + inSoft,  d);
    result *= 1.0 - smoothstep(uOutsideCutoff - outSoft, uOutsideCutoff + outSoft, d);

    // --- Quad-edge fade: the draw quad ends at d == uQuadMargin (exterior).
    // Fade the emission to zero over the last stretch so a strong bloom never
    // shows a hard rectangular cutoff where the quad clips it. Interior pixels
    // have d < 0, well below the fade band, so they're unaffected. With the
    // outsideCutoff clamp above this is usually already zero, but the fade is
    // kept as a safety net for the case where uQuadMargin is smaller than
    // outsideCutoff (e.g. the CPU-side clamp is loosened later).
    result *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, d);

    // --- Grade --------------------------------------------------------
    // Hue-preserving Reinhard: tonemap the peak channel and scale the
    // others by the same ratio. Per-channel tonemap desaturates warm mixes
    // (orange → peach) because R saturates while G/B are still linear;
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
