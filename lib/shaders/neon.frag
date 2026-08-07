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

uniform float uSampleSpacing;

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
    const float PI      = 3.141592653589793;
    const float TWO_PI  = 6.283185307179586;
    const float HALF_PI = 1.5707963267948966;

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

// Returns 1.0 if sample at perimeter position @c si is inside an arc that
// starts at @p start and extends forwards by @p length. Length 0 = empty,
// length 1 = full (start becomes an irrelevant phase). Anything in between
// is a wrap-aware [start, start+length] range over the unit circle.
// Fractional [0, 1] contribution of the sample at @p si to the lit arc.
// Two design points:
//   1. Smooth feather (~1 sample-width) at each end via smoothstep, so a
//      sample crossing a boundary ramps up/down instead of snapping on/off.
//   2. Wrap-aware via testing both @p si and @p si + 1 and taking the max.
//      When the arc extends past 1.0 (end > 1.0), a sample near position 0
//      is physically close to end via the perimeter loop; the virtual
//      @p si + 1 test picks that up. Same expression handles the non-wrap
//      case because @p si + 1 always falls outside a sub-unit arc there.
//
// Both together fix the "sample-density gap" - without them, when the arc's
// head sweeps across the wrap point between the last and first samples,
// there's no sample position to represent the head for ~1/N of the
// perimeter, so the arc visually stalls. With smooth + wrap check, sample 0
// starts contributing before sample N-1 stops.
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    // Feather sits OUTSIDE the arc on each side (the ramp only extends
    // outward), so the sample exactly at `start` / `end` keeps weight 1.0 and
    // visible ends line up with debug markers.
    //
    // The widths are ASYMMETRIC:
    //   - HEAD (end side): one full sample. Adjacent samples' fade-in ranges
    //     are then contiguous, so the halo head advances without a dead-zone
    //     jump as the arc grows.
    //   - TAIL (start side): a quarter sample - a near-hard, clean trailing
    //     edge. A wider tail spills extra halo/bloom OUTSIDE the arc start,
    //     very visible when the start sits just below a corner (the corner arc
    //     is the perimeter segment right BEFORE position 0), and buys nothing
    //     since the tail does not move for a growing tracer.
    float fHead = invNumSamples;
    float fTail = 0.25 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - fTail, start, si);
    float g2a = 1.0 - smoothstep(end, end + fHead, si);
    float g1b = smoothstep(start - fTail, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + fHead, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

// Continuous [0,1] TAPER for the filament's PERPENDICULAR sigma. Peaks at 1
// in the arc INTERIOR and ramps down to 0 across each endpoint - the taper
// zone STRADDLES the endpoint (half inside, half outside), so at the endpoint
// itself sigma is halfWidth * 0.5 and continues to shrink past it as
// contCover fades brightness. The visible tip becomes a soft point instead of
// a sharp cut. Distinct from arcCoverContinuous (that one gates BRIGHTNESS
// outside the endpoints with tight along-edge feathers to keep the tail wrap
// from lighting the corner curve). @p fSpan is the FULL taper span in
// perimeter fractions - half of it sits inside the arc, half outside. Capped
// per-arc at length so very short arcs still light (peak in the middle).
float arcTipTaper(float sPos, float start, float length, float fSpan) {
    if (length >= 1.0 - 1e-6) return 1.0;
    if (length <= 1e-6)       return 0.0;
    // Signed wrap-distance to the arc's centre, in [-0.5, 0.5]. Using the
    // centre keeps both endpoints symmetric and lets the taper extend equally
    // inside and outside without a special case.
    float centre = start + length * 0.5;
    float rel    = sPos - centre;
    rel         -= floor(rel + 0.5);          // wrap to [-0.5, 0.5]
    float d       = abs(rel);                 // 0 at centre, length/2 at endpoint
    float halfLen = length * 0.5;
    // Taper half-span, capped so the two straddling zones never overlap the
    // opposite endpoint (would darken the middle of a short arc).
    float fHalf   = min(fSpan * 0.5, halfLen);
    // Full at d = halfLen - fHalf (inside end of taper), 0.5 at d = halfLen
    // (endpoint), 0 at d = halfLen + fHalf (outside). Smoothstep for a soft
    // curve.
    return 1.0 - smoothstep(halfLen - fHalf, halfLen + fHalf, d);
}

// Continuous [0,1] coverage of a fragment at CONTINUOUS perimeter position
// @p sPos by the arc [start, start+length]. Unlike arcInside (which samples
// at the 128 fixed gather points and so quantises the arc head to 1/N of the
// perimeter), this reads the arc directly at the fragment's own sub-sample
// position, so a slowly growing arc head moves smoothly at ANY duration.
// Used only to gate the sharp SDF filament; the halo/bloom stay on the
// sample gather (they're wide and blurry, so their 1/N stepping is invisible).
// @p fHead / @p fTail are the head/tail feathers in perimeter-fraction units.
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // fract -> [0,1): distance past start
    // Feathers sit OUTSIDE the arc on each side, so coverage is exactly 1 at
    // the start (rel = 0) and reaches the end point (rel = length) - no inset
    // gap at either end. The tail wrap (rel -> 1) ramps coverage up as it
    // approaches the start from the other side of the circle, antialiasing the
    // start edge. With the exact geometric sPos and a small pixel-space
    // TAIL_FEATHER_PX, that ramp only lights a few px past the start - the
    // corner curve an arc beginning at position 0 sits right after stays dark.
    //   tail: 0 at rel = 1 - fTail, 1 by rel = 1 (= start).
    //   head: 1 up to length, 0 by length + fHead.
    float tailWrap = smoothstep(1.0 - fTail, 1.0, rel);
    float headFade = 1.0 - smoothstep(length, length + fHead, rel);
    return max(tailWrap, headFade);
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

    // --- Perimeter position + continuous arc coverage (hoisted) ------------
    // Recover the fragment's OWN continuous perimeter position GEOMETRICALLY
    // from vPos (inverse of the CPU's GetPointOnRectangle) and read each arc
    // at that position. Hoisted above the filament so contCover can taper the
    // filament width (sigma) toward the arc endpoints - see the sigma line
    // below. Same value later gates filamentGate and the halo/bloom.
    float sPos   = perimeterPosition(vPos);
    float r      = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri   = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + 2.0 * 3.141592653589793 * r;
    float headF  = HEAD_FEATHER_PX / peri;
    float tailF  = TAIL_FEATHER_PX / peri;
    float tipTaperF = TIP_TAPER_PX / peri;
    float contCover = 0.0;
    float tipTaper  = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        // contCover: brightness gate; feathers sit OUTSIDE the arc on each
        // side so the filament is fully lit AT the arc's start and reaches
        // its end (no inset gap). With the exact geometric sPos the tail
        // feather only wraps a few px onto the corner curve an arc starting
        // at 0 sits right after - no more smear lighting the whole corner.
        contCover = max(contCover,
                        arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF));
        // tipTaper: symmetric width taper from INSIDE the arc so both the
        // leading and trailing heads narrow to a point at the endpoint. Kept
        // independent of contCover's tiny tail feather so the tail wrap stays
        // safe against corner lighting.
        tipTaper  = max(tipTaper,
                        arcTipTaper(sPos, arc.x, arc.y, tipTaperF));
    }

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
    float halfWidth = uLineWidth * 0.5;
    // Taper the filament WIDTH toward each arc endpoint so leading and
    // trailing heads narrow to a point instead of being chopped off flat.
    // tipTaper is 1 in the arc interior and ramps down to 0 AT each endpoint
    // (from INSIDE the arc) over TIP_TAPER_PX along the perimeter, symmetric
    // at both ends. Decoupled from contCover so the tight TAIL_FEATHER_PX
    // brightness gate stays intact (needed to keep the corner curve dark
    // before an arc starting at 0). The 0.5 px floor keeps sigma safe.
    float sigma     = max(halfWidth * tipTaper, TIP_SIGMA_FLOOR_PX);
    float N         = 2.0 * max(uFilamentFalloff, 1e-3);
    float core      = exp2(-pow(ad / sigma, N));
    float lineGate  = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

    // --- Kernel widths ------------------------------------------------
    // Halo kernel doubles as the colour-gather weight (saves a divide per
    // sample with no visible difference vs. the previous 1.5×-wider weight).
    // The width is floored to haloFloor so the gather never beads into dots,
    // even at tiny glow radii. That floor must NOT manufacture a halo when the
    // user asked for none (glowRadius == 0): haloGate (below) fades the halo's
    // intensity from 0 at glowRadius=0 up to full once glowRadius reaches the
    // floor, so glowRadius=0 reads as "filament only".
    float haloFloor = uSampleSpacing * HALO_SPACING_FLOOR;
    float kg  = max(uGlowRadius,                       haloFloor);
    float kg2 = kg * kg;
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);
    float bw2 = bw * bw;

    // --- Additive gather --------------------------------------------------
    // Per iteration: 1 texture fetch + decode for the sample position, 1 sub,
    // 1 dot, 2 reciprocals, 1 sqrt, 1 gradient-LUT lookup, plus one exp() per
    // active segment boost (skipped entirely when uSegmentCount == 0).
    // No pow(), no in-shader stops walk, no HSV math. Sweep advance is folded
    // into the GL_REPEAT-wrapped LUT - no fract() either.
    float glow      = 0.0;
    float bloom     = 0.0;
    vec3  acc       = vec3(0.0); // base colour × per-sample gather weight
    vec3  segAcc    = vec3(0.0); // segment additive colour × bell × gather weight
    float wsum      = 0.0;
    float wsumSeg   = 0.0; // ∑ SEGMENT-only covered g - the sample-based part of
                           // the filament gate (arcs use the continuous gate
                           // below instead, so the arc filament never inherits
                           // the sample stepping or the tail's corner spill)

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

        float g   = 1.0 / (dd + kg2);

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
        acc  += baseColI * lg;
        // wsum accumulates ALL samples (not gated). This way `col`/`segCol`
        // divide by the full local sample density - fragments far from any lit
        // point get a denominator that grows even as the numerator stays near
        // zero, so the SDF-derived filament fades to black instead of showing
        // the lit colour everywhere. wsumSeg is the segment-coverage-gated
        // counterpart, used for the segment part of the filament gate below.
        wsum += g;

        // --- Travelling segments (independent additive lights) ---
        // Gathered with the raw proximity weight `g`, NOT the arc-gated `lg`,
        // so a segment lights even on perimeter stretches no arc covers.
        // segMask sums the samples' bells and feeds the shared coverage below,
        // giving the segment its own filament/halo/bloom there. Composed
        // outside uIntensity so segments stay lit even at intensity 0. Skipped
        // whole-loop when uSegmentCount == 0.
        float segMask = 0.0;
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
            segAcc  += segColor * bell * g;
            segMask += bell;
        }

        // Shared coverage: arc mask OR segment coverage (clamped so stacked
        // segments can't push the halo/bloom past a single light's reach).
        // Drives the halo, bloom and filament gate, so a segment-only stretch
        // emits just like an arc-lit one. In a fully arc-covered stretch
        // (arcW = 1 -> lg = g, cover = arcW) this reduces to the previous
        // arc-gated behaviour exactly, so covered regions are unchanged.
        float segCov = min(segMask, 1.0);
        float cover = max(arcW, segCov);
        glow      += cover * g * sqrt(g);   // -> ~1/D^2 neon halo
        bloom     += cover / (dd + bw2);    // -> ~1/D   wide spill
        wsumSeg   += segCov * g;            // segment-only, for the filament gate

        ti  += dti;
        si  += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col    = acc    / max(wsum, WSUM_EPSILON); // base perimeter colour
    vec3 segCol = segAcc / max(wsum, WSUM_EPSILON); // segments' additive contribution

    // Sharp gate for the SDF-derived filament. Two independent contributors:
    //
    //  1. SEGMENTS - gathered, not arc-parameterised, so they keep the
    //     sample-based gate: the segment-only lit fraction, smoothstepped
    //     above 0.5 to suppress the filament past a segment's soft edge.
    //
    //  2. ARCS - gated by the CONTINUOUS coverage below, read at this
    //     fragment's own perimeter position, NOT by the sample gather. The
    //     sample gather would (a) quantise the arc head to the 128 gather
    //     points (visible stepping on a slow tracer) and (b) light the tail's
    //     preceding corner, because the lit start sample sits right next to
    //     it - that was the "hook"/bleed at position 0. The continuous gate
    //     has neither problem: it is smooth and is exactly zero before start.
    float segFraction = wsumSeg / max(wsum, WSUM_EPSILON);
    float filamentGate = smoothstep(0.5, 1.0, segFraction);
    // sPos / peri / contCover are hoisted above the filament formula so
    // contCover can also taper sigma. contCover is a per-fragment continuous
    // arc coverage: 1 inside the arc's along-perimeter range, ramping to 0
    // over HEAD_FEATHER_PX / TAIL_FEATHER_PX past each endpoint.
    filamentGate = max(filamentGate, contCover);

    // --- Fragment-level halo/bloom arc gate --------------------------------
    // The per-sample arcInside gate above turns samples OFF past the arc
    // endpoints but does nothing to stop still-lit samples from bleeding
    // halo/bloom kernels along a shared edge into the gap: a fragment on the
    // same edge as the last-lit samples receives their full colinear halo
    // tail, showing as a thin horizontal streak past the arc end (the
    // "faint line trailing into the top-left corner" bug on start=0/len<1).
    // Mirror the filament's fragment-level continuous gate here with WIDER
    // feathers (HALO_HEAD_FEATHER_PX / HALO_TAIL_FEATHER_PX) so the halo
    // still tapers naturally past the endpoint over a short distance and
    // then goes dark. Segments are covered by segFraction (a fragment-level
    // lit fraction) so a segment inside the gap still lights halo/bloom
    // locally.
    float haloHeadF = HALO_HEAD_FEATHER_PX / peri;
    float haloTailF = HALO_TAIL_FEATHER_PX / peri;
    float haloArcCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;
        haloArcCover = max(haloArcCover,
                           arcCoverContinuous(sPos, arc.x, arc.y, haloHeadF, haloTailF));
    }
    float haloGateFrag = max(haloArcCover, segFraction);

    // Halo visibility follows glowRadius so glowRadius == 0 means "filament
    // only". Below the anti-bead floor the kernel can't shrink further, so we
    // dim instead - fading the halo to nothing at glowRadius=0.
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    // Compose: base arc × intensity + segments (independent of intensity, so
    // a segment stays lit even on a dark arc - the whole point of the
    // additive segment model).
    vec3 lightCol = col * uIntensity + segCol;

    vec3 result  = lightCol * core  * FILAMENT_GAIN  * filamentGate * lineGate;
    result      += lightCol * glow  * HALO_GAIN      * haloGate * haloGateFrag;
    result      += lightCol * bloom * uBloomStrength * haloGateFrag;

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
