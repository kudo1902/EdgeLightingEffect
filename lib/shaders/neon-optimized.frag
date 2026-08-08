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
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Positive scaled-px distance INSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel * scale CPU-side.
uniform float uInsideCutoffSoftness;  ///< Feather width (scaled px) at the inside cutoff boundary.
uniform float uOutsideCutoff;         ///< Positive scaled-px distance OUTSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel * scale CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width (scaled px) at the outside cutoff boundary.
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).

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

// Baked perimeter emission - one texel per loop sample, produced once per
// frame by neon-emission.frag:
//   .rgb = composed light colour at that sample (arc colour * mask * intensity
//          + the segments' additive contribution).
//   .a   = coverage in [0, 1], driving the halo and the bloom.
// Read with texelFetch so sample i maps exactly to uLoopSamples[i]. See
// neon-emission.frag for why the per-sample colour/mask work belongs there.
uniform sampler2D uEmission;

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops). The emission pre-pass consumes the colour side of this; here the
// block is read only to gate the sharp filament at the fragment's own
// perimeter position.
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

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). The pre-pass resolves overlap
// winner-take-all for the gathered emission; here the block is read only for
// the continuous filament gate. See neon.frag for full rationale.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Exact per-fragment perimeter position: maps this fragment's local-space point
// back to its arc-length parameter t in [0, 1), matching the CPU's
// GeometryUtils::GetPointOnRectangle for BOTH windings (uWinding = 0/1 for
// CLOCKWISE / COUNTER_CLOCKWISE). Replaces the proximity-weighted circular
// mean of the sample angles - near a corner those phases wrap through 2*pi
// right into the arc's start, smearing the whole corner curve to ~0 and
// lighting it for any arc that begins at position 0. The geometric inverse
// reads the nearest perimeter point directly, so corner and edge fragments get
// their true positions. Works in scaled FBO space because it only uses the
// (already scaled) uRectSize / uCornerRadius. See neon.frag for the details.
float perimeterPosition(vec2 p) {
    const float PI      = 3.141592653589793;
    const float TWO_PI  = 6.283185307179586;
    const float HALF_PI = 1.5707963267948966;

    float halfW  = uRectSize.x * 0.5;
    float halfH  = uRectSize.y * 0.5;
    float r      = clamp(uCornerRadius, 0.0, min(halfW, halfH));
    float halfWs = halfW - r;
    float halfHs = halfH - r;
    float ws     = uRectSize.x - 2.0 * r;
    float hs     = uRectSize.y - 2.0 * r;
    float arcLen = PI * r * 0.5;
    float peri   = 2.0 * ws + 2.0 * hs + 4.0 * arcLen;

    // Closest point on the rounded-rect perimeter (inverse rounded-box SDF).
    vec2  b  = vec2(halfWs, halfHs);
    vec2  c  = clamp(p, -b, b);
    vec2  d  = p - c;
    float dl = length(d);
    vec2  cp;
    if (dl > 1e-6)
    {
        cp = c + d * (r / dl);
    }
    else
    {
        // Inside the inner box: project straight along the dominant axis to
        // the nearest edge.
        vec2  e  = b - abs(p);
        float sx = (p.x >= 0.0) ? 1.0 : -1.0;
        float sy = (p.y >= 0.0) ? 1.0 : -1.0;
        cp = (e.x < e.y) ? vec2(sx * halfW, p.y) : vec2(p.x, sy * halfH);
    }

    float ax = abs(cp.x);
    float ay = abs(cp.y);

    // Canonical segment id (0..7 in CW order: top, TR, right, BR, bottom, BL,
    // left, TL) and the traversal progress u in [0, 1] measured in the CW
    // direction. CCW runs the same geometric core with mirrored progress
    // (1 - u) and a CCW segment layout, so both windings stay exact.
    int   seg;
    float u;
    if (ax > halfWs && ay > halfHs)
    {
        // Corner arc. The angle of the offset from the corner centre (radius r)
        // gives the fraction across the quarter-arc.
        float sx = (cp.x >= 0.0) ? 1.0 : -1.0;
        float sy = (cp.y >= 0.0) ? 1.0 : -1.0;
        float th = atan(cp.y - sy * halfHs, cp.x - sx * halfWs);
        if (sx > 0.0 && sy > 0.0)
        {
            seg = 1;                                     // top-right: theta 0..pi/2
            u   = (HALF_PI - th) / HALF_PI;
        }
        else if (sx > 0.0)
        {
            seg = 3;                                     // bottom-right: theta -pi/2..0
            u   = -th / HALF_PI;
        }
        else if (sy < 0.0)
        {
            seg = 5;                                     // bottom-left: theta -pi/2..-pi
            if (th > 0.0) th -= TWO_PI;                  // atan2 hands the left tangency back as +pi
            u = (-HALF_PI - th) / HALF_PI;
        }
        else
        {
            seg = 7;                                     // top-left: theta pi/2..pi
            u   = (PI - th) / HALF_PI;
        }
    }
    else if (ay >= halfHs)
    {
        if (cp.y > 0.0)
        {
            seg = 0;                                     // top edge: left to right
            u   = (cp.x + halfWs) / ws;
        }
        else
        {
            seg = 4;                                     // bottom edge: right to left
            u   = (halfWs - cp.x) / ws;
        }
    }
    else if (ax >= halfWs)
    {
        if (cp.x > 0.0)
        {
            seg = 2;                                     // right edge: top to bottom
            u   = (halfHs - cp.y) / hs;
        }
        else
        {
            seg = 6;                                     // left edge: bottom to top
            u   = (cp.y + halfHs) / hs;
        }
    }
    else
    {
        seg = 0;                                         // degenerate - never hit for r > 0
        u   = 0.0;
    }

    float base;
    float len;
    if (uWinding == 0)
    {
        // Segment starts (cumulative) in CW order: top, TR, right, BR, bottom,
        // BL, left, TL.
        switch (seg)
        {
        case 0: base = 0.0;                                   len = ws;     break;
        case 1: base = ws;                                    len = arcLen; break;
        case 2: base = ws + arcLen;                           len = hs;     break;
        case 3: base = ws + arcLen + hs;                      len = arcLen; break;
        case 4: base = ws + 2.0 * arcLen + hs;                len = ws;     break;
        case 5: base = ws + 2.0 * arcLen + hs + ws;           len = arcLen; break;
        case 6: base = ws + 3.0 * arcLen + hs + ws;           len = hs;     break;
        default: base = peri - arcLen;                        len = arcLen; break;
        }
        return (base + len * u) / peri;
    }

    // Segment starts in CCW order: left, BL, bottom, BR, right, TR, top, TL.
    switch (seg)
    {
    case 0: base = 2.0 * hs + 3.0 * arcLen + ws;              len = ws;     break;
    case 1: base = 2.0 * hs + 2.0 * arcLen + ws;              len = arcLen; break;
    case 2: base = hs + 2.0 * arcLen + ws;                    len = hs;     break;
    case 3: base = hs + arcLen + ws;                          len = arcLen; break;
    case 4: base = hs + arcLen;                               len = ws;     break;
    case 5: base = hs;                                        len = arcLen; break;
    case 6: base = 0.0;                                       len = hs;     break;
    default: base = 2.0 * hs + 3.0 * arcLen + 2.0 * ws;       len = arcLen; break;
    }
    return (base + len * (1.0 - u)) / peri;
}

