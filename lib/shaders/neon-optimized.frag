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
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
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

// Travelling segments — up to MAX_SEGMENT_BOOSTS Gaussian brightness peaks.
// Each vec3 is packed as (position, invSigma, boost) so the shader avoids the
// per-sample divide. When uSegmentCount == 0 the whole feature is skipped in
// the gather loop.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern)
// instead of loose array uniforms: DALi/Tizen writes array elements one at
// a time into the block's UBO ("uSegments[0]", "uSegments[1]", …) using the
// std140 stride, with no bulk-array upload path. On desktop GL the block is
// fed from a UBO in neon-optimized-renderer.cpp. std140 pads each vec3
// element to a 16-byte stride.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec3 uSegments[MAX_SEGMENT_BOOSTS];
};

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
// See neon.frag for full rationale: feather sits OUTSIDE the arc's
// mathematical bounds so a sample exactly at start / end gets weight 1.
// The virtual si+1 check keeps the arc continuous across the wrap point.
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;
    if (length <= 1e-6)       return 0.0;
    float f   = 0.5 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - f, start, si);
    float g2a = 1.0 - smoothstep(end, end + f, si);
    float g1b = smoothstep(start - f, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + f, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
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
    // Generalized-Gaussian profile with exponentially smooth falloff (matches
    // the base NeonRenderer so the two look identical):
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 → N = 1   (Laplace — heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 → N = 2   (Gaussian — pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 → N = 4   (platykurtic — flatter top, sharper shoulder)
    //   uFilamentFalloff = 5.0 → N = 10  (near-rectangular)
    //
    // The Gaussian has no power-law tail, so the filament reads as a clean
    // thin line with a naturally smooth roll-off.
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

    int n = textureSize(uLoopSamplesTex, 0).x;
    float invNumSamples = 1.0 / float(max(n, 1));
    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (i advances in the winding direction; REPEAT wrap handles
    // the negative LUT coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = invNumSamples;
    float si   = 0.0;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - decodeSample(texelFetch(uLoopSamplesTex, ivec2(i, 0), 0));
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);
        float arcW = arcInside(si, uArcStart, uArcLength, invNumSamples);
        float lg   = g * arcW;

        glow  += lg * sqrt(g);
        bloom += arcW / (dd + bw2);

        acc  += texture(uGradientLUT, vec2(ti, 0.5)).rgb * lg;

        wsum    += g;
        wsumArc += lg;

        // Travelling-segment head weight — sum of Gaussian bells over the
        // uSegments array. The uSegmentCount == 0 case skips the whole loop
        // (uniform branch, coherent across the draw) so the common "no boost"
        // path avoids the exp() entirely.
        if (uSegmentCount > 0) {
            float headW = 1.0;
            for (int s = 0; s < uSegmentCount; s++) {
                vec3  seg      = uSegments[s];
                float segDist  = abs(si - seg.x);
                segDist        = min(segDist, 1.0 - segDist);
                float e        = segDist * seg.y;
                headW         += seg.z * exp(-e * e);
            }
            headWSum += headW * lg;
        } else {
            headWSum += lg; // headW == 1 -> same as the bell branch with no segments
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
    // Hue-preserving Reinhard (see neon.frag for the rationale).
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output (coverage = brightest channel). Rendered into
    // the cleared-transparent half-res FBO with GL_ONE,GL_ONE_MINUS_SRC_ALPHA so
    // the FBO holds premultiplied colour + coverage, which the blit then
    // composites over background objects (core occludes, halo/bloom add).
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
