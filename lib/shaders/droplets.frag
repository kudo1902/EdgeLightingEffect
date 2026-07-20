precision highp float;

// ---------------------------------------------------------------------------
// Rain-on-glass droplets ("wet window pane"), fullscreen with side gating.
//
// The pane covers the entire viewport: it repaints the captured background
// (everything rendered before this pass, including the neon glow) through a
// frost blur, and trickling droplets refract it sharply - so the glow reads
// as diffused through wet glass everywhere except where drops run down.
//
// A rect-relative rounded-box SDF then masks the pane by @c uGlowSide:
//   BOTH    -> whole screen wet.
//   INSIDE  -> wet only inside the rect (with soft feather).
//   OUTSIDE -> wet only outside the rect.
//
// Droplet field adapted from the well-known Shadertoy rain technique
// (grid-hashed trickling drops with trails; see "Heartfelt" by Martijn
// Steinrucken / The Art of Code and its many forks, e.g. tdG3Rw).
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos; ///< Fullscreen NDC ([-1,+1]); we drive UVs off gl_FragCoord.
out vec4 fragColor;

uniform vec2  uRectSize;          ///< Rect size (px) - used only by the SDF mask.
uniform vec2  uRectCenter;        ///< Rect centre (px) in framebuffer space.
uniform float uCornerRadius;
uniform vec2  uViewport;          ///< Framebuffer size in pixels.
uniform float uTime;
uniform float uAmount;
uniform float uSpeed;
uniform float uScale;
uniform float uDistortion;
uniform float uBlur;
uniform vec4  uTint;
uniform int   uGlowSide;          ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness;  ///< SDF-mask feather width in pixels.
uniform int   uOverlayOnly;       ///< 0 = sample uBackground (refract+frost); 1 = translucent overlay only.
uniform sampler2D uBackground;    ///< Framebuffer snapshot; ignored when uOverlayOnly != 0.

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

// ---------------------------------------------------------------------------
// Droplet field (screen-space)
// ---------------------------------------------------------------------------

vec2 DropLayer(vec2 uv, float t) {
    vec2 baseUV = uv;
    uv.y += t * 0.75;
    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x);
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

    y = baseUV.y;
    float trail2 = S(0.2 * r, 0.0, cd);
    float droplets = max(0.0, (sin(y * (1.0 - y) * 120.0) - st.y)) * trail2 * trailFront * n.z;
    y = fract(y * 10.0) + (st.y - 0.5);
    float dd = length(st - vec2(x, y));
    droplets = S(0.3, 0.0, dd);

    float m = mainDrop + droplets * r * trailFront;
    return vec2(m, trail);
}

float StaticDrops(vec2 uv, float t) {
    uv *= 40.0;
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

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 screenUV = gl_FragCoord.xy / uViewport;

    // Droplet UV: screen-space, normalised by viewport height so the pattern
    // fills the whole screen at a consistent aspect; uScale zooms the field.
    // Drops now cover the viewport rather than tracking the rect.
    vec2 uv = (gl_FragCoord.xy - 0.5 * uViewport) / uViewport.y * uScale;

    float t = uTime * 0.2 * uSpeed;

    float rain = clamp(uAmount, 0.0, 1.0);
    float staticDrops = S(-0.5, 1.0, rain) * 2.0;
    float layer1 = S(0.25, 0.75, rain);
    float layer2 = S(0.0, 0.5, rain);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2);

    vec2 e = vec2(0.001, 0.0);
    float cx = Drops(uv + e, t, staticDrops, layer1, layer2).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2).x;
    vec2 normal = vec2(cx - c.x, cy - c.x);

    vec3 col;
    float dropAlpha;
    if (uOverlayOnly != 0) {
        // Overlay path: no background sample. Body reads as a soft translucent
        // fill in the tint colour; length(normal) peaks at drop edges so it
        // doubles as a fake rim highlight; trails add a thin bright streak.
        float body = c.x;
        float rim  = clamp(length(normal) * 40.0, 0.0, 1.0);
        float trail = c.y;
        dropAlpha = clamp(body * 0.5 + rim * 0.7 + trail * 0.35, 0.0, 1.0);
        col = uTint.rgb * (0.75 + rim * 1.4 + trail * 0.6);
    } else {
        // Refraction + frost path: sample the captured framebuffer.
        float focus = mix(max(uBlur - c.y * uBlur, 0.0), 0.0, S(0.1, 0.2, c.x));
        col = textureLod(uBackground, screenUV + normal * uDistortion, focus).rgb;
        col *= uTint.rgb;
        dropAlpha = 1.0;
    }

    // Side mask: fullscreen when BOTH, else clip to the corresponding half of
    // the rect SDF with a `uGlowSideSoftness`-wide feather.
    float alpha = 1.0;
    if (uGlowSide != GLOW_SIDE_BOTH) {
        vec2 pLocal = gl_FragCoord.xy - uRectCenter;
        float sd = sdRoundBox(pLocal, uRectSize * 0.5, uCornerRadius);
        float soft = max(uGlowSideSoftness, 1e-3);
        if (uGlowSide == GLOW_SIDE_INSIDE) {
            // sd < 0 inside -> keep. Ramp to 0 as sd crosses 0.
            alpha = 1.0 - S(-soft, 0.0, sd);
        } else {
            // OUTSIDE: sd > 0 outside -> keep.
            alpha = S(0.0, soft, sd);
        }
    }

    alpha *= dropAlpha;
    if (alpha <= 0.0) {
        discard;
    }

    fragColor = vec4(col * alpha, alpha);
}
