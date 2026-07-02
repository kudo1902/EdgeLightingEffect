precision highp float;

#define NEON_NUM_SAMPLES 128

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

// Loop sample positions (perimeter points) as an N×1 RGBA32F data texture,
// texelFetch'd in the gather loop. Using a texture instead of a
// `uniform vec2 uLoopSamples[N]` array avoids the large per-element uniform
// allocation, which can blow the fragment uniform-vector limit (and silently
// fail to link) on some mobile GLES drivers.
//
// The texture is RGBA8 (only byte textures are guaranteed): each position is
// packed as two 16-bit fixed-point coords (x = R:hi G:lo, y = B:hi A:lo),
// normalised to [0,1] over [-uSampleMaxCoord, uSampleMaxCoord] and decoded below.
uniform sampler2D uLoopSamplesTex;
uniform float     uSampleMaxCoord;

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

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it — independent of bloom strength.
uniform float uQuadMargin;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Decode a byte-packed loop sample. Per coord: hi(channel) + lo(channel)/255
// reconstructs the 16-bit value in [0,1]; (2*n - 1) maps it to [-1,1];
// * uSampleMaxCoord scales back to pixels in [-uSampleMaxCoord, uSampleMaxCoord].
// The /255 keeps the intermediate < 1 so it stays fp16-safe.
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
    // wrap: lit region is [start, 1] ∪ [0, end - 1]
    end -= 1.0;
    return (si >= start || si <= end) ? 1.0 : 0.0;
}

// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far-exterior early-out is handled on the CPU — the draw quad is
    // sized to rect + earlyOut in NeonRenderer::setupGeometry, so geometry culls
    // the far region instead of a per-fragment discard (tiler-friendly).
    // The one-sided cuts below stay as discards: they cull a useful half-band
    // the quad can't express.
    float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    // --- Filament -----------------------------------------------------
    // Flat-top profile: a solid bar of width = lineWidth with a CONSTANT-width
    // soft edge (FILAMENT_EDGE_SOFTNESS). Because the edge width is constant,
    // thickening the line widens only the solid part — the soft "halo" around
    // it does NOT grow. (The old pow(k/(ad+k)) profile had an edge whose width
    // scaled with lineWidth, so a thick line bloomed into a wide glow even with
    // glowRadius=0.) All spreading glow now comes from glowRadius alone.
    //
    // lineGate fades the filament intensity from 0 at lineWidth=0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth=0 means "no line"
    // while sub-pixel widths fade out instead of aliasing.
    float halfWidth = uLineWidth * 0.5;
    float core = 1.0 - smoothstep(halfWidth, halfWidth + FILAMENT_EDGE_SOFTNESS, ad);
    float lineGate = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

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
    // Per iteration: 1 sub, 1 dot, 2 reciprocals, 1 sqrt, 1 texture lookup
    // (plus a tiny head-weight computation when uSegmentBoost > 0).
    // No pow(), no in-shader stops walk, no HSV math. Sweep advance is folded
    // into the GL_REPEAT-wrapped texture — no fract() needed either.
    float glow      = 0.0;
    float bloom     = 0.0;
    vec3  acc       = vec3(0.0);
    float wsum      = 0.0;
    float wsumArc   = 0.0; // ∑ lit g — for the sharp filament gate
    float headWSum  = 0.0; // ∑ headW(i) · g(i)  — for the position-weighted average

    // Negate the time term so a positive hueRotationRate scrolls the colours
    // WITH the winding (sample index i advances in the winding direction; the
    // REPEAT-wrapped LUT handles the resulting negative coordinate).
    float ti   = -uTime * uHueRotationRate;
    float dti  = 1.0 / float(NEON_NUM_SAMPLES);
    float invSegSigma = 1.0 / max(uSegmentLength * 0.5, 1e-3);
    float si   = 0.0; // sample's normalised perimeter position

    for (int i = 0; i < NEON_NUM_SAMPLES; i++) {
        vec2  dv  = vPos - decodeSample(texelFetch(uLoopSamplesTex, ivec2(i, 0), 0));
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);
        // Arc gating: arcW is 0 (off) or 1 (on). lg = "lit g".
        float arcW = arcInside(si, uArcStart, uArcLength);
        float lg   = g * arcW;

        glow  += lg * sqrt(g);              // -> ~1/D^2 neon halo, arc-gated
        bloom += arcW / (dd + bw2);         // -> ~1/D   wide spill, arc-gated

        acc  += texture(uGradientLUT, vec2(ti, 0.5)).rgb * lg;
        // wsum accumulates ALL samples (not arc-gated). This way `col` and
        // `headWAvg` divide by the full local sample density — fragments far
        // from the lit arc get a denominator that grows even as `acc` and
        // `headWSum` stay near zero, so the SDF-derived filament naturally
        // fades to black instead of showing the lit colour everywhere.
        // wsumArc is the arc-gated counterpart — used to compute a sharper
        // litFraction below for hard-cutting the filament past the arc edge.
        wsum    += g;
        wsumArc += lg;

        // --- Travelling-segment head weight (Gaussian, wrap-around) -------
        // Only lit samples contribute to the bell average so the segment
        // boost can't "shine through" into the off-arc region.
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
    // Position-weighted average head-multiplier for this fragment.
    // When uSegmentBoost == 0 every headW is 1 → headWAvg == 1 → no-op.
    float headWAvg = headWSum / max(wsum, WSUM_EPSILON);

    // Sharp gate for the SDF-derived filament. `col` already softly fades at
    // the arc boundary (acc/wsum dilution), but with FILAMENT_GAIN at 12 even
    // a 50%-lit boundary still produces a visible line. litFraction is the
    // ratio of lit-to-total sample weight; smoothstepped above 0.5 it cleanly
    // suppresses the filament past the arc end without affecting halo/bloom.
    float litFraction = wsumArc / max(wsum, WSUM_EPSILON);
    float filamentGate = smoothstep(0.5, 1.0, litFraction);

    // Halo visibility follows glowRadius so glowRadius == 0 means "filament
    // only". Below the anti-bead floor the kernel can't shrink further, so we
    // dim instead — fading the halo to nothing at glowRadius=0.
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    vec3 result  = col * core  * FILAMENT_GAIN  * uIntensity * headWAvg * filamentGate * lineGate;
    result      += col * glow  * HALO_GAIN      * uIntensity * headWAvg * haloGate;
    result      += col * bloom * uBloomStrength * uIntensity * headWAvg;

    // --- One-sided cut: mask the WHOLE emission at the line ----------
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Quad-edge fade: the draw quad ends at d == uQuadMargin (exterior).
    // Fade the emission to zero over the last stretch so a strong bloom never
    // shows a hard rectangular cutoff where the quad clips it. Interior pixels
    // have d < 0, well below the fade band, so they're unaffected.
    result *= 1.0 - smoothstep(uQuadMargin * 0.8, uQuadMargin, d);

    // --- Grade --------------------------------------------------------
    result = result / (result + vec3(TONE_MAP_SHOULDER));
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
