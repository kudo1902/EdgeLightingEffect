# Neon Square — Prompt, Effect Analysis & GLSL

A glowing rectangular outline with a warm→cool hue sweep (orange/pink → magenta → blue),
strong inner bloom, a hollow dark center, and an over-bright core that softly bleeds into
the background. Built with an SDF (signed distance field), so it stays crisp at any resolution.

-----

## 1. Detailed prompt

> Render a single neon-lit **square outline**, centered, occupying ~60% of the frame.
> The shape is **hollow** — only the border glows; the interior stays dark with a faint
> color wash. The border is a **thin bright filament** with a wide soft **halo** around it,
> like an addressable RGB LED strip behind a thin diffuser.
> 
> **Color:** a continuous hue gradient that rotates around the frame — warm **orange/amber**
> at the top-left corner, deepening to **red/pink** down the left edge, **magenta** across
> the bottom, and **electric blue** on the right side. Colors are saturated but the
> brightest filament reads near-white at its core.
> 
> **Light behavior:** the filament intensity follows an **inverse-distance falloff** (very
> bright at the line, decaying smoothly outward). Layer a tight bright **core** over a wider
> soft **bloom**. Let the glow spill slightly into the surrounding darkness. Apply gentle
> **tone mapping** so over-bright additive light rolls off to white instead of clipping
> harshly. Add subtle **film grain**.
> 
> **Motion (optional):** slowly **sweep the hue** around the border; add a faint
> breathing/flicker on intensity; allow a small **parallax** offset that follows the cursor.
> 
> **Style refs:** synthwave / cyberpunk wireframe, retro LED sign, “Tron” outline.
> Background: near-black, slightly warm-to-cool vignette.

Adjustable parameters worth exposing: `lineThickness`, `glowIntensity`, `cornerRadius`,
`backgroundBloom`, `hueSweepSpeed`, `grainAmount`.

-----

## 2. Effect analysis

### What’s happening optically (in the real photo)

- **Thin emitter + diffuser:** a narrow high-luminance source read as a bright filament
  surrounded by a scattered halo. The scatter is what makes neon look “neon.”
- **Inverse-square-ish falloff:** brightness drops sharply with distance from the line, so
  the core is near-white while the surround keeps its hue.
- **Sensor clipping:** the camera over-exposes the brightest pixels to white and blooms them
  — we *simulate* this deliberately with tone mapping instead of letting it clip uglily.
- **Conic color:** the hue changes with angular position around the square, not linearly.

### How each is reconstructed in the shader

|Visual feature                     |Technique                                          |
|-----------------------------------|---------------------------------------------------|
|Crisp resolution-independent square|`sdRoundBox()` signed distance field               |
|Hollow border (not a filled block) |`abs(d)` → distance to the *edge*, not the interior|
|Bright filament                    |`core = w / abs(d)` (inverse-distance, sharp)      |
|Soft surrounding halo              |`glow = pow(w / abs(d), 1.6)` (wider, softer)      |
|Warm→cool sweep                    |`atan(p.y, p.x)` → hue via `hsv2rgb` (conic)       |
|Color spill into interior          |`smoothstep` fill mask × bloom term                |
|No harsh white clipping            |Reinhard tone map `c / (c + k)` + gamma            |
|LED/film texture                   |hash-based per-pixel grain                         |
|Liveliness                         |time-driven hue offset, mouse parallax             |

**Key insight:** the whole look hinges on two moves — (1) `abs(distance)` to turn a filled
SDF into an outline, and (2) `1/distance` falloff for the glow. Everything else is grading.

-----

## 3. GLSL code

### 3a. GLSL ES 3.00 fragment shader (WebGL2)

