precision highp float;

// ---------------------------------------------------------------------------
// Rain-on-glass droplets, confined to a band along the rounded-rect perimeter.
//
// Rain falls DOWN. The droplet field is hashed in screen space with a single
// global gravity direction, exactly as real rain behaves - it does not flow
// around the perimeter loop, which would read as circulating water rather than
// rain. What the perimeter geometry controls is *where* the rain is allowed to
// show and *how big* the drops are:
//
//   * A rounded-box SDF masks everything outside a band of @c uBandWidth
//     pixels, on the side selected by @c uGlowSide.
//   * Droplet cell size is derived from @c uBandWidth rather than from the
//     viewport, so drops fit the band however thin it is. (Sizing off the
//     viewport was the original bug: at a 20px band you saw slivers of drops
//     tens of pixels across.)
//
// Because gravity is global and the grid is screen-space, the field never
// shears or tears - but that means the band's orientation matters. A vertical
// stretch of band runs parallel to gravity, so drops trickle down it with long
// trails. A horizontal stretch runs perpendicular, where real rain does not
// streak - it beads and sits. So the layer *amplitudes* are modulated by how
// vertical the local edge is: trickles on the sides, condensation beads along
// the top and bottom, smoothly blended round the corners. This is amplitude
// only - never position - precisely so the grid stays unsheared.
//
// @c uGlowSide selects which side of the edge the band occupies:
//   OUTSIDE -> band grows outward from the edge.
//   INSIDE  -> band grows inward.
//   BOTH    -> band straddles the edge, centred on it.
//
// Drops are self-lit: transparent body plus a crescent rim and a specular dot.
// There is no framebuffer capture and no refraction pass - refraction was
// invisible over the smooth neon gradient (the only backdrop this band ever
// sees), so the whole capture/lens/wet-glass path was removed and only this
// highlight-only shading remains.
//
// Droplet field adapted from the well-known Shadertoy rain technique
// (grid-hashed trickling drops with trails; see "Heartfelt" by Martijn
// Steinrucken / The Art of Code and its many forks, e.g. tdG3Rw).
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

/// One droplet cell spans this many uv units. The droplet grid inside
/// DropLayer is (12, 2) cells per uv unit with a 6:1 tall aspect, so dividing
/// screen pixels by (CELL_UV * cellPx) makes one cell exactly cellPx wide -
/// and 6 * cellPx tall, which is the room the trail needs.
#define CELL_UV 12.0

in vec2 vPos; ///< Fullscreen NDC ([-1,+1]); we drive UVs off gl_FragCoord.
out vec4 fragColor;

uniform vec2  uRectSize;          ///< Rect size (px).
uniform vec2  uRectCenter;        ///< Rect centre (px) in framebuffer space.
uniform float uCornerRadius;
uniform vec2  uViewport;          ///< Framebuffer size in pixels.
uniform float uTime;
uniform float uAmount;
uniform float uSpeed;
uniform int   uLanes;             ///< Droplet lanes across the band (>= 1).
uniform vec4  uTint;
uniform int   uGlowSide;          ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness;  ///< Band-boundary feather width in pixels.
uniform float uBandWidth;         ///< Band thickness in pixels; also sets droplet size.
uniform float uBandOffset;        ///< Gap in pixels between the rect edge and the band's inner boundary.

#define S(a, b, t) smoothstep(a, b, t)

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

vec3 N13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0.0, b, t) * S(1.0, b, t);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------
// Droplet field (screen space, gravity-aligned)
// ---------------------------------------------------------------------------
//
// Cells are 6:1 tall so each drop has room for the trail it leaves behind.
// uv.y increases upward (gl_FragCoord convention), so advancing the pattern
// along +uv.y makes drops travel downward.

vec2 DropLayer(vec2 uv, float t) {
    vec2 baseUV = uv;
    uv.y += t * 0.75;
    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x); ///< Per-column phase so columns do not fall in lockstep.
    uv.y += colShift;
    id = floor(uv * grid);

    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(0.5, 0.0);

    float x = n.x - 0.5;
    float y = baseUV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;

    float ti = fract(t + n.z);
    y = (Saw(0.85, ti) - 0.5) * 0.9 + 0.5;
    vec2 p = vec2(x, y);
    float d = length((st - p) * a.yx);
    float mainDrop = S(0.4, 0.0, d);

    float r = sqrt(S(1.0, y, st.y));
    float cd = abs(st.x - x);
    float trail = S(0.23 * r, 0.15 * r * r, cd);
    float trailFront = S(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r;

    // Beads shed along the trail.
    y = baseUV.y;
    float trail2 = S(0.2 * r, 0.0, cd);
    y = fract(y * 10.0) + (st.y - 0.5);
    float dd = length(st - vec2(x, y));
    float droplets = S(0.3, 0.0, dd) * trail2;

    float m = mainDrop + droplets * r * trailFront;
    return vec2(m, trail);
}

/// Static condensation beads - they fade in and out in place rather than
/// running, which is what rain actually does on a near-horizontal surface.
///
/// These beads are the ONLY thing that shows on horizontal runs of the band
/// (rain cannot streak down a horizontal edge), so the cell size is tuned to
/// give a comfortable per-band count rather than the sparse "condensation on
/// a windowpane" look the original tuning aimed for.
float StaticDrops(vec2 uv, float t) {
    uv *= CELL_UV * 0.85;
    vec2 id = floor(uv);
    uv = fract(uv) - 0.5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - 0.5) * 0.7;
    float d = length(uv - p);
    float fade = Saw(0.025, fract(t + n.z));
    return S(0.3, 0.0, d) * fract(n.z * 10.0) * fade;
}

