precision highp float;

// ---------------------------------------------------------------------------
// Sunlight on the rounded-rect perimeter: glints + ray spill, confined to a
// band along the edge - the same band model as droplets.frag, because the
// production target is a strip only ~20px wide on one side of the edge:
//
//   * A rounded-box SDF masks everything outside a band of @c uBandWidth
//     pixels, on the side selected by @c uGlowSide (OUTSIDE grows outward,
//     INSIDE inward, BOTH straddles the edge).
//   * Every element is sized from @c uBandWidth, never from the viewport, so
//     the effect holds up however thin the band is.
//
// Two self-lit phenomena, both running uniformly around the whole perimeter:
//
//   * Glints - star-shaped twinkles, one lane of @c uBandWidth / @c uLanes
//     pixel cells. Grid-hashed with per-cell fade envelopes so each sparkle
//     pops in and out on its own schedule.
//
//   * Ray spill - soft light shafts that are brightest at the band boundary
//     nearest the edge and fade across the band. The angular pattern is a
//     sum of integer-frequency sines of the polar angle around the rect
//     centre - integer frequencies make it seamless across the atan wrap.
//     Shafts drift at different speeds so they slowly appear, merge, and
//     dissolve.
//
// Everything is emitted additively - the pass outputs premultiplied colour
// with zero alpha so it adds light and occludes nothing.
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos; ///< Fullscreen NDC ([-1,+1]); we drive everything off gl_FragCoord.
out vec4 fragColor;

uniform vec2  uRectSize;         ///< Rect size (px).
uniform vec2  uRectCenter;       ///< Rect centre (px) in framebuffer space.
uniform float uCornerRadius;
uniform float uTime;
uniform float uAmount;           ///< Glint density [0-1]: fraction of cells that sparkle.
uniform float uSpeed;            ///< Twinkle / ray-drift speed multiplier.
uniform int   uLanes;            ///< Glint lanes across the band (>= 1).
uniform float uRayStrength;      ///< Ray-spill brightness. 0 = glints only.
uniform vec4  uTint;             ///< Sunlight colour (.rgb) and master opacity (.a).
uniform int   uGlowSide;         ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness; ///< Band-boundary feather width in pixels.
uniform float uBandWidth;        ///< Band thickness in pixels; also sets glint size.
uniform float uBandOffset;       ///< Gap in pixels between the rect edge and the band's inner boundary.

#define S(a, b, t) smoothstep(a, b, t)

// ---------------------------------------------------------------------------
// Hash helpers (same family as droplets.frag)
// ---------------------------------------------------------------------------

vec3 N13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float Saw(float b, float t) {
    return S(0.0, b, t) * S(1.0, b, t);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------
// A 4-point lens star in cell-local coords (cell spans [-0.5, 0.5]).
// Hot core + two thin perpendicular flares. Flare reach (0.35) plus the
// maximum centre offset (0.15) stays inside the half-cell, so stars never
// get clipped by their cell boundary.
// ---------------------------------------------------------------------------

float Star(vec2 d) {
    float r = length(d);
    float core = S(0.10, 0.0, r);
    float flare = S(0.35, 0.0, abs(d.x)) * S(0.030, 0.0, abs(d.y)) +
                  S(0.35, 0.0, abs(d.y)) * S(0.030, 0.0, abs(d.x));
    return clamp(core + flare * 0.8, 0.0, 1.0);
}

// ---------------------------------------------------------------------------

void main() {
    vec2 p = gl_FragCoord.xy - uRectCenter; ///< Rect-local pixels.
    vec2 halfSize = uRectSize * 0.5;

    float sd = sdRoundBox(p, halfSize, uCornerRadius);
    float bandWidth = max(uBandWidth, 1.0);

    // Depth into the band, measured from its inner boundary outward - the
    // exact parameterisation droplets.frag uses.
    float depth;
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        depth = -sd - uBandOffset;
    }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        depth = sd - uBandOffset;
    }
    else
    {
        // Straddle the edge: the band is centred on sd = 0.
        depth = sd + bandWidth * 0.5 - uBandOffset;
    }

    float across = depth / bandWidth; ///< 0 at the inner boundary, 1 at the outer.

    // Early bail - the band is a small slice of the viewport.
    if (across < -0.25 || across > 1.25)
    {
        discard;
    }

    // Band boundary feather, in the same normalised units.
    float soft = clamp(max(uGlowSideSoftness, 0.5) / bandWidth, 0.001, 0.5);
    float bandMask = S(0.0, soft, across) * S(1.0, 1.0 - soft, across);
    if (bandMask <= 0.0)
    {
        discard;
    }

    float t = uTime * uSpeed;

    // --- Ray spill --------------------------------------------------------
    // Brightest at the inner band boundary (nearest the light source side of
    // the slit), fading across the band - short shafts of light bleeding
    // through, whatever the band thickness.
    float rayLight = 0.0;
    if (uRayStrength > 0.0)
    {
        float theta = atan(p.y, p.x);
        // Integer frequencies -> continuous across the -pi/+pi wrap.
        float shafts = (0.5 + 0.5 * sin(theta * 36.0 + t * 0.70)) +
                       (0.5 + 0.5 * sin(theta * 23.0 - t * 0.45 + 1.7)) +
                       (0.5 + 0.5 * sin(theta * 57.0 + t * 1.10 + 4.1));
        shafts /= 3.0;
        // Sharpen the smooth interference field into distinct shafts.
        shafts = pow(S(0.35, 1.0, shafts), 2.0);

        float fade = pow(clamp(1.0 - across, 0.0, 1.0), 2.0);
        rayLight = shafts * fade * uRayStrength;
    }

    // --- Glints -----------------------------------------------------------
    // Cell size comes from the band, not the viewport: one lane of glints is
    // bandWidth / lanes pixels, so sparkles fit the band at any thickness.
    float lanes = float(max(uLanes, 1));
    float cellPx = bandWidth / lanes;
    vec2 uv = gl_FragCoord.xy / cellPx;
    vec2 id = floor(uv);
    vec2 st = fract(uv) - 0.5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);

    float keep = step(n.y, clamp(uAmount, 0.0, 1.0));
    // Per-cell twinkle: each sparkle has its own phase and rate, and the Saw
    // envelope keeps it dark most of the cycle - sparkles pop, not glow.
    float twinkle = Saw(0.12, fract(t * (0.10 + 0.15 * n.x) + n.z));
    vec2 center = (n.xy - 0.5) * 0.3;
    float glint = Star(st - center) * keep * twinkle;

    // --- Compose ----------------------------------------------------------
    float bright = (rayLight + glint * 1.5) * bandMask;
    if (bright <= 0.001)
    {
        discard;
    }

    // Premultiplied with zero alpha = pure additive: sunlight adds to the
    // scene, it never occludes it. Glint cores get an extra white-hot kick so
    // they read as specular flashes rather than tinted dots.
    vec3 color = uTint.rgb * bright + vec3(1.0) * glint * bandMask * 0.6;
    fragColor = vec4(color * uTint.a, 0.0);
}
