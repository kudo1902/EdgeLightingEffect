precision highp float;

// ---------------------------------------------------------------------------
// Snowfall on the rounded-rect perimeter: falling flakes only, on the same
// band model as droplets.frag - production target is a thin outside band.
//
// Grid-hashed soft dots drift downward through the band with a gentle
// horizontal wiggle. Cell size derives from @c uBandWidth so flakes fit the
// band at any thickness. No accumulation - flakes just appear randomly and
// fall through.
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
uniform float uAmount;           ///< Flake density [0-1].
uniform float uFallSpeed;        ///< Fall-speed multiplier. 0 = frozen flakes.
uniform int   uLanes;            ///< Flake lanes across the band (>= 1).
uniform int   uDensity;          ///< Number of stacked flake grids [1-3] for extra visual density.
uniform vec4  uTint;             ///< Snow colour (.rgb) and master opacity (.a).
uniform int   uGlowSide;         ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness; ///< Band-boundary feather width in pixels.
uniform float uBandWidth;        ///< Band thickness in pixels; also sets flake size.
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

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------
// Snowflake field - a slimmer cousin of DropLayer: soft round flakes, no
// trails, gentle horizontal drift. uv.y increases upward (gl_FragCoord
// convention), so advancing the pattern along +uv.y makes flakes travel
// downward.
// ---------------------------------------------------------------------------

// Per-flake shape. Type selects one of three variants so the snowfall reads
// as varied crystals rather than a grid of identical dots. Rotation @c rot
// spins the shape so the asterisks don't all point the same way.
//
//   0 -> soft round blob (packed clumps).
//   1 -> classic 6-arm crystal asterisk with a small hot centre.
//   2 -> smaller 6-arm asterisk (delicate crystal).
float FlakeShape(vec2 d, int type, float rot) {
    // Rotate the flake so each cell's asterisk sits at its own angle.
    float c = cos(rot);
    float s = sin(rot);
    d = vec2(c * d.x - s * d.y, s * d.x + c * d.y);

    float r = length(d);
    if (type == 0)
    {
        // Round blob.
        return S(0.20, 0.0, r);
    }

    // 6-arm asterisk: fold the polar angle to a wedge of pi/3 and read the
    // distance to the nearest arm axis. Two size variants share this body.
    float armThin = (type == 1) ? 0.028 : 0.020;
    float outer = (type == 1) ? 0.24 : 0.18;
    float centerR = (type == 1) ? 0.06 : 0.04;

    float a = atan(d.y, d.x);
    a = mod(a + 3.14159265, 1.04719755) - 0.52359878; ///< pi/3 fold with pi/6 offset.
    float armDist = abs(r * sin(a));
    float arm = S(armThin, 0.0, armDist) * S(outer, 0.0, r);
    float center = S(centerR, 0.0, r);
    return max(center, arm);
}

float SnowLayer(vec2 uv, float t) {
    uv.y += t;

    vec2 id = floor(uv);
    float colShift = N(id.x); ///< Per-column phase so columns don't fall in lockstep.
    uv.y += colShift;
    id = floor(uv);

    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv) - vec2(0.5, 0.5);

    // Flake centre - random x within the cell plus a slow wiggle so it
    // sways rather than tracks a straight column down.
    float x = (n.x - 0.5) * 0.6;
    x += 0.15 * sin(t * (0.4 + 0.8 * n.z) + n.y * 6.2831);

    // Density gate - only cells with n.z below the amount threshold host a
    // flake. Bail early so the shape work is skipped for empty cells.
    if (n.z >= clamp(uAmount, 0.0, 1.0))
    {
        return 0.0;
    }

    // Shape and rotation are hashed independently of the density gate so
    // the mix of blobs vs crystals stays even regardless of amount.
    // Bias: 60% blobs (dominant), 25% classic asterisks, 15% delicate
    // asterisks - dots read as the everyday flake, the star shapes are an
    // occasional accent.
    float shapeHash = fract(n.x * 91.3 + n.y * 17.7);
    int shapeType = (shapeHash < 0.60) ? 0 : ((shapeHash < 0.85) ? 1 : 2);
    float rot = fract(n.x * 7.31 + n.y * 3.71) * 6.2831;

    return FlakeShape(st - vec2(x, 0.0), shapeType, rot);
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
        depth = sd + bandWidth * 0.5 - uBandOffset;
    }

    float across = depth / bandWidth; ///< 0 at the inner boundary, 1 at the outer.

    if (across < -0.25 || across > 1.25)
    {
        discard;
    }

    float soft = clamp(max(uGlowSideSoftness, 0.5) / bandWidth, 0.001, 0.5);
    float bandMask = S(0.0, soft, across) * S(1.0, 1.0 - soft, across);
    if (bandMask <= 0.0)
    {
        discard;
    }

    // --- Falling flakes -----------------------------------------------
    // Cell size comes from the band, not the viewport - flakes fit the band
    // however thin it is.
    float lanes = float(max(uLanes, 1));
    float cellPx = bandWidth / lanes;
    vec2 uv = gl_FragCoord.xy / cellPx;
    float t = uTime * 0.2 * uFallSpeed;

    // Stack multiple grids at different UV scales for extra density and a
    // subtle parallax feel (smaller flakes read as further away). Each
    // extra layer gets its own scale + phase offset so the populations
    // don't align. The droplets renderer uses the same trick.
    float flakes = SnowLayer(uv, t);
    int density = clamp(uDensity, 1, 3);
    if (density >= 2)
    {
        flakes = max(flakes, SnowLayer(uv * 1.55 + vec2(17.3, 4.1), t * 1.15));
    }
    if (density >= 3)
    {
        flakes = max(flakes, SnowLayer(uv * 2.30 + vec2(-9.7, 22.5), t * 0.85));
    }

    float alpha = flakes * 0.85 * bandMask;
    if (alpha <= 0.001)
    {
        discard;
    }

    // Premultiplied "over": snow occludes what's behind it.
    vec3 premul = uTint.rgb * alpha;
    fragColor = vec4(premul, alpha * uTint.a);
}