vec2 Drops(vec2 uv, float t, float l0, float l1, float l2) {
    float s = StaticDrops(uv, t) * l0;
    vec2 m1 = DropLayer(uv, t) * l1;
    vec2 m2 = DropLayer(uv * 1.85, t) * l2;

    float c = s + m1.x + m2.x;
    c = S(0.3, 1.0, c);
    return vec2(c, max(m1.y * l0, m2.y * l1));
}

// ---------------------------------------------------------------------------

void main() {
    vec2 p = gl_FragCoord.xy - uRectCenter; ///< Rect-local pixels.
    vec2 halfSize = uRectSize * 0.5;

    float sd = sdRoundBox(p, halfSize, uCornerRadius);
    float bandWidth = max(uBandWidth, 1.0);

    // Depth into the band, measured from its inner boundary outward.
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

    // Early bail. A thin band is a small slice of the viewport, so rejecting
    // before any droplet work is where most of this pass's cost goes away.
    if (across < -0.25 || across > 1.25)
    {
        discard;
    }

    // Band boundary feather, expressed in the same normalised units.
    float soft = clamp(max(uGlowSideSoftness, 0.5) / bandWidth, 0.001, 0.5);
    float bandMask = S(0.0, soft, across) * S(1.0, 1.0 - soft, across);
    if (bandMask <= 0.0)
    {
        discard;
    }

    // --- Droplet grid ----------------------------------------------------
    // Cell size comes from the band, not the viewport, so drops fit the band
    // at any thickness. `lanes` is how many drops sit side by side across it.
    float lanes = float(max(uLanes, 1));
    float cellPx = bandWidth / lanes;
    vec2 uv = gl_FragCoord.xy / (CELL_UV * cellPx);

    float t = uTime * 0.2 * uSpeed;

    // --- Orientation-aware layer mix -------------------------------------
    // Outward edge normal, from the SDF gradient. A vertical band has the
    // normal running horizontally (|grad.x| dominates), so rain can streak
    // down it. A horizontal band has |grad.y| dominant and can only bead.
    vec2 grad = vec2(sdRoundBox(p + vec2(1.0, 0.0), halfSize, uCornerRadius) - sd,
                     sdRoundBox(p + vec2(0.0, 1.0), halfSize, uCornerRadius) - sd);
    float horizontality = abs(grad.x) / max(length(grad), 1e-6);
    float runs = S(0.25, 0.8, horizontality); ///< 1 where the band is vertical.

    float rain = clamp(uAmount, 0.0, 1.0);
    // Gravity is global: the trickling layers are active everywhere, not just
    // on vertical runs. On horizontal runs the drops just cross the band
    // vertically instead of streaking along its length, which is what falling
    // rain looks like passing through a narrow slit. Static condensation stays
    // as a mild base density and is still weighted a little higher on the
    // horizontal runs, where drops pass through quickly and beads sit longer.
    float staticDrops = S(-0.5, 1.0, rain) * mix(1.6, 0.5, runs);
    float layer1 = S(0.25, 0.75, rain);
    float layer2 = S(0.0, 0.5, rain);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2);

    // Height-field gradient in cell space - independent of viewport size.
    vec2 e = vec2(0.001, 0.0);
    float cx = Drops(uv + e.xy, t, staticDrops, layer1, layer2).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2).x;
    vec2 normal = vec2(cx - c.x, cy - c.x);
    vec2 nrm = normal / max(length(normal), 1e-5);

    // --- Water shading ---------------------------------------------------
    // Water has no pigment. A drop is legible only through what it does to the
    // light behind it, so its body stays mostly transparent and the two things
    // that actually read as water are drawn on top:
    //
    //   rim  - a crescent at the drop's edge, derived from the drop MASK
    //          (c.x * (1 - c.x) * 4), not from the height-field gradient. The
    //          gradient saturates to 1 across the whole drop at any usable
    //          gain, which is what made older passes fill drops solid white.
    //          The mask-based form stays a thin outline at any drop size.
    //   spec - one tight hotspot per drop, exponent 16 so it's a dot, not a
    //          broad sheen.
    float rim = pow(clamp(c.x * (1.0 - c.x) * 4.0, 0.0, 1.0), 1.5);
    vec2 lightDir = normalize(vec2(-0.4, 0.8));
    // Weight the rim toward the lit side. An evenly bright ring reads as a
    // soap bubble; a real drop catches the light as a crescent.
    float facing = dot(nrm, lightDir) * 0.5 + 0.5;
    rim *= 0.35 + 0.65 * facing;
    float spec = pow(max(0.0, dot(nrm, lightDir)), 16.0) * c.x;

    float bright = clamp(rim * 0.5 + spec * 0.6 + c.y * 0.25, 0.0, 1.0);

    // Composite premultiplied: a faint tinted body suggests refraction the
    // drop doesn't actually do, then self-lit highlights add on top.
    vec3 bodyColor = uTint.rgb;
    float bodyAlpha = c.x * 0.10;

    vec3 premul = bodyColor * bodyAlpha + vec3(1.0) * bright;
    float dropAlpha = clamp(bodyAlpha + bright, 0.0, 1.0);

    float alpha = bandMask * dropAlpha;
    if (alpha <= 0.0)
    {
        discard;
    }

    fragColor = vec4(premul * bandMask, alpha);
}
