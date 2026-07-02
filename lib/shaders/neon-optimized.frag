precision highp float;

// Precision: highp (NOT mediump). This renderer's "optimized" wins come from
// the half-res FBO, reduced gather sample count, data-texture sample lookup
// and the baked colour LUT — NOT from mediump. On desktop GLES (ANGLE on
// Windows) mediump maps to fp16, whose 65504 max and ~11-bit mantissa can't
// hold the fragment coordinates: the draw quad extends hundreds of px past
// the rect, so the interpolated vPos and dot(dv, dv) overflow/quantise and
// the gather divides go 0/0 = NaN, rasterising as scattered colour "noise
// dots". highp is mandatory for fragment shaders in GLES 3.0 (#version 300
// es), so this is safe on every 3.0 target; the mediump ALU savings weren't
// worth the precision breakage.
#define NEON_NUM_SAMPLES 64

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
// (Far early-out lives on the CPU: the Pass-1 quad is sized to rect + earlyOut,
//  so there's no per-fragment discard here. See neon-optimized-renderer.cpp.)

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

// Distance (in scaled/FBO px, from the rect edge) to the draw quad's edge.
// The emission is faded to zero just before this so a tight quad never shows
// a hard rectangular cutoff where a strong bloom is clipped.
uniform float uQuadMargin;

// Loop sample positions (perimeter points) as an N×1 data texture, texelFetch'd
// in the gather loop instead of a `uniform vec2[]` array (some mobile GLES
// drivers don't accept large uniform arrays / blow the uniform-vector limit).
// RGBA8 (only byte textures are guaranteed): each position is packed as two
// 16-bit fixed-point coords (x = R:hi G:lo, y = B:hi A:lo) over
// [-uSampleMaxCoord, uSampleMaxCoord]; decodeSample() reconstructs it.
uniform sampler2D uLoopSamplesTex;
uniform float     uSampleMaxCoord;

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

// Decode a byte-packed loop sample. Per coord: hi + lo/255 reconstructs the
// 16-bit value in [0,1]; (2*n - 1) maps it to [-1,1]; * uSampleMaxCoord scales
// back to pixels in [-uSampleMaxCoord, uSampleMaxCoord]. The /255 keeps the
// intermediate < 1 so it stays fp16-safe in this mediump shader.
vec2 decodeSample(vec4 e) {
    vec2 n = vec2(e.r + e.g * (1.0 / 255.0), e.b + e.a * (1.0 / 255.0));
    return (2.0 * n - 1.0) * uSampleMaxCoord;
}

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

    // Note: the far-exterior early-out (ad > earlyOut → discard) that the
    // standard NeonRenderer uses is intentionally absent here. The Pass-1
    // quad is sized on the CPU to exactly rect + earlyOut, so geometry culls
    // the far region instead — friendlier to tile-based mobile GPUs, which
    // pay a hidden-surface-removal penalty for any discard in the shader.
    //
    // The one-sided cuts below stay as discards: they cull a useful half of
    // the band (real work saved) and the quad can't express that shape.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament -----------------------------------------------------
    // Flat-top profile: solid bar of width = lineWidth with a CONSTANT soft
    // edge (FILAMENT_EDGE_SOFTNESS) — independent of lineWidth so thickening
    // the line widens only the solid part, not the surrounding glow.
    // lineGate fades the filament from 0 at lineWidth = 0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth = 0 means "no
    // line" instead of inheriting the floored half-width.
    // fwidth-based adaptive SDF AA: aa = per-fragment rate of change of ad,
    // floored to FILAMENT_EDGE_SOFTNESS so straight edges get the same
    // constant transition as the main NeonRenderer. Symmetric smoothstep
    // around halfWidth so the visible line width stays = uLineWidth (no
    // "wider filament" side effect from the previous overly-wide gradient).
    // On rounded corners fwidth naturally widens where the gradient turns,
    // giving more sample points along the curve to bilinear-upscale from.
    float halfWidth = uLineWidth * 0.5;
    float aa        = max(fwidth(ad), FILAMENT_EDGE_SOFTNESS);
    float core      = 1.0 - smoothstep(halfWidth - aa * 0.5, halfWidth + aa * 0.5, ad);
    float lineGate  = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

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
    vec3  acc       = vec3(0.0);
    float wsum      = 0.0;
    float wsumArc   = 0.0;
    float headWSum  = 0.0;

    int n = min(uNumSamples, NEON_NUM_SAMPLES);
    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (i advances in the winding direction; REPEAT wrap handles
    // the negative LUT coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = 1.0 / float(n);
    float invSegSigma = 1.0 / max(uSegmentLength * 0.5, 1e-3);
    float si   = 0.0;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - decodeSample(texelFetch(uLoopSamplesTex, ivec2(i, 0), 0));
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);
        float arcW = arcInside(si, uArcStart, uArcLength);
        float lg   = g * arcW;

        glow  += lg * sqrt(g);
        bloom += arcW / (dd + bw2);

        acc  += texture(uGradientLUT, vec2(ti, 0.5)).rgb * lg;

        wsum    += g;
        wsumArc += lg;

        // Travelling-segment head weight. The exp() bell is the most expensive
        // op in the loop, so skip it entirely in the common case where the
        // segment feature is off (uSegmentBoost == 0). This is a uniform
        // branch — coherent across the whole draw, so effectively free.
        if (uSegmentBoost > 0.0) {
            float segDist = abs(si - uSegmentPosition);
            segDist = min(segDist, 1.0 - segDist);
            float bell = exp(-(segDist * invSegSigma) * (segDist * invSegSigma));
            headWSum += (1.0 + uSegmentBoost * bell) * lg;
        } else {
            headWSum += lg; // headW == 1 → same as the bell branch with boost 0
        }

        ti  += dti;
        si  += dti;
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    vec3 col = acc / max(wsum, WSUM_EPSILON);
    float headWAvg = headWSum / max(wsum, WSUM_EPSILON);

    float litFraction = wsumArc / max(wsum, WSUM_EPSILON);
    float filamentGate = smoothstep(0.5, 1.0, litFraction);

    // Halo visibility follows glowRadius (glowRadius == 0 -> filament only).
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity * headWAvg * filamentGate * lineGate;
    result      += col * glow  * HALO_GAIN      * uIntensity * headWAvg * haloGate;
    result      += col * bloom * uBloomStrength * uIntensity * headWAvg;

    // --- One-sided cut ---
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Quad-edge fade: the draw quad ends at d == uQuadMargin (all in
    // scaled/FBO space). Fade the emission to zero over the last stretch so a
    // strong bloom never shows a hard rectangular cutoff where the quad clips
    // it — mirrors the base NeonRenderer so the two match. Interior pixels
    // have d < 0, well below the band.
    result *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, d);

    // --- Grade --------------------------------------------------------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output (coverage = brightest channel). Rendered into
    // the cleared-transparent half-res FBO with GL_ONE,GL_ONE_MINUS_SRC_ALPHA so
    // the FBO holds premultiplied colour + coverage, which the blit then
    // composites over background objects (core occludes, halo/bloom add).
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
