// Default precision is mediump: Mali (Bifrost/Valhall) runs fp16 at ~2x fp32.
// Anything whose RANGE exceeds fp16 (max 65504) is marked highp explicitly:
//  - vPos / the SDF distance d (pixel coords up to a few thousand),
//  - the gather accumulators (sums of 128 terms — fp16 would band),
// while the per-sample gather math runs in mediump on a NORMALISED coordinate
// (see GATHER_SCALE) so squared distances stay small and fp16-safe.
precision mediump float;

#define NEON_NUM_SAMPLES 128

// Normalisation for the gather: lengths are multiplied by GATHER_SCALE so a
// squared distance dot(dv,dv) stays well under fp16's 65504 even across a 4K
// viewport, while the near-line weight 1/(dd+kg^2) stays under fp16 max too.
// The gather is scale-invariant (every length scales together), so the output
// is IDENTICAL to the pixel-space version — this only changes the numeric
// range, not the result. 1/64 balances the far (dd) and near (1/kg^2) ends.
#define GATHER_SCALE 0.015625   // 1.0 / 64.0

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// All other tuning constants (FILAMENT_*, HALO_*, BLOOM_*, grading, epsilons)
// are injected from lib/include/renderer/neon-tuning.h via @NEON_TUNING@ in
// shaders.h.in — single source of truth shared with the C++ renderer.
//
// (Far early-out lives on the CPU: the draw quad is sized to rect + earlyOut,
//  so there's no per-fragment discard here. See neon-renderer.cpp.)

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

uniform float     uSampleSpacing;
uniform highp vec2 uLoopSamples[NEON_NUM_SAMPLES];

// Per-sample gather data, precomputed on the CPU by ColorUtils::BuildSampleData:
//   rgb = colour at this sample (global ring / per-segment gradient / spot),
//   a   = emission weight (baseLevel, segment level, +spot; 0 = dark, >1 = hot).
// One branch-free, texture-free lookup that folds in baseLevel, any number of
// segments, per-segment colour + rotation, and nested spots.
uniform vec4 uSampleData[NEON_NUM_SAMPLES];

// Distance (px, from rect edge) to the draw quad's edge. The emission is faded
// to zero just before this so the bloom never shows a hard rectangular cutoff.
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

    // One-sided cuts stay as discards (cull a coherent half-band).
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament: flat-top bar of width = lineWidth, constant soft edge -----
    float halfWidth = uLineWidth * 0.5;
    float core = 1.0 - smoothstep(halfWidth, halfWidth + FILAMENT_EDGE_SOFTNESS, float(ad));
    float lineGate = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

    // --- Kernel widths (pixels), then normalised for the fp16-safe gather ----
    float haloFloor = uSampleSpacing * HALO_SPACING_FLOOR;
    float kg = max(uGlowRadius,                       haloFloor);
    float bw = max(uGlowRadius * BLOOM_REACH_TO_GLOW, uSampleSpacing * BLOOM_SPACING_FLOOR);

    float kgN      = kg * GATHER_SCALE;
    float kg2N     = kgN * kgN;
    float bwN      = bw * GATHER_SCALE;
    float bw2N     = bwN * bwN;
    float spacingN = uSampleSpacing * GATHER_SCALE;

    // --- Additive gather --------------------------------------------------
    // Per sample (mediump on normalised coords): 1 sub, 1 dot, 1 reciprocal,
    // 1 array fetch (colour + weight, precomputed). No texture sample (a Mali
    // throughput win), no sqrt (halo uses g*g), no exp, no branch. Accumulators
    // are highp so summing 128 terms doesn't band.
    highp float glow    = 0.0;
    highp float bloom   = 0.0;
    highp vec3  acc     = vec3(0.0);
    highp float wsum    = 0.0;
    highp float wsumLit = 0.0;
    highp float wsumCov = 0.0;

    for (int i = 0; i < NEON_NUM_SAMPLES; i++) {
        vec2  dv = (vPos - uLoopSamples[i]) * GATHER_SCALE; // normalised → fp16-safe
        float dd = dot(dv, dv);

        float g  = 1.0 / (dd + kg2N);
        vec4  sd = uSampleData[i];   // rgb = colour, a = weight (level * coverage)
        float w  = sd.a;
        float lg = g * w;

        glow  += lg * g;             // g*g -> ~1/D^3 neon halo (no sqrt)
        bloom += w / (dd + bw2N);    // ~1/D wide spill, weighted by emission

        acc     += sd.rgb * lg;
        wsum    += g;
        wsumLit += lg;
        wsumCov += g * min(w, 1.0);  // weight capped at coverage, for the gate
    }
    // g*g normalisation needs kg^3 (kg2N * kgN) to stay unit-density.
    glow  *= spacingN * kg2N * kgN * HALO_NORM_FACTOR;
    bloom *= spacingN * bwN        * BLOOM_NORM_FACTOR;

    vec3  col = vec3(acc / max(wsum, WSUM_EPSILON));

    // g-weighted local average of the emission weight — drives the filament,
    // halo and bloom brightness (>1 where a bright segment / spot overlaps).
    float headWAvg = float(wsumLit / max(wsum, WSUM_EPSILON));

    // The filament GATE uses the coverage-capped average instead, so a high
    // level (or a spot at a segment end) brightens without pushing the sharp
    // line past the segment's coverage. Caps at 1 -> gate = segment extent.
    float litCoverage  = float(wsumCov / max(wsum, WSUM_EPSILON));
    float filamentGate = smoothstep(0.5, 1.0, litCoverage);

    // Halo visibility follows glowRadius (glowRadius == 0 -> filament only).
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity * headWAvg * filamentGate * lineGate;
    result      += col * float(glow)  * HALO_GAIN      * uIntensity * headWAvg * haloGate;
    result      += col * float(bloom) * uBloomStrength * uIntensity * headWAvg;

    // One-sided cut: mask the whole emission at the line.
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, float(d));
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, float(d));

    // Quad-edge fade: avoid a hard cutoff where the draw quad clips the bloom.
    result *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, float(d));

    // --- Grade (no film grain — dropped to save the per-pixel sin/hash) ---
    // Hue-preserving Reinhard: tone-map the MAGNITUDE (max channel) and scale
    // the colour by it, so a bright filament keeps its gradient hue instead of
    // washing to white/magenta (per-channel Reinhard saturates each channel to 1).
    float toneL = max(result.r, max(result.g, result.b));
    result = result / (toneL + TONE_MAP_SHOULDER);
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output (coverage = brightest channel) for "over"
    // compositing in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
