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
    // Feather = ONE full sample width OUTSIDE the arc on each side. Placement
    // OUTSIDE still ensures the sample sitting exactly at `start` or `end`
    // gets weight 1.0 (the ramp only extends outward), so visible ends stay
    // lined up with debug markers regardless of the feather width.
    //
    // The width MUST be >= one sample spacing (invNumSamples) so that a
    // moving arc end reads as smooth motion. A sample turns fully on once the
    // end reaches its position and starts turning on when the end is one
    // feather away; with a full-sample feather the fade-in ranges of adjacent
    // samples are contiguous, so a continuously growing arc (e.g. a slow
    // OutlineTracer over ~10 s) always has a leading sample mid-fade and the
    // head glides between gather points. A narrower ½-sample feather left a
    // ½-sample dead zone between samples where the head froze, then jumped -
    // visible as stepping/stutter on long, slow sweeps.
    //
    // invNumSamples comes from the loop-sample count so the feather always
    // matches the gather-point spacing.
    float f   = invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - f, start, si);
    float g2a = 1.0 - smoothstep(end, end + f, si);
    float g1b = smoothstep(start - f, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + f, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

// Continuous [0,1] coverage of a fragment at CONTINUOUS perimeter position
// @p sPos by the arc [start, start+length]. Unlike arcInside (which samples
// at the 128 fixed gather points and so quantises the arc head to 1/N of the
// perimeter), this reads the arc directly at the fragment's own sub-sample
// position, so a slowly growing arc head moves smoothly at ANY duration.
// Used only to gate the sharp SDF filament; the halo/bloom stay on the
// sample gather (they're wide and blurry, so their 1/N stepping is invisible).
// @p f is the head/tail feather in perimeter-fraction units.
float arcCoverContinuous(float sPos, float start, float length, float f) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // fract -> [0,1): distance past start
    // Feathered core [0, length]; head feather extends past `length`, tail
    // feather rises again as rel -> 1 (i.e. sPos just BEFORE start), matching
    // arcInside's "feather OUTSIDE the arc on each side" convention.
    float headEdge = 1.0 - smoothstep(length, length + f, rel);
    float tailEdge = smoothstep(1.0 - f, 1.0, rel);
    return max(headEdge, tailEdge);
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
    float halfWidth = uLineWidth * 0.5;
    float sigma     = max(halfWidth, 0.5);
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
    float wsumCover = 0.0; // ∑ covered g (arc OR segment) - for the filament gate

    // Proximity-weighted circular mean of the loop samples' perimeter angle,
    // used to recover this fragment's OWN continuous perimeter position (see
    // sPos below). uLoopSamples[i].zw hold (cos, sin) of 2*pi*(i/N), baked on
    // the CPU, so this is two extra FMAs per sample and no per-fragment trig.
    float sumCos    = 0.0;
    float sumSin    = 0.0;

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

        // Accumulate the circular mean of perimeter angle (weighted by the
        // same proximity g). Near the filament the 1-2 closest samples
        // dominate, so this resolves the fragment's perimeter position to a
        // small fraction of a sample - continuous, monotonic, wrap-safe.
        sumCos += g * uLoopSamples[i].z;
        sumSin += g * uLoopSamples[i].w;

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
        // the lit colour everywhere. wsumCover is the coverage-gated
        // counterpart (arc OR segment), used for the filament gate below.
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
        float cover = max(arcW, min(segMask, 1.0));
        glow      += cover * g * sqrt(g);   // -> ~1/D^2 neon halo
        bloom     += cover / (dd + bw2);    // -> ~1/D   wide spill
        wsumCover += cover * g;

        ti  += dti;
        si  += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col    = acc    / max(wsum, WSUM_EPSILON); // base perimeter colour
    vec3 segCol = segAcc / max(wsum, WSUM_EPSILON); // segments' additive contribution

    // Sharp gate for the SDF-derived filament. `col` already softly fades at
    // the arc boundary (acc/wsum dilution), but with FILAMENT_GAIN at 12 even
    // a 50%-lit boundary still produces a visible line. litFraction is the
    // ratio of lit-to-total sample weight; smoothstepped above 0.5 it cleanly
    // suppresses the filament past the arc end without affecting halo/bloom.
    float litFraction = wsumCover / max(wsum, WSUM_EPSILON);
    float filamentGate = smoothstep(0.5, 1.0, litFraction);

    // --- Continuous arc head for the filament ----------------------------
    // The sample-based litFraction above quantises the arc's head/tail to the
    // 128 gather points, so a slow tracer (e.g. a ~10 s OutlineTracer) steps
    // sample-to-sample. Recover this fragment's OWN continuous perimeter
    // position from the circular mean accumulated in the loop, then gate the
    // filament by the arc read directly at that position. atan2 of the two
    // sums gives the mean angle in [-pi, pi]; map to [0, 1). Far-from-line
    // fragments give a meaningless mean, but their filament core ~= 0 so it
    // never shows. max() with litFraction keeps segment-lit stretches (which
    // are gathered, not arc-gated) lighting their filament as before.
    float sPos = atan(sumSin, sumCos) * 0.15915494;       // * 1/(2*pi)
    sPos -= floor(sPos);                                  // -> [0, 1)
    float contCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        // ~1.5-sample feather so the moving tip is soft but still crisp.
        contCover = max(contCover,
                        arcCoverContinuous(sPos, arc.x, arc.y, 1.5 * invNumSamples));
    }
    filamentGate = max(filamentGate, contCover);

    // Halo visibility follows glowRadius so glowRadius == 0 means "filament
    // only". Below the anti-bead floor the kernel can't shrink further, so we
    // dim instead - fading the halo to nothing at glowRadius=0.
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    // Compose: base arc × intensity + segments (independent of intensity, so
    // a segment stays lit even on a dark arc - the whole point of the
    // additive segment model).
    vec3 lightCol = col * uIntensity + segCol;

    vec3 result  = lightCol * core  * FILAMENT_GAIN  * filamentGate * lineGate;
    result      += lightCol * glow  * HALO_GAIN      * haloGate;
    result      += lightCol * bloom * uBloomStrength;

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
