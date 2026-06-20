#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2      uRectSize;
uniform float     uCornerRadius;
uniform float     uLineWidth;
uniform float     uIntensity;
uniform float     uTime;
uniform float     uGlowRadius;
uniform float     uBloomStrength;
uniform int       uGlowSide;          // 0 both, 1 inside, 2 outside
uniform float     uGlowSideSoftness;

uniform vec2      uViewportSize;
uniform sampler2D uBar;               // Pass 1 output (premultiplied bar)
uniform sampler2D uHalo;              // Pass 3 output (Gaussian-blurred bar)

uniform bool      uShowGradient;      // debug: bypass composite, show bar texture
uniform bool      uShowBlurred;       // debug: bypass composite, show blurred halo

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// -- Filament (bar boost) --
// The bar texture already has the right colour; FILAMENT_GAIN drives the
// sharp line to white-hot through the tone map.
#define FILAMENT_GAIN     12.0

// -- Halo (blurred bar) --
// Blurring spreads colour outward. Diluted by the kernel, so a high gain
// brings it back up to a usable brightness.
#define HALO_GAIN         3.0

// -- Early-out: blur kernel only extends ~2σ; past that, halo is dust.
#define EARLY_OUT_RADIUS  6.0

// -- Grading --
#define TONE_MAP_SHOULDER 0.6
#define GAMMA_EXPONENT    0.85
#define FILM_GRAIN_AMOUNT 0.04

// -- Epsilons --
#define SIDE_SOFT_EPSILON 1e-5

// ---------------------------------------------------------------------------

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 uv   = gl_FragCoord.xy / uViewportSize;
    vec3 bar  = texture(uBar,  uv).rgb;  // premultiplied colour
    vec3 halo = texture(uHalo, uv).rgb;

    // --- Debug visualisations ---
    if (uShowGradient) { fragColor = vec4(bar,  1.0); return; }
    if (uShowBlurred)  { fragColor = vec4(halo, 1.0); return; }

    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // --- Early-out: well past the blur reach there's nothing left to draw ---
    float earlyOut = uGlowRadius * EARLY_OUT_RADIUS;
    if (ad > earlyOut) discard;

    // Stronger early-out for one-sided modes: opposite half is dark anyway.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Composite: sharp filament + smooth halo + extra bloom -------------
    // The bar texture has the bright pixel-accurate line; multiplying it by
    // FILAMENT_GAIN drives the centre to white through the tone map. The blur
    // chain provides a crease-free halo because every screen pixel pulls
    // colour from a Gaussian-weighted neighbourhood of the bar — there's no
    // discontinuity at the corner diagonals (no closest-point projection).
    vec3 result = bar  * FILAMENT_GAIN                  * uIntensity
                + halo * (HALO_GAIN + uBloomStrength)   * uIntensity;

    // --- One-sided cut: mask the WHOLE emission at d = 0 -------------------
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Grade -------------------------------------------------------------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
    result = pow(result, vec3(GAMMA_EXPONENT));
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * FILM_GRAIN_AMOUNT;

    fragColor = vec4(result, 1.0);
}
