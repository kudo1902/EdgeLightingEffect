precision highp float;

// Pass 2 of the optimized neon: the full-resolution composite.
//
// The half-res gather pass (neon-optimized.frag) hands over two smooth fields
// - the emission colour and a scalar halo+bloom weight, plus the sample-based
// segment gate - and this pass:
//
//   1. upscales them (bilinear, faithful because they are low-frequency),
//   2. rasterises the FILAMENT here, at full resolution, from the analytic
//      rounded-box SDF,
//   3. sums the two and tone-maps ONCE,
//   4. writes premultiplied colour + coverage for the blend over the backbuffer.
//
// Why the filament moved up here: at resolutionScale 0.5 the filament is about
// one texel wide in the half-res buffer, so its sharpness depended on where the
// rect edge happened to fall between texel centres - the same line rendered
// crisp at one geometry and soft two pixels over. Drawing it at full res makes
// the geometry exact and the sharpness geometry-independent, and it is the
// cheap half of the shader: no gather loop, just an SDF. See
// docs/full-res-filament-design.md.
//
// The tone map moving here (rather than running at half res on the summed
// result) is also strictly better: mapping is non-linear, so doing it before
// an upscale filters already-compressed values.

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos;
out vec4 fragColor;

/// Half-res pass output 0: .rgb = lightCol, .a = halo+bloom weight.
uniform sampler2D uSource;
/// Half-res pass output 1: .r = sample-based segment gate.
uniform sampler2D uSegGate;

// Everything below is in FULL-RES pixels - this pass does not know about
// resolutionScale.
uniform vec2  uRectCenter;      ///< Rect centre in gl_FragCoord space (y-up).
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Positive px distance INSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uInsideCutoffSoftness;
uniform float uOutsideCutoff;         ///< Positive px distance OUTSIDE the rect edge past which the emission is culled.
uniform float uOutsideCutoffSoftness;
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).

// Arc gating - same std140 block the gather pass binds, reused here for the
// continuous (geometric, not sample-stepped) filament gate.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// ---------------------------------------------------------------------------
// Helpers - kept byte-identical to neon.frag / neon-optimized.frag. There is no
// #include for GLSL here; the shaders are separate strings baked by CMake.
// ---------------------------------------------------------------------------

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Exact per-fragment perimeter position: maps this fragment's local-space point
// back to its arc-length parameter t in [0, 1), matching the CPU's
// GeometryUtils::GetPointOnRectangle for BOTH windings (uWinding = 0/1 for
// CLOCKWISE / COUNTER_CLOCKWISE). See neon.frag for the full write-up.
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
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV

    vec4  src      = texture(uSource, uv);
    vec3  lightCol = src.rgb;
    float haloTerm = src.a;

    // Halo + bloom, already masked and normalised by the gather pass.
    vec3 result = lightCol * haloTerm;

    // --- Filament, at full resolution ------------------------------------
    // Local space = gl_FragCoord relative to the rect centre, exactly the space
    // the gather pass uses (scaled), so `d` means the same thing in both.
    vec2  p  = gl_FragCoord.xy - uRectCenter;
    float d  = sdRoundBox(p, uRectSize * 0.5, uCornerRadius);
    float ad = abs(d);

    // Generalized-Gaussian profile, identical to the base NeonRenderer:
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 -> N = 1   (Laplace - heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 -> N = 2   (Gaussian - pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 -> N = 4   (platykurtic - flatter top, sharper shoulder)
    //   uFilamentFalloff = 5.0 -> N = 10  (near-rectangular)
    //
    // lineGate fades the filament from 0 at lineWidth = 0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth = 0 means "no
    // line" instead of a single-pixel bright dot.
    float sigma = max(uLineWidth * 0.5, 0.5);
    // The profile is dead below this many sigmas out (exp2(-36) at N = 2), so
    // everything past it - the whole interior and the whole halo field - skips
    // the filament branch. That keeps this pass close to the plain blit it
    // replaced for the vast majority of fragments.
    if (ad < sigma * 6.0)
    {
        float N        = 2.0 * max(uFilamentFalloff, 1e-3);
        float core     = exp2(-pow(ad / sigma, N));
        float lineGate = clamp(uLineWidth / (FILAMENT_MIN_HALF_WIDTH * 2.0), 0.0, 1.0);

        // Gate: the sample-based segment half arrives from the gather pass; the
        // arc half is recovered geometrically from this fragment's own perimeter
        // position, so a slow tracer's head moves smoothly instead of stepping
        // across the gather points. Feathers point OUTWARD so the filament is
        // lit at the arc start (no dark lead-in) and reaches its end.
        float sPos  = perimeterPosition(p);
        float r     = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
        float peri  = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + 2.0 * 3.141592653589793 * r;
        float headF = HEAD_FEATHER_PX / peri;
        float tailF = TAIL_FEATHER_PX / peri;
        float gate  = texture(uSegGate, uv).r;
        for (int a = 0; a < uArcCount; a++)
        {
            vec4 arc = uArcs[a];
            if (arc.z <= 0.0) continue;
            gate = max(gate, arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF));
        }

        vec3 filament = lightCol * core * FILAMENT_GAIN * gate * lineGate;

        // The same masks the gather pass applied to haloTerm, recomputed here
        // at full res against this pass's own `d`.
        float softEdge = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
        if (uGlowSide == GLOW_SIDE_INSIDE)       filament *= smoothstep( softEdge, -softEdge, d);
        else if (uGlowSide == GLOW_SIDE_OUTSIDE) filament *= smoothstep(-softEdge,  softEdge, d);

        float inSoft  = max(uInsideCutoffSoftness,  SIDE_SOFT_EPSILON);
        float outSoft = max(uOutsideCutoffSoftness, SIDE_SOFT_EPSILON);
        filament *= smoothstep(-uInsideCutoff - inSoft, -uInsideCutoff + inSoft, d);
        filament *= 1.0 - smoothstep(uOutsideCutoff - outSoft, uOutsideCutoff + outSoft, d);

        result += filament;
    }

    // --- Grade --------------------------------------------------------
    // Hue-preserving Reinhard over the SUM (see neon.frag for the rationale).
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // Premultiplied-alpha output (coverage = brightest channel), composited
    // over the backbuffer with GL_ONE, GL_ONE_MINUS_SRC_ALPHA so the core
    // occludes and the halo/bloom add.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
