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
#define DROPLETS_MODE_WET_GLASS  0
#define DROPLETS_MODE_LENS       1
#define DROPLETS_MODE_HIGHLIGHTS 2

uniform int   uMode;              ///< See DROPLETS_MODE_* above.
uniform sampler2D uBackground;    ///< Framebuffer snapshot; ignored when uMode == HIGHLIGHTS.

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
    if (uMode == DROPLETS_MODE_HIGHLIGHTS) {
        // No background sample. Drops read as clear water - transparent
        // centres, bright rims + a single specular dot per drop. Between
        // drops alpha stays 0 so whatever is behind the pane shows through.
        //
        //   rim   - length(normal) peaks at drop edges; that's the water's
        //           edge highlight (total internal reflection at grazing).
        //   spec  - fake directional light dotted against the height-field
        //           normal, giving each drop a bright specular hotspot.
        //   trail - thin bright streak left by the trickle.
        float rim = clamp(length(normal) * 55.0, 0.0, 1.0);
        vec2 lightDir = normalize(vec2(-0.4, 0.8));
        vec2 nrm = normal / max(length(normal), 1e-5);
        float spec = pow(max(0.0, dot(nrm, lightDir)), 6.0) * c.x;
        float trail = c.y;

        float highlight = clamp(rim * 0.9 + spec * 0.9 + trail * 0.5, 0.0, 1.0);
        dropAlpha = highlight;
        col = mix(vec3(1.0), uTint.rgb, 0.35) * highlight;
    } else if (uMode == DROPLETS_MODE_LENS) {
        // Drops act as water lenses - each drop refracts the captured
        // framebuffer through itself. Between drops alpha = 0 so the pane
        // leaves the framebuffer untouched (transparent everywhere the
        // drop mask is 0). Inside a drop, the alpha stays partial so the
        // refracted sample blends with whatever is already there instead
        // of hard-replacing it: at the drop centre the framebuffer under
        // the drop is the same pixel we're refracting, so ~60% opacity
        // reads as a subtle displaced tint (mirrors how WET_GLASS looks
        // in flat regions) rather than a hard body fill. This is the
        // "wet glass over UI" look.
        vec3 refracted = textureLod(uBackground, screenUV + normal * uDistortion, 0.0).rgb;
        col = refracted * uTint.rgb;
        dropAlpha = clamp(c.x * 0.6 + c.y * 0.45, 0.0, 1.0);
        // Luminance gate: where the refracted sample is dark (e.g. under
        // the neon's opaque-black interior fill) the drop has nothing
        // meaningful to refract, so fade its alpha out. Rec.601 luma is
        // close enough; smoothstep 0.02->0.15 kills near-black without
        // touching mid-tones.
        float lum = dot(refracted, vec3(0.299, 0.587, 0.114));
        dropAlpha *= smoothstep(0.02, 0.15, lum);
    } else {
        // WET_GLASS - fullscreen frost + sharp refraction inside drops.
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
