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
// (Far early-out lives on the CPU: the Pass-1 quad is sized to rect + earlyOut,
//  so there's no per-fragment discard here. See neon-optimized-renderer.cpp.)

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

in vec2 vPos;

// Pass 1 no longer produces a picture - it produces the two low-frequency
// inputs the full-res composite pass needs, so the filament can be rasterised
// at full resolution where it is thin enough to matter. See
// docs/full-res-filament-design.md.
//
//   location 0 (RGBA16F): .rgb = lightCol, the gathered emission colour
//                         .a   = haloTerm, the scalar halo+bloom weight with
//                                every side/cutoff/quad mask already applied.
//   location 1 (R8):      .r   = segGate, the sample-based segment coverage
//                                gate for the filament.
//
// Both are smooth, many-texel-wide fields, so bilinear upscaling them is
// faithful - unlike the one-texel-wide filament that used to live here.
// Nothing is tone-mapped here: the composite pass sums filament + halo and
// tone-maps once, which is also more correct than mapping before the upscale.
layout(location = 0) out vec4 fragLight;
layout(location = 1) out vec4 fragGate;

uniform vec2  uRectSize;
uniform float uCornerRadius;
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

uniform float uSampleSpacing;

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
// ("uSegments[0]", …) at the std140 stride. On desktop GL the block is fed
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

// (perimeterPosition moved to neon-composite.frag - see the note above
// arcInside. It existed only to gate the filament, which is now drawn at full
// resolution in that pass.)

