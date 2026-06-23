precision mediump float;

#define NEON_NUM_SAMPLES 64

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// Early-out: fragments past this many glow-radii past the line receive < 1%.
#define EARLY_OUT_RADIUS_FACTOR   48.0
#define EARLY_OUT_SPACING_FACTOR  32.0

// Filament (sharp line)
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_FALLOFF          2.0
#define FILAMENT_GAIN             12.0

// Halo (sharp coloured glow). Kernel uses g * sqrt(g) ≈ p = 1.5 — close to
// the original p = 1.3 visually but ~10× cheaper than pow() on most GPUs.
// Density normalisation must match: transverse falloff is 1/D^2 so we
// multiply the sum by kg² to recover unit-density brightness.
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43
#define HALO_SPACING_FLOOR        1.2

// Bloom (wide background spill)
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_SPACING_FLOOR       4.0
#define BLOOM_NORM_FACTOR         0.32

// Grading
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// Epsilons
#define SIDE_SOFT_EPSILON         1e-5
#define WSUM_EPSILON              1e-6

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uIntensity;
uniform float uTime;
uniform float uHueRotationRate;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;

uniform float uSampleSpacing;
uniform vec2  uLoopSamples[NEON_NUM_SAMPLES];

// Dynamic sample count (≤ NEON_NUM_SAMPLES). Lower = fewer ALU ops.
uniform int uNumSamples;

// Travelling segment — Gaussian brightness peak at uSegmentPosition along
// the perimeter. When uSegmentBoost == 0 the feature is effectively inactive
// (headW is uniformly 1, just a few wasted ops per sample).
uniform float uSegmentPosition;
uniform float uSegmentLength;
uniform float uSegmentBoost;

// Arc gating — only samples whose perimeter position falls within an arc of
// uArcLength starting at uArcStart contribute. Defaults (0, 1) = full lit.
//   uArcLength = 0 → nothing lit
//   uArcLength = 1 → fully lit, regardless of start (start is just a phase)
uniform float uArcStart;
uniform float uArcLength;

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

// Returns 1.0 if sample at perimeter position @c si is inside an arc that
// starts at @p start and extends forwards by @p length. Length 0 = empty,
// length 1 = full (start becomes an irrelevant phase). Anything in between
// is a wrap-aware [start, start+length] range over the unit circle.
float arcInside(float si, float start, float length) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float end = start + length;
    if (end <= 1.0) {
        // non-wrap: [start, end] fits inside [0, 1]
        return (si >= start && si <= end) ? 1.0 : 0.0;
    }
    // wrap: lit region is [start, 1] U [0, end - 1]
    end -= 1.0;
    return (si >= start || si <= end) ? 1.0 : 0.0;
}

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // --- Early-out: skip the gather where contribution is dust -------
    float earlyOut = max(uGlowRadius    * EARLY_OUT_RADIUS_FACTOR,
                         uSampleSpacing * EARLY_OUT_SPACING_FACTOR);
    if (ad > earlyOut)
        discard;

    // Stronger early-out for one-sided modes.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament -----------------------------------------------------
    float k    = max(uLineWidth * 0.5, FILAMENT_MIN_HALF_WIDTH);
    float core = pow(k / (ad + k), FILAMENT_FALLOFF);

    // --- Kernel widths ------------------------------------------------
    float kg  = max(uGlowRadius,                       uSampleSpacing * HALO_SPACING_FLOOR);
    float kg2 = kg * kg;
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);
    float bw2 = bw * bw;

    // --- Additive gather --------------------------------------------------
    float glow      = 0.0;
    float bloom     = 0.0;
    vec3  acc       = vec3(0.0);
    float wsum      = 0.0;
    float wsumArc   = 0.0;
    float headWSum  = 0.0;

    int n = min(uNumSamples, NEON_NUM_SAMPLES);
    float ti   = uTime * uHueRotationRate;
    float dti  = 1.0 / float(n);
    float invSegSigma = 1.0 / max(uSegmentLength * 0.5, 1e-3);
    float si   = 0.0;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - uLoopSamples[i];
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);
        float arcW = arcInside(si, uArcStart, uArcLength);
        float lg   = g * arcW;

        glow  += lg * sqrt(g);
        bloom += arcW / (dd + bw2);

        acc  += texture(uGradientLUT, vec2(ti, 0.5)).rgb * lg;

        wsum    += g;
        wsumArc += lg;

        float segDist = abs(si - uSegmentPosition);
        segDist = min(segDist, 1.0 - segDist);
        float bell = exp(-(segDist * invSegSigma) * (segDist * invSegSigma));
        float headW = 1.0 + uSegmentBoost * bell;
        headWSum += headW * lg;

        ti  += dti;
        si  += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col = acc / max(wsum, WSUM_EPSILON);
    float headWAvg = headWSum / max(wsum, WSUM_EPSILON);

    float litFraction = wsumArc / max(wsum, WSUM_EPSILON);
    float filamentGate = smoothstep(0.5, 1.0, litFraction);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity * headWAvg * filamentGate;
    result      += col * glow  * HALO_GAIN      * uIntensity * headWAvg;
    result      += col * bloom * uBloomStrength * uIntensity * headWAvg;

    // --- One-sided cut ---
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Grade --------------------------------------------------------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
    result = pow(result, vec3(GAMMA_EXPONENT));
    fragColor = vec4(result, 1.0);
}
