#version 330 core
precision highp float;

#define NEON_NUM_SAMPLES 128

// ---------------------------------------------------------------------------
// Tuning constants — all values calibrated for the gathered-loop algorithm.
// See docs/neon-square-final.md for the derivations behind each ratio.
// ---------------------------------------------------------------------------

// -- Glow-side mode (mirrors the GlowSide enum on the C++ side) --
#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// -- Early-out: fragments far past the bloom kernel contribute < 1% --
//    Multiplier on uGlowRadius, ≈ 8 × BLOOM_REACH_TO_GLOW so bloom is dust.
#define EARLY_OUT_RADIUS_FACTOR   48.0
//    Spacing-based floor for long-perimeter / small-glow cases.
#define EARLY_OUT_SPACING_FACTOR  32.0

// -- Filament (the bright line itself) --
//    Minimum half-width in pixels — keeps the line visible for sub-pixel widths.
#define FILAMENT_MIN_HALF_WIDTH   0.5
//    Squared falloff: pow(k/(ad+k), FILAMENT_FALLOFF) — higher = sharper.
#define FILAMENT_FALLOFF          2.0
//    Filament gain inside the HDR sum; combines with sat<1 colour to roll to white.
#define FILAMENT_GAIN             12.0

// -- Halo (sharp coloured neon glow) --
//    Per-sample kernel exponent. Sum along the bar -> ~1/D^(2p-1) transverse falloff.
#define HALO_KERNEL_EXPONENT      1.3
//    = 2 * HALO_KERNEL_EXPONENT - 1; used to normalise the gather to unit density.
#define HALO_NORM_EXPONENT        1.6
//    Density / kernel-width normalisation constant (empirical).
#define HALO_NORM_FACTOR          0.43
//    Halo gain inside the HDR sum.
#define HALO_GAIN                 0.90
//    Spacing floor: kernel width must be ≥ 1.2× spacing or the line beads into LEDs.
#define HALO_SPACING_FLOOR        1.2

// -- Bloom (wide soft background spill) --
//    Bloom kernel width relative to halo radius.
#define BLOOM_REACH_TO_GLOW       6.0
//    Spacing floor for the bloom kernel.
#define BLOOM_SPACING_FLOOR       4.0
//    Density / kernel-width normalisation constant (empirical).
#define BLOOM_NORM_FACTOR         0.32

// -- Colour gather (corner cross-fade between stops) --
//    Cross-fade width relative to halo radius; low = crisp, high = pastel corners.
#define COLOR_BLEND_TO_GLOW       1.5
//    Spacing floor for the colour-weight kernel.
#define COLOR_BLEND_SPACING_FLOOR 1.5

// -- Grading --
//    Reinhard tone-map shoulder: result / (result + SHOULDER).
#define TONE_MAP_SHOULDER         0.6
//    Final gamma lift (display gamma).
#define GAMMA_EXPONENT            0.85
//    Per-pixel hash noise amplitude.
#define FILM_GRAIN_AMOUNT         0.04

// -- Epsilons --
//    Tiny lower bound for the side-cut transition so smoothstep(0,0,·) never fires.
#define SIDE_SOFT_EPSILON         1e-5
//    Floor for the colour-weight sum so the divide never sees zero.
#define WSUM_EPSILON              1e-6

in vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uIntensity;
uniform float uTime;
uniform float uSweepSpeed;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;        // 0 both, 1 inside, 2 outside
uniform float uGlowSideSoftness;

uniform int   uSampleCount;
uniform float uSampleSpacing;                  // arc length between consecutive samples (pixels)
uniform vec2  uLoopSamples[NEON_NUM_SAMPLES];  // perimeter sample positions, rect-local space

uniform int   uColorStopCount;
uniform float uColorStopPositions[16];
uniform vec4  uColorStopColors[16];
uniform int   uBlendSpace;

// --- Signed distance to a rounded rectangle centered at origin -----------
// d < 0 inside, d = 0 on the edge, d > 0 outside.
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// --- Colour-space helpers -----------------------------------------------
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (d + e), q.x);
}

vec3 blendRGB(vec3 a, vec3 b, float t) { return mix(a, b, t); }

vec3 blendHSV(vec3 a, vec3 b, float t) {
    vec3 ha = rgb2hsv(a);
    vec3 hb = rgb2hsv(b);
    float dh = hb.x - ha.x;
    if (dh > 0.5)  dh -= 1.0;
    if (dh < -0.5) dh += 1.0;
    return hsv2rgb(vec3(ha.x + dh * t, mix(ha.y, hb.y, t), mix(ha.z, hb.z, t)));
}

vec3 blend(vec3 a, vec3 b, float t) {
    return (uBlendSpace == 1) ? blendHSV(a, b, t) : blendRGB(a, b, t);
}