// Returns 1.0 if sample at perimeter position @c si is inside an arc that
// starts at @p start and extends forwards by @p length. Length 0 = empty,
// length 1 = full (start becomes an irrelevant phase). Anything in between
// is a wrap-aware [start, start+length] range over the unit circle.
// See neon.frag for full rationale: feather sits OUTSIDE the arc's
// mathematical bounds so a sample exactly at start / end gets weight 1.
// The virtual si+1 check keeps the arc continuous across the wrap point.
// Feather is ONE full sample width so adjacent samples' fade-in ranges are
// contiguous - a slowly growing arc head glides between gather points
// instead of freezing in the gap and jumping (see neon.frag for details).
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;
    if (length <= 1e-6)       return 0.0;
    // Asymmetric: full-sample head (contiguous fade-in -> smooth halo head),
    // quarter-sample tail (near-hard, so the start does not spill halo/bloom
    // into the corner before it). See neon.frag for the full rationale.
    float fHead = invNumSamples;
    float fTail = 0.25 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - fTail, start, si);
    float g2a = 1.0 - smoothstep(end, end + fHead, si);
    float g1b = smoothstep(start - fTail, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + fHead, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

// (arcCoverContinuous and perimeterPosition's continuous arc gate now live in
// neon-composite.frag - they only ever fed the filament, which this pass no
// longer draws.)

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far-exterior early-out (ad > earlyOut → discard) that the
    // standard NeonRenderer uses is intentionally absent here. The Pass-1
    // quad is sized on the CPU to exactly rect + earlyOut, so geometry culls
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
    if (d >  uOutsideCutoff + outSoft) discard;
    if (d < -uInsideCutoff  - inSoft ) discard;

    // (The filament profile is gone from this pass - it is rasterised at full
    // resolution in neon-composite.frag, which owns core / lineGate and the
    // continuous arc gate. This pass only feeds it lightCol + segGate.)

    // --- Kernel widths ------------------------------------------------
    // The halo kernel is floored to haloFloor so the gather never beads into
    // dots, even at tiny glow radii. That floor must NOT manufacture a halo
    // when the user asked for none (glowRadius == 0): haloGate (below) fades
    // the halo's intensity from 0 at glowRadius = 0 up to full once
    // glowRadius reaches the floor, so glowRadius = 0 reads as "filament only".
    float haloFloor = uSampleSpacing * HALO_SPACING_FLOOR;
    float kg  = max(uGlowRadius,                       haloFloor);
    float kg2 = kg * kg;
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);
    float bw2 = bw * bw;

    // --- Additive gather --------------------------------------------------
    float glow      = 0.0;
    float bloom     = 0.0;
    vec3  acc       = vec3(0.0); // base colour × per-sample gather weight
    vec3  segAcc    = vec3(0.0); // segment additive colour × bell × gather weight
    float wsum      = 0.0;
    float wsumSeg   = 0.0; // ∑ SEGMENT-only covered g - sample-based filament
                           // gate; arcs use the continuous gate (see neon.frag)

    // uNumSamples is dynamic so the perf slider actually reduces work; the
    // upper bound stays baked in the UBO array size so GL still knows the
    // maximum register pressure.
    int n = uNumSamples;
    float invNumSamples = 1.0 / float(max(n, 1));
    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (i advances in the winding direction; REPEAT wrap handles
    // the negative LUT coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = invNumSamples;
    float si   = 0.0;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);

        // Arc winner-take-all: see neon.frag for rationale.
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

        // Arc-local sampling for hasStops; perimeter-space fall-back for empty
        // stops so the arc reads continuously with the rest of the base gradient.
        // See neon.frag for the full write-up. segFallback is the colour a
        // stop-less segment inherits: the arc's colour where an arc covers,
        // the base gradient where none does (so the segment still lights).
        vec3 baseColI;
        vec3 segFallback;
        if (bestIdx >= 0) {
            vec4 winner = uArcs[bestIdx];
            if (winner.w > 0.5) {
                float rowY  = (float(bestIdx) + 0.5) / float(MAX_ARCS);
                float uArc  = (si - winner.x) / max(winner.y, 1e-4);
                uArc       -= uTime * uHueRotationRate;
                baseColI    = texture(uArcLUT, vec2(uArc, rowY)).rgb;
            } else {
                baseColI = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
            }
            segFallback = baseColI;
        } else {
            baseColI    = vec3(0.0);
            segFallback = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
        }
        acc  += baseColI * lg;

        wsum += g;

        // --- Travelling segments (independent additive lights) ---
        // Gathered with the raw proximity weight `g`, NOT the arc-gated `lg`,
        // so a segment lights even where no arc covers; segMask feeds the
        // shared coverage below. See neon.frag for the full model. Composed
        // outside uIntensity so segments stay lit even at intensity 0. Skipped
        // whole-loop when uSegmentCount == 0.
        float segMask = 0.0;
        for (int s = 0; s < uSegmentCount; s++) {
            vec4  seg  = uSegments[s];
            float rel  = si - seg.x;
            rel       -= floor(rel + 0.5);              // wrap to [-0.5, 0.5]
            float e    = rel * seg.y;
            float bell = seg.z * exp(-e * e);
            if (bell < 0.005) continue;

            vec3 segColor;
            if (seg.w > 0.5) {
                float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
                float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
                segColor     = texture(uSegmentLUT, vec2(tLocal, rowY)).rgb;
            } else {
                segColor = segFallback;
            }
            segAcc  += segColor * bell * g;
            segMask += bell;
        }

        // Shared coverage (arc mask OR segment coverage, clamped) drives halo,
        // bloom and the filament gate so a segment-only stretch emits like an
        // arc-lit one. arcW = 1 -> cover = arcW, matching the old behaviour in
        // fully arc-covered stretches. See neon.frag.
        float segCov = min(segMask, 1.0);
        float cover = max(arcW, segCov);
        glow      += cover * g * sqrt(g);
        bloom     += cover / (dd + bw2);
        wsumSeg   += segCov * g;

        ti  += dti;
        si  += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col    = acc    / max(wsum, WSUM_EPSILON);
    vec3 segCol = segAcc / max(wsum, WSUM_EPSILON);

    // Sample-based segment gate for the filament. The arc half of the gate is
    // continuous and geometric, so the composite pass computes it at full res
    // from its own fragment position; only this sample-derived half has to
    // travel through the FBO. See neon.frag.
    float segFraction = wsumSeg / max(wsum, WSUM_EPSILON);
    float segGate = smoothstep(0.5, 1.0, segFraction);

    // Halo visibility follows glowRadius (glowRadius == 0 -> filament only).
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    // Base gates on uIntensity; segments are independent (stay lit at intensity 0).
    vec3 lightCol = col * uIntensity + segCol;

    // Halo and bloom share lightCol, so their whole contribution is one scalar
    // weight - which is what lets the pair fit in a single RGBA target
    // alongside the colour.
    float haloTerm = glow * HALO_GAIN * haloGate + bloom * uBloomStrength;

    // --- One-sided cut ---
    if (uGlowSide == GLOW_SIDE_INSIDE)       haloTerm *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) haloTerm *= smoothstep(-softEdge,  softEdge, d);

    // --- Hard cutoff soft masks: fade the emission over the per-side
    // softness on each side of the [-uInsideCutoff, +uOutsideCutoff] band so
    // bloom/halo never punch past the stated reach. Mirrors neon.frag.
    // Disabled sides push their boundary to a huge value so the smoothstep
    // naturally evaluates to a pass-through 1.0. (The composite pass applies
    // the same masks to the filament, at full res, from its own `d`.)
    haloTerm *= smoothstep(-uInsideCutoff - inSoft,  -uInsideCutoff + inSoft,  d);
    haloTerm *= 1.0 - smoothstep(uOutsideCutoff - outSoft, uOutsideCutoff + outSoft, d);

    // --- Quad-edge fade: the draw quad ends at d == uQuadMargin (all in
    // scaled/FBO space). Fade the emission to zero over the last stretch so a
    // strong bloom never shows a hard rectangular cutoff where the quad clips
    // it - mirrors the base NeonRenderer so the two match. Interior pixels
    // have d < 0, well below the band.
    haloTerm *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, d);

    // Linear, un-tone-mapped, not premultiplied: this is data for the
    // composite pass, not a picture. Blending is off for this pass.
    fragLight = vec4(lightCol, haloTerm);
    fragGate  = vec4(segGate, 0.0, 0.0, 0.0);
}