// (The per-sample arc mask `arcInside` now lives in neon-emission.frag - it is
// a pure function of the sample's perimeter position, so it is evaluated once
// per sample per frame in the pre-pass instead of once per sample per
// fragment. This shader only needs the continuous gate below.)

// Continuous [0,1] arc coverage - INWARD FEATHER. See neon.frag for the full
// rationale; the shape here is identical.
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail) {
    if (length >= 1.0 - 1e-6) return 1.0;
    if (length <= 1e-6)       return 0.0;
    float rel = sPos - start;
    rel -= floor(rel);
    float tailIn = smoothstep(0.0, fTail, rel);
    float headIn = 1.0 - smoothstep(length - fHead, length, rel);
    return tailIn * headIn;
}

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

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile with exponentially smooth falloff (matches
    // the base NeonRenderer so the two look identical):
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 → N = 1   (Laplace - heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 → N = 2   (Gaussian - pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 → N = 4   (platykurtic - flatter top, sharper shoulder)
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
    // A numerical line integral of the perimeter emission against a
    // 1/(D^2+k^2) family of kernels - the one part the SDF cannot replace (see
    // neon.frag). Per iteration: 1 UBO read (sample position), 1 texelFetch
    // (baked emission) and the distance maths. The arc winner-take-all search,
    // the segment loop and the gradient lookups that used to run here are all
    // fragment-invariant, so they live in neon-emission.frag now and are paid
    // once per sample per FRAME instead of once per sample per fragment.
    float glow  = 0.0;
    float bloom = 0.0;
    vec3  acc   = vec3(0.0); // composed light colour * per-sample gather weight
    float wsum  = 0.0;

    // uNumSamples is dynamic so the perf slider actually reduces work; the
    // upper bound stays baked in the UBO array size so GL still knows the
    // maximum register pressure. The emission texture is indexed by the same i.
    int n = uNumSamples;

    for (int i = 0; i < n; i++) {
        vec2  dv  = vPos - uLoopSamples[i].xy;
        float dd  = dot(dv, dv);

        float g   = 1.0 / (dd + kg2);

        // Everything about WHAT is lit at this sample was resolved in the
        // pre-pass: .rgb is the composed light colour, .a the shared
        // arc-or-segment coverage that drives halo and bloom.
        vec4 emission = texelFetch(uEmission, ivec2(i, 0), 0);

        acc   += emission.rgb * g;
        // wsum accumulates ALL samples (not gated) so a fragment far from any
        // lit point gets a denominator that grows while the numerator stays
        // near zero - the filament fades to black instead of showing the lit
        // colour everywhere.
        wsum  += g;
        glow  += emission.a * g * sqrt(g);
        bloom += emission.a / (dd + bw2);
    }
    glow  *= uSampleSpacing * kg2 * HALO_NORM_FACTOR;
    bloom *= uSampleSpacing * bw  * BLOOM_NORM_FACTOR;

    // Base arc * intensity + segments, already composed per sample by the
    // pre-pass (segments sit outside uIntensity there, so one stays lit even
    // on a dark arc).
    vec3 lightCol = acc / max(wsum, WSUM_EPSILON);

    // --- Filament gate ----------------------------------------------------
    // Gated CONTINUOUSLY at this fragment's own perimeter position, never by
    // the sample gather: the gather would quantise a light's head to the
    // gather points (visible stepping on a slow tracer) and light the tail's
    // preceding corner. The position is recovered GEOMETRICALLY from vPos
    // (inverse of the CPU's GetPointOnRectangle) and is exact even at corners.
    // See neon.frag for full rationale.
    float sPos = perimeterPosition(vPos);
    // Inward feathers: convert pixel widths to perimeter fractions.
    float r      = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri   = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + 2.0 * 3.141592653589793 * r;
    float headF  = HEAD_FEATHER_PX / peri;
    float tailF  = TAIL_FEATHER_PX / peri;
    float contCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;
        contCover = max(contCover,
                        arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF));
    }

    // Segments get the same continuous treatment - their bell is a pure
    // function of perimeter position, so one evaluation at sPos is both exact
    // and cheaper than averaging the gather's stepped samples.
    float segCover = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);                     // wrap to [-0.5, 0.5]
        float e   = rel * seg.y;
        segCover  = max(segCover, seg.z * exp(-e * e));
    }

    float filamentGate = max(contCover, smoothstep(0.5, 1.0, min(segCover, 1.0)));

    // Halo visibility follows glowRadius (glowRadius == 0 -> filament only).
    float haloGate = clamp(uGlowRadius / max(haloFloor, 1e-4), 0.0, 1.0);

    vec3 result  = lightCol * core  * FILAMENT_GAIN  * filamentGate * lineGate;
    result      += lightCol * glow  * HALO_GAIN      * haloGate;
    result      += lightCol * bloom * uBloomStrength;

    // --- One-sided cut ---
    if (uGlowSide == GLOW_SIDE_INSIDE)       result *= smoothstep( softEdge, -softEdge, d);
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-softEdge,  softEdge, d);

    // --- Hard cutoff soft masks: fade the emission over the per-side
    // softness on each side of the [-uInsideCutoff, +uOutsideCutoff] band so
    // bloom/halo never punch past the stated reach. Mirrors neon.frag.
    // Disabled sides push their boundary to a huge value so the smoothstep
    // naturally evaluates to a pass-through 1.0.
    result *= smoothstep(-uInsideCutoff - inSoft,  -uInsideCutoff + inSoft,  d);
    result *= 1.0 - smoothstep(uOutsideCutoff - outSoft, uOutsideCutoff + outSoft, d);

    // --- Quad-edge fade: the draw quad ends at d == uQuadMargin (all in
    // scaled/FBO space). Fade the emission to zero over the last stretch so a
    // strong bloom never shows a hard rectangular cutoff where the quad clips
    // it - mirrors the base NeonRenderer so the two match. Interior pixels
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