// Sample the user-supplied colour-stop ring at perimeter position `pos` in [0,1).
vec3 sampleStops(float pos) {
    int count = uColorStopCount;
    if (count <= 0) return vec3(1.0);
    if (count == 1) return uColorStopColors[0].rgb;
    if (count == 2) {
        float tri = 1.0 - abs(2.0 * pos - 1.0);
        return blend(uColorStopColors[0].rgb, uColorStopColors[1].rgb, tri);
    }
    for (int i = 0; i < count; i++) {
        int next = (i + 1 < count) ? i + 1 : 0;
        float a = uColorStopPositions[i];
        float b = uColorStopPositions[next];
        if (next != 0) {
            if (pos >= a && pos < b) {
                float t = (pos - a) / max(b - a, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
        } else {
            float wrapLen = (1.0 - a) + b;
            if (pos >= a) {
                float t = (pos - a) / max(wrapLen, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
            if (pos < b) {
                float t = ((1.0 - a) + pos) / max(wrapLen, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
        }
    }
    return uColorStopColors[0].rgb;
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // --- Early-out: skip the gather where contribution is dust -------
    // Past EARLY_OUT_RADIUS_FACTOR × glow radius the bloom is below ~1% of
    // peak — invisible after the tone map. The spacing-based floor keeps
    // the halo intact on long perimeters with small glow values.
    float earlyOut = max(uGlowRadius    * EARLY_OUT_RADIUS_FACTOR,
                         uSampleSpacing * EARLY_OUT_SPACING_FACTOR);
    if (ad > earlyOut)
        discard;

    // Stronger early-out for one-sided modes: the masked half is unconditionally
    // dark past the softness band, so the whole gather can be skipped there.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament: width scales with lineWidth, peak stays ~1 --------
    // k / (ad + k) peaks at 1 when ad = 0 and falls as ad grows; the
    // FILAMENT_FALLOFF-power sharpens the line so it does not bleed across.
    float k    = max(uLineWidth * 0.5, FILAMENT_MIN_HALF_WIDTH);
    float core = pow(k / (ad + k), FILAMENT_FALLOFF);

    // --- Additive gather over the perimeter samples ------------------
    // Each loop sample drops two smooth kernels (halo + wide spill) plus an
    // inverse-square colour weight. Summed along the bar, the kernels add up
    // to a clean transverse falloff with no medial creases and no LED dots.
    // Kernel widths are clamped to the sample spacing so the sum stays smooth
    // even for thin halos on long perimeters.
    float kg     = max(uGlowRadius,                       uSampleSpacing * HALO_SPACING_FLOOR);
    float kg2    = kg * kg;
    float bw     = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);
    float bw2    = bw * bw;
    float blendW = max(uGlowRadius * COLOR_BLEND_TO_GLOW, uSampleSpacing * COLOR_BLEND_SPACING_FLOOR);
    float b2     = blendW * blendW;

    float glow  = 0.0;
    float bloom = 0.0;
    vec3  acc   = vec3(0.0);
    float wsum  = 0.0;
    float invCount = 1.0 / float(max(uSampleCount, 1));

    for (int i = 0; i < NEON_NUM_SAMPLES; i++) {
        if (i >= uSampleCount) break;

        vec2  dv  = vPos - uLoopSamples[i];
        float dd  = dot(dv, dv);

        glow  += pow(1.0 / (dd + kg2), HALO_KERNEL_EXPONENT);   // -> ~1/D^1.6 neon halo
        bloom += 1.0 / (dd + bw2);                              // -> ~1/D     wide spill

        float ww   = 1.0 / (dd + b2);                           // inverse-square colour weight
        float ti   = fract(float(i) * invCount + uTime * uSweepSpeed);
        acc  += sampleStops(ti) * ww;
        wsum += ww;
    }
    glow  *= uSampleSpacing * pow(kg, HALO_NORM_EXPONENT) * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw                          * BLOOM_NORM_FACTOR;

    vec3 col = acc / max(wsum, WSUM_EPSILON);   // smooth gathered colour

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity;  // white-hot filament
    result      += col * glow  * HALO_GAIN      * uIntensity;  // coloured neon halo
    result      += col * bloom * uBloomStrength * uIntensity;  // wide background spill

    // --- One-sided cut: mask the WHOLE emission at the line ----------
    // The cut sits at d = 0 (fixed, doesn't drift with thickness), and multiplies
    // the entire summed light so the dark side is truly black — no leak from the
    // filament's own falloff. softEdge is clamped to SIDE_SOFT_EPSILON to avoid the
    // degenerate smoothstep(0,0,·) when softness == 0 (the hard-edge case).
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Grade: tone map AFTER summing, then gamma + grain -----------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
    result = pow(result, vec3(GAMMA_EXPONENT));
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * FILM_GRAIN_AMOUNT;

    fragColor = vec4(result, 1.0);
}
