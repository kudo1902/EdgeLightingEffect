#version 330 core
precision highp float;

#define NEON_NUM_SAMPLES 128

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
#define FILM_GRAIN_AMOUNT         0.04

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

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
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
    // Halo kernel doubles as the colour-gather weight (saves a divide per
    // sample with no visible difference vs. the previous 1.5×-wider weight).
    float kg  = max(uGlowRadius,                       uSampleSpacing * HALO_SPACING_FLOOR);
    float kg2 = kg * kg;
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);
    float bw2 = bw * bw;

    // --- Additive gather --------------------------------------------------
    // Per iteration: 1 sub, 1 dot, 2 reciprocals, 1 sqrt, 1 texture lookup.
    // No pow(), no in-shader stops walk, no HSV math. Sweep advance is folded
    // into the GL_REPEAT-wrapped texture — no fract() needed either.
    float glow  = 0.0;
    float bloom = 0.0;
    vec3  acc   = vec3(0.0);
    float wsum  = 0.0;

    float ti   = uTime * uSweepSpeed;
    float dti  = 1.0 / float(NEON_NUM_SAMPLES);

    for (int i = 0; i < NEON_NUM_SAMPLES; i++) {
        vec2  dv  = vPos - uLoopSamples[i];
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);
        glow  += g * sqrt(g);              // -> ~1/D^2 neon halo
        bloom += 1.0 / (dd + bw2);         // -> ~1/D   wide spill

        acc  += texture(uGradientLUT, vec2(ti, 0.5)).rgb * g;
        wsum += g;
        ti   += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col = acc / max(wsum, WSUM_EPSILON);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity;
    result      += col * glow  * HALO_GAIN      * uIntensity;
    result      += col * bloom * uBloomStrength * uIntensity;

    // --- One-sided cut: mask the WHOLE emission at the line ----------
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Grade --------------------------------------------------------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
    result = pow(result, vec3(GAMMA_EXPONENT));
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * FILM_GRAIN_AMOUNT;

    fragColor = vec4(result, 1.0);
}
