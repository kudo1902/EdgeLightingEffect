# Directional Glow (Inside / Outside Area) — Prompt, Effect Analysis & GLSL

Control whether a neon SDF outline glows into its **interior**, into the **surrounding
background**, both, or neither — with the glow cleanly contained to the chosen side of the
border. Builds on the neon-square + HDR-bloom setup.

> Key lesson baked into this doc: in a pipeline with a bloom post-process, the glow exists in
> **two** places (the scene halo *and* the blurred bloom), so masking it requires clipping in
> **both** the scene pass and the composite pass. Masking only one leaves the “disabled” side
> still lit.

-----

## 1. Detailed prompt

> Take a neon **square outline** rendered with a signed distance field (`d` < 0 inside the
> border, `d` > 0 outside). Add two independent toggles — **Inside area** and **Outside
> area** — that control where the glow appears:
> 
> - **Inside on, Outside off:** the glow fills the square’s interior and stops at the border;
>   the background stays dark.
> - **Outside on, Inside off:** the glow spills into the background; the interior stays dark.
> - **Both:** glow on both sides (default). **Neither:** only the bare filament line.
> 
> The thin bright **filament (the line itself) is always visible** regardless of toggles —
> only the soft glow is gated.
> 
> If a **bloom post-process** follows, the glow must also be clipped there: recompute the
> square’s region in the composite and multiply the bloom by an inside/outside mask, or the
> blur will leak light back onto the disabled side. Use a small smooth transition at the
> border so the boundary isn’t a hard jagged edge; optionally expose its width as a
> “containment softness” control.

Params: `uInner` (0/1 or 0..1), `uOuter` (0/1 or 0..1), plus optional `softness`.

-----

## 2. Effect analysis

### The core idea

A signed distance field gives the **sign** of `d` for free: negative inside the shape, positive
outside. That sign *is* the inside/outside selector. The glow is normally drawn from
`abs(d)` (symmetric across the line); to make it directional, keep the symmetric falloff but
**weight each side by its toggle**, with a smooth seam at `d = 0` so the line stays continuous.

### Why two masks are required

|Glow lives in…                                          |Masked where                        |If you skip it                                   |
|--------------------------------------------------------|------------------------------------|-------------------------------------------------|
|**Scene halo** (`pow(w/abs(d), …)` in the scene shader) |scene pass, by sign of `d`          |halo bleeds both ways                            |
|**Bloom** (blur of the bright line, in the post-process)|composite pass, by recomputed region|blur refills the “off” side — toggle looks broken|

A blur is **direction-agnostic**: it spreads the bright filament outward and inward equally,
with no knowledge of the SDF. So the composite has to re-derive the region and clip the bloom
to it. This is the step that’s easy to miss and the reason a scene-only mask appears to “not
work.”

### The seam at the line

Splitting by `sign(d)` with a hard step would leave a 1-pixel seam exactly on the brightest
part of the glow. A `smoothstep(-e, e, d)` blends the two sides over a small band: at `d = 0`,
inside and outside each weight 0.5, so with both enabled they sum back to 1.0 (no dip), and
with one enabled the always-on core filament covers the half-strength handoff.

### Hard vs soft containment

Clipping a blurred field at the border produces a **crisp edge** in the glow (it ends exactly
at the square). That’s usually the intent of “contain the glow inside/outside.” Widening the
composite smoothstep band turns that into a **soft fade** across the border instead.

-----

## 3. GLSL code

### 3.1 Scene pass — directional halo (mask by sign of `d`)

```glsl
uniform float uThick, uGlow, uRound, uSpeed, uInner, uOuter;

// ... sdRoundBox(), hsv2rgb(), palette() as in the neon-square shader ...

void main(){
  vec2 uv = (gl_FragCoord.xy*2.0 - uRes.xy)/min(uRes.x, uRes.y);
  uv -= (uMouse*2.0 - 1.0)*0.06;

  float d  = sdRoundBox(uv, vec2(0.62), uRound);
  float ad = abs(d);
  float w  = uThick*0.01;

  float core = w/(ad + 0.0008);              // filament — ALWAYS on, symmetric
  float g    = pow(w/(ad + 0.02), 1.6);      // symmetric halo

  float e     = max(w*0.5, 0.0025);          // seam half-width at the line
  float outer = smoothstep(-e, e, d);        // 1 outside the line, 0 inside
  float inner = 1.0 - outer;                 // 1 inside  the line, 0 outside
  float glow  = g * (outer*uOuter + inner*uInner);   // directional halo

  vec3 col = palette(uv, uTime*uSpeed);
  o = vec4(col*core*0.6*uGlow + col*glow*0.9*uGlow, 1.0);  // HDR
}
```