```glsl
#version 300 es
precision highp float;
out vec4 outColor;

uniform vec2  uRes;      // viewport resolution (px)
uniform float uTime;     // seconds
uniform vec2  uMouse;    // 0..1 normalized cursor (optional parallax)
uniform float uThick;    // line thickness     (~2.2)
uniform float uGlow;     // glow intensity     (~0.9)
uniform float uRound;    // corner radius      (~0.06)
uniform float uBloom;    // bg bloom amount    (~0.55)
uniform float uSpeed;    // hue sweep speed    (~0.15)
uniform float uGrain;    // film grain         (~0.04)

// --- signed distance to a rounded box -------------------------------------
float sdRoundBox(vec2 p, vec2 b, float r){
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec3 hsv2rgb(vec3 c){
    vec4 K = vec4(1.0, 2.0/3.0, 1.0/3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float hash(vec2 p){
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// conic hue around the center: amber TL -> magenta bottom -> blue right
vec3 palette(vec2 p, float t){
    float ang = atan(p.y, p.x);            // -pi..pi
    float h   = (ang / 6.2831853) + 0.5;   // 0..1
    h = fract(0.04 + h * 0.92 + t);        // tuned to match the photo
    return hsv2rgb(vec3(h, 0.85, 1.0));
}

void main(){
    // aspect-correct, centered coords in roughly [-1, 1]
    vec2 uv = (gl_FragCoord.xy * 2.0 - uRes.xy) / min(uRes.x, uRes.y);

    // subtle parallax
    uv -= (uMouse * 2.0 - 1.0) * 0.06;

    float d  = sdRoundBox(uv, vec2(0.62), uRound);
    float ad = abs(d);                       // distance to the EDGE -> outline
    float w  = uThick * 0.01;

    // neon = sharp core + soft halo, both inverse-distance
    float core = w / (ad + 0.0008);
    float glow = pow(w / (ad + 0.02), 1.6);

    vec3 col = palette(uv, uTime * uSpeed);

    vec3 result  = col * core * 0.35 * uGlow;
    result      += col * glow * 0.90 * uGlow;

    // color spill into the interior / immediate surround
    float fill = smoothstep(0.9, -0.4, d);
    result    += col * fill * uBloom * 0.18;

    // tone map so over-bright additive light rolls to white (no hard clip)
    result = result / (result + vec3(0.6));
    result = pow(result, vec3(0.85));        // gamma lift

    // film grain
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * uGrain;

    outColor = vec4(result, 1.0);
}
```

### 3b. ShaderToy version (paste into the editor as-is)

ShaderToy uses GLSL ES 3.00 under the hood but exposes `mainImage` and built-in `iResolution`,
`iTime`, `iMouse`. Drop the `#version`/`uniform` lines and remap:

```glsl
// --- paste sdRoundBox, hsv2rgb, hash, palette from 3a above here ---

void mainImage(out vec4 fragColor, in vec2 fragCoord){
    vec2 uv = (fragCoord * 2.0 - iResolution.xy) / min(iResolution.x, iResolution.y);
    uv -= (iMouse.xy / iResolution.xy * 2.0 - 1.0) * 0.06;

    // hard-coded params (replace uniforms)
    float uThick = 2.2, uGlow = 0.9, uRound = 0.06,
          uBloom = 0.55, uSpeed = 0.15, uGrain = 0.04;

    float d  = sdRoundBox(uv, vec2(0.62), uRound);
    float ad = abs(d);
    float w  = uThick * 0.01;

    float core = w / (ad + 0.0008);
    float glow = pow(w / (ad + 0.02), 1.6);
    vec3  col  = palette(uv, iTime * uSpeed);

    vec3 result  = col * core * 0.35 * uGlow + col * glow * 0.90 * uGlow;
    float fill   = smoothstep(0.9, -0.4, d);
    result      += col * fill * uBloom * 0.18;

    result = result / (result + vec3(0.6));
    result = pow(result, vec3(0.85));
    result += (hash(fragCoord + iTime) - 0.5) * uGrain;

    fragColor = vec4(result, 1.0);
}
```

-----

## 4. Tuning cheatsheet

|Want…                                        |Change                                                        |
|---------------------------------------------|--------------------------------------------------------------|
|Thinner, sharper filament                    |lower `uThick` (e.g. 1.2)                                     |
|Bigger fuzzy halo                            |raise glow exponent target / lower the `+0.02` constant       |
|Linear gradient (left→right) instead of conic|replace `palette`’s `atan` with `uv.x * 0.5 + 0.5`            |
|Breathing pulse                              |multiply `uGlow` by `0.85 + 0.15*sin(uTime*3.0)`              |
|Flicker (broken-neon)                        |gate intensity with `step(0.1, hash(vec2(floor(uTime*12.0))))`|
|Sharper corners                              |`uRound = 0.0`                                                |
|Bloom-only on the inside                     |clamp `fill` to `d > 0.0 ? 0.0 : fill`                        |

**Pitfalls:** keep the `+epsilon` in the denominators (`+0.0008`, `+0.02`) or `1/d`
explodes to `inf` exactly on the line. Always tone-map *after* summing all additive light,
not per-layer, or you lose the bright-core-to-white roll-off.