// Edge-device (Mali / Tizen) variant of neon.frag. Same visual logic, tuned
// for fp16: default mediump (Mali runs it at ~2x), with highp only where the
// numeric RANGE needs it (vPos / SDF distance, and the gather accumulators).
// The gather runs in mediump on a NORMALISED coordinate (GATHER_SCALE) so the
// squared distance stays fp16-safe. Rendered at reduced resolution by
// NeonOptimizedRenderer, with a dynamic (lower) sample count.
precision mediump float;

#define NEON_NUM_SAMPLES 64

// See neon.frag for the rationale; 1/64 keeps the far (dd) and near (1/kg^2)
// ends of the gather within fp16 range. The gather is scale-invariant, so this
// only changes the numeric range, not the result.
#define GATHER_SCALE 0.015625   // 1.0 / 64.0

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// FILAMENT_*, HALO_*, BLOOM_*, grading + epsilons are injected from
// lib/include/renderer/neon-tuning.h via @NEON_TUNING@ in shaders.h.in.
//
// (Far early-out is on the CPU: the Pass-1 quad is sized to rect + earlyOut.)

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

in highp vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uIntensity;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;

uniform float      uSampleSpacing;
uniform highp vec2 uLoopSamples[NEON_NUM_SAMPLES];

// Dynamic sample count (<= NEON_NUM_SAMPLES). Lower = fewer ALU/texture ops.
uniform int uNumSamples;

// Per-sample gather data, precomputed on the CPU by ColorUtils::BuildSampleData:
//   rgb = colour at this sample, a = emission weight (0 = dark, >1 = hot).
// One branch-free, texture-free lookup folding in baseLevel, any number of
// segments, per-segment colour + rotation, and nested spots. Only the first
// uNumSamples entries are read (matching the gather loop).
uniform vec4 uSampleData[NEON_NUM_SAMPLES];

// Distance (px, from rect edge) to the draw quad's edge — fade band so the
// bloom never shows a hard rectangular cutoff.
uniform float uQuadMargin;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

highp float sdRoundBox(highp vec2 p, highp vec2 b, float r) {
    highp vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------

void main() {
    highp vec2  halfSize = uRectSize * 0.5;
    highp float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    highp float ad = abs(d);

    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament: flat-top bar of width = lineWidth, constant soft edge -----
    float halfWidth = uLineWidth * 0.5;
    float core = 1.0 - smoothstep(halfWidth, halfWidth + FILAMENT_EDGE_SOFTNESS, float(ad));
    float lineGate = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

    // --- Kernel widths, normalised for the fp16-safe gather ------------------
    float haloFloor = uSampleSpacing * HALO_SPACING_FLOOR;
    float kg = max(uGlowRadius,                       haloFloor);
    float bw = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);

    float kgN      = kg * GATHER_SCALE;
    float kg2N     = kgN * kgN;
    float bwN      = bw * GATHER_SCALE;
    float bw2N     = bwN * bwN;
    float spacingN = uSampleSpacing * GATHER_SCALE;

    // --- Additive gather (mediump per-sample, highp accumulators) ------------
    highp float glow    = 0.0;
    highp float bloom   = 0.0;
    highp vec3  acc     = vec3(0.0);
    highp float wsum    = 0.0;
    highp float wsumLit = 0.0;
    highp float wsumCov = 0.0;

    int n = min(uNumSamples, NEON_NUM_SAMPLES);

    for (int i = 0; i < NEON_NUM_SAMPLES; i++) {
        if (i >= n) break;

        vec2  dv = (vPos - uLoopSamples[i]) * GATHER_SCALE;
        float dd = dot(dv, dv);

        float g  = 1.0 / (dd + kg2N);
        vec4  sd = uSampleData[i];   // rgb = colour, a = weight (level * coverage)
        float w  = sd.a;
        float lg = g * w;

        glow  += lg * g;             // g*g -> ~1/D^3 halo (no sqrt)
        bloom += w / (dd + bw2N);

        acc     += sd.rgb * lg;
        wsum    += g;
        wsumLit += lg;
        wsumCov += g * min(w, 1.0);  // weight capped at coverage, for the gate
    }
    glow  *= spacingN * kg2N * kgN * HALO_NORM_FACTOR; // kg^3 for the g*g halo
    bloom *= spacingN * bwN        * BLOOM_NORM_FACTOR;

    vec3  col = vec3(acc / max(wsum, WSUM_EPSILON));

    // Brightness from the full weight; the filament GATE uses the coverage-capped
    // average so a high level / spot brightens without extending the lit length.
    float headWAvg     = float(wsumLit / max(wsum, WSUM_EPSILON));
    float litCoverage  = float(wsumCov / max(wsum, WSUM_EPSILON));
    float filamentGate = smoothstep(0.5, 1.0, litCoverage);

    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity * headWAvg * filamentGate * lineGate;
    result      += col * float(glow)  * HALO_GAIN      * uIntensity * headWAvg * haloGate;
    result      += col * float(bloom) * uBloomStrength * uIntensity * headWAvg;

    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, float(d));
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, float(d));

    result *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, float(d));

    // --- Grade (no film grain) ---
    // Hue-preserving Reinhard: tone-map the max channel and scale the colour by
    // it, so a bright filament keeps its hue instead of washing to white/magenta.
    float toneL = max(result.r, max(result.g, result.b));
    result = result / (toneL + TONE_MAP_SHOULDER);
    result = pow(result, vec3(GAMMA_EXPONENT));

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