### 3.2 Composite pass — clip the bloom to the same region

```glsl
uniform sampler2D uScene, uBloom;
uniform float uIntensity, uExposure, uGrain, uTime; uniform int uView;
uniform vec2 uRes, uMouse; uniform float uRound, uInner, uOuter;

// ... hash(), aces() ...
float sdRoundBox(vec2 p, vec2 b, float r){
  vec2 q = abs(p)-b+r; return min(max(q.x,q.y),0.0)+length(max(q,0.0))-r;
}

void main(){
  vec3 scene = texture(uScene, vUv).rgb;     // contains the always-on filament
  vec3 bloom = texture(uBloom, vUv).rgb;

  // recompute the square's region — the blur doesn't know the SDF
  vec2 uv = (gl_FragCoord.xy*2.0 - uRes.xy)/min(uRes.x, uRes.y);
  uv -= (uMouse*2.0 - 1.0)*0.06;
  float d       = sdRoundBox(uv, vec2(0.62), uRound);
  float outside = smoothstep(-0.01, 0.01, d);     // widen 0.01 -> soft containment
  float inside  = 1.0 - outside;
  bloom *= clamp(outside*uOuter + inside*uInner, 0.0, 1.0);

  vec3 c = (uView==1) ? bloom*uIntensity
         : (uView==2) ? scene
         : scene + bloom*uIntensity;
  c *= uExposure;
  c  = aces(c);
  c  = pow(c, vec3(1.0/2.2));
  c += (hash(gl_FragCoord.xy + uTime) - 0.5)*uGrain;
  o  = vec4(c, 1.0);
}
```

### 3.3 Host wiring (WebGL2)

```js
// toggles
params.inner = 1; params.outer = 1;             // both on by default
outerCheckbox.onchange = e => params.outer = e.target.checked ? 1 : 0;
innerCheckbox.onchange = e => params.inner = e.target.checked ? 1 : 0;

// scene pass
gl.uniform1f(Us.scene.uInner, params.inner);
gl.uniform1f(Us.scene.uOuter, params.outer);

// composite pass — MUST also receive these + uRes/uMouse/uRound
gl.uniform2f(Us.comp.uRes,   canvas.width, canvas.height);
gl.uniform2f(Us.comp.uMouse, mouse.x, mouse.y);
gl.uniform1f(Us.comp.uRound, params.round);
gl.uniform1f(Us.comp.uInner, params.inner);
gl.uniform1f(Us.comp.uOuter, params.outer);
```

-----

## 4. Behaviour matrix

|Inside|Outside|Result                                               |
|:----:|:-----:|-----------------------------------------------------|
|✅     |✅      |glow both sides (default)                            |
|✅     |⬜      |interior glows, background dark, glow stops at border|
|⬜     |✅      |background glows, interior dark                      |
|⬜     |⬜      |bare filament line only                              |

-----

## 5. Pitfalls

- **Masking only the scene halo** (not the bloom) → the “off” side still glows because the
  blur refills it. This is the #1 mistake. Clip in the composite too.
- **Hard `step()` split** → seam on the brightest part of the line. Use `smoothstep`.
- **Masking the filament core** → the line vanishes when both toggles are off; usually you
  want the line to persist, so leave `core` unmasked.
- **Composite recomputes a different `uv`/`uRound`/parallax than the scene** → the mask edge
  won’t align with the visible square. Feed identical uniforms to both passes.
- **Want a soft boundary instead of a crisp one?** Increase the composite smoothstep band
  (`0.01` → e.g. `0.06`) and expose it as a “containment softness” slider.