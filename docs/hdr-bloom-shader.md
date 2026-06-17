# HDR Bloom Post-Process — Prompt, Effect Analysis & GLSL

A physically-plausible bloom: render the scene to an **HDR** buffer, extract the bright parts,
blur them across **multiple downsampled mips**, then composite back with filmic tone mapping.
This is the Jimenez / Call-of-Duty “next-gen bloom” approach — wide, soft, stable, and energy-
conserving, unlike a single fixed-radius Gaussian.

> Pairs with the neon-square shader — that’s the *scene* this bloom consumes. But the pipeline
> is generic: drop any HDR scene texture in and it works.

-----

## 1. Detailed prompt

> Add a **multi-pass HDR bloom** post-process to a rendered scene.
> 
> Render the scene into a **floating-point (HDR) render target** so bright sources keep values
> above 1.0 — do **not** tone-map at scene stage. Then:
> 
> 1. **Bright-pass / prefilter:** extract only luminance above a **threshold** with a **soft
>    knee** (gradual ramp, not a hard cutoff). Apply a **Karis average** (weight each sample by
>    `1/(1+luma)`) to suppress *fireflies* — single ultra-bright pixels that otherwise flicker.
> 1. **Downsample chain:** progressively halve resolution across ~6–7 mips using a wide,
>    center-weighted **13-tap** filter. Each smaller mip captures a broader, softer glow.
> 1. **Upsample chain:** walk mips from smallest back to largest, blurring each with a **3×3
>    tent filter** and **adding** it onto the next-larger mip. Accumulating across scales is
>    what produces the smooth, wide falloff.
> 1. **Composite:** `final = scene + bloom × intensity`, apply **exposure**, then a **filmic
>    tone map** (ACES), gamma-correct to sRGB, and add subtle grain.
> 
> Expose: `threshold`, `softKnee`, `intensity`, `radius` (tent spread), `exposure`.
> Provide debug views to inspect **bloom-only** and **raw HDR scene** in isolation.
> 
> **Look:** soft cinematic glow that bleeds from bright edges into surrounding darkness, brightest
> cores rolling smoothly to white rather than clipping into hard flat blobs.

-----

## 2. Effect analysis

### What bloom simulates (optics)

Bloom mimics **light scattering inside a lens/eye** and **sensor/film response to intense light**:
very bright sources spill a soft halo into neighbouring darker regions. The halo is **wide and
scale-varying** — a tight core glow *plus* a broad ambient wash — which is exactly why a single
blur radius looks wrong and a **multi-scale** blur looks right.

### Why each pass exists

|Pass                              |Purpose                              |Failure if skipped/wrong                             |
|----------------------------------|-------------------------------------|-----------------------------------------------------|
|**HDR scene** (float buffer)      |Preserve values > 1.0 to bloom       |LDR clips to 1.0 → nothing bright left to extract    |
|**Threshold + soft knee**         |Only bright areas glow; gradual onset|Hard cutoff → visible banding ring at the threshold  |
|**Karis average** (prefilter only)|Tame fireflies before they spread    |Bright single pixels flicker/crawl when animated     |
|**Downsample mips**               |Capture glow at many scales cheaply  |One radius → either too tight or too soft, never both|
|**Tent upsample, additive**       |Recombine scales into smooth falloff |Replace-instead-of-add → ringing, flat plateaus      |
|**Filmic tone map** (ACES)        |Roll over-bright to white gracefully |Reinhard desaturates; raw clip looks digital         |

### Two non-obvious correctness rules

1. **Karis average on the prefilter pass only.** It’s a firefly fix for the *first* downsample.
   Applying it on every mip over-darkens the bloom (it’s a non-linear, energy-losing average).
1. **Upsample must be additive (`blendFunc(ONE, ONE)`), not a copy.** Each larger mip already
   holds its own downsampled content; you *add* the blurred smaller mip on top. This accumulation
   across scales is the whole point — it’s what makes the falloff wide and smooth.

### Cost / quality knobs

- More mips → wider max glow (each mip doubles reach) for negligible cost (they’re tiny).
- `radius` scales tent sampling distance → softens/widens without changing mip count.
- Render bloom mips at half-res of the scene (mip0 = ½) — bloom is low-frequency, full-res is wasted.

-----

## 3. GLSL code

Four fragment shaders + a shared fullscreen-triangle vertex shader. All GLSL ES 3.00 (WebGL2).

### 3.0 Shared vertex shader

```glsl
#version 300 es
layout(location=0) in vec2 p;
out vec2 vUv;
void main(){ vUv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }
```

### 3.1 Scene → HDR (example: the neon source; the key is NO tone-map here)

```glsl
#version 300 es
precision highp float;
in vec2 vUv; out vec4 o;
uniform vec2 uRes; uniform float uTime; uniform vec2 uMouse;
uniform float uThick, uGlow, uRound, uSpeed;

float sdRoundBox(vec2 p, vec2 b, float r){
  vec2 q=abs(p)-b+r; return min(max(q.x,q.y),0.0)+length(max(q,0.0))-r;
}
vec3 hsv2rgb(vec3 c){
  vec4 K=vec4(1.,2./3.,1./3.,3.);
  vec3 p=abs(fract(c.xxx+K.xyz)*6.-K.www);
  return c.z*mix(K.xxx,clamp(p-K.xxx,0.,1.),c.y);
}
void main(){
  vec2 uv=(gl_FragCoord.xy*2.-uRes.xy)/min(uRes.x,uRes.y);
  uv-=(uMouse*2.-1.)*0.06;
  float ad=abs(sdRoundBox(uv,vec2(0.62),uRound));
  float w=uThick*0.01;
  float core=w/(ad+0.0008), glow=pow(w/(ad+0.02),1.6);
  float h=fract(0.04+((atan(uv.y,uv.x)/6.2831853)+0.5)*0.92+uTime*uSpeed);
  vec3 col=hsv2rgb(vec3(h,0.85,1.0));
  o=vec4(col*core*0.6*uGlow + col*glow*0.9*uGlow, 1.0);  // HDR: values can be >> 1
}
```

### 3.2 Downsample — 13-tap, with prefilter (threshold + Karis) on first pass

```glsl
#version 300 es
precision highp float;
in vec2 vUv; out vec4 o;
uniform sampler2D uTex; uniform vec2 uTexel;
uniform int uPrefilter; uniform float uThreshold, uKnee;
#define T(dx,dy) texture(uTex, vUv+uTexel*vec2(dx,dy)).rgb
float luma(vec3 c){ return dot(c, vec3(0.2126,0.7152,0.0722)); }

vec3 prefilter(vec3 c){                 // Unreal-style soft-knee threshold
  float br=max(c.r,max(c.g,c.b));
  float kn=uThreshold*uKnee+1e-5;
  float soft=clamp(br-uThreshold+kn,0.0,2.0*kn);
  soft=soft*soft/(4.0*kn+1e-5);
  float contrib=max(soft, br-uThreshold)/max(br,1e-5);
  return c*contrib;
}
vec3 karis(vec3 a,vec3 b,vec3 c,vec3 d){ // firefly-suppressing weighted average
  float wa=1./(1.+luma(a)), wb=1./(1.+luma(b)), wc=1./(1.+luma(c)), wd=1./(1.+luma(d));
  return (a*wa+b*wb+c*wc+d*wd)/(wa+wb+wc+wd);
}
void main(){
  vec3 a=T(-2.,-2.), b=T(0.,-2.), c=T(2.,-2.);
  vec3 d=T(-1.,-1.), e=T(1.,-1.);
  vec3 f=T(-2.,0.),  g=T(0.,0.),  h=T(2.,0.);
  vec3 i=T(-1.,1.),  j=T(1.,1.);
  vec3 k=T(-2.,2.),  l=T(0.,2.),  m=T(2.,2.);
  vec3 res;
  if(uPrefilter==1){
    res = karis(d,e,i,j)*0.5
        + (karis(a,b,f,g)+karis(b,c,g,h)+karis(f,g,k,l)+karis(g,h,l,m))*0.125;
    res = prefilter(res);
  } else {
    res = (d+e+i+j)*0.25*0.5
        + ((a+b+f+g)+(b+c+g+h)+(f+g+k+l)+(g+h+l+m))*0.25*0.125;
  }
  o=vec4(res,1.0);
}
```

### 3.3 Upsample — 3×3 tent (drawn with additive blending)

```glsl
#version 300 es
precision highp float;
in vec2 vUv; out vec4 o;
uniform sampler2D uTex; uniform vec2 uTexel; uniform float uRadius;
#define U(dx,dy) texture(uTex, vUv+uTexel*vec2(dx,dy)*uRadius).rgb
void main(){
  vec3 s = U(-1., 1.)*1. + U(0., 1.)*2. + U(1., 1.)*1.
         + U(-1., 0.)*2. + U(0., 0.)*4. + U(1., 0.)*2.
         + U(-1.,-1.)*1. + U(0.,-1.)*2. + U(1.,-1.)*1.;
  o=vec4(s*(1.0/16.0),1.0);
}
```

### 3.4 Composite — exposure + ACES filmic tone map + gamma + grain

```glsl
#version 300 es
precision highp float;
in vec2 vUv; out vec4 o;
uniform sampler2D uScene, uBloom;
uniform float uIntensity, uExposure, uGrain, uTime; uniform int uView;
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
vec3 aces(vec3 x){
  return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14), 0.0, 1.0);
}
void main(){
  vec3 scene=texture(uScene,vUv).rgb;
  vec3 bloom=texture(uBloom,vUv).rgb;
  vec3 c = (uView==1) ? bloom*uIntensity
         : (uView==2) ? scene
         : scene + bloom*uIntensity;
  c*=uExposure;
  c=aces(c);
  c=pow(c, vec3(1.0/2.2));
  c+=(hash(gl_FragCoord.xy+uTime)-0.5)*uGrain;
  o=vec4(c,1.0);
}
```

-----

## 4. Pipeline orchestration (the part the shaders can’t show)

Bloom is 90% *driver code*. Per frame:

```
# targets: scene = HDR full-res ; mips[0..N] = HDR, half-res then halving
buildTargets(): scene = RGBA16F(W,H)
                mips[i] = RGBA16F(W>>(i+1), H>>(i+1)), while min(w,h) > 4

render():
  # PASS 0 — scene to HDR (no tone-map)
  draw(sceneShader -> scene)

  # PASS 1 — downsample chain; first hop does the bright-pass
  for i in 0..N:
    src = (i==0) ? scene : mips[i-1]
    set uTexel = 1/src.size ; uPrefilter = (i==0)
    bind src ; draw(downShader -> mips[i])

  # PASS 2 — upsample chain, ADDITIVE, smallest -> mip0
  enable BLEND ; blendFunc(ONE, ONE)
  for i in N..1:
    set uTexel = 1/mips[i].size ; uRadius
    bind mips[i] ; draw(upShader -> mips[i-1])    # adds onto existing content
  disable BLEND

  # PASS 3 — composite to screen
  bind scene -> unit0 ; bind mips[0] -> unit1
  draw(compShader -> screen)
```

### Render-target setup (WebGL2)

```js
const extF = gl.getExtension('EXT_color_buffer_float');   // enables RGBA16F render
gl.getExtension('OES_texture_float_linear');               // linear filter on float
const FMT = extF
  ? { internal: gl.RGBA16F, format: gl.RGBA, type: gl.HALF_FLOAT }   // HDR
  : { internal: gl.RGBA8,   format: gl.RGBA, type: gl.UNSIGNED_BYTE }; // LDR fallback
// textures: MIN/MAG = LINEAR, WRAP = CLAMP_TO_EDGE  (linear + clamp are essential
// for the bilinear taps and to avoid edge bleed)
```

-----

## 5. Tuning & pitfalls

|Want…                        |Do                                           |
|-----------------------------|---------------------------------------------|
|Wider max glow               |add mip levels (each ~doubles reach, ~free)  |
|Softer without more mips     |raise `radius` (tent spread)                 |
|Only very bright sources glow|raise `threshold`                            |
|Smoother onset (less ring)   |raise `softKnee`                             |
|Less washed-out highlights   |lower `intensity` or `exposure`              |
|Punchier filmic highlights   |keep ACES; avoid plain Reinhard (desaturates)|

**Pitfalls that bite:**

- **LDR scene buffer** → bloom has nothing to extract. The scene target *must* be float/HDR, and the scene shader must **not** tone-map.
- **Karis on every mip** → over-dark, lifeless bloom. Prefilter pass only.
- **Copy instead of add on upsample** → ringing and flat plateaus. Use additive blend.
- **NEAREST filtering on mips** → blocky bloom. Float targets need `LINEAR` (requires `OES_texture_float_linear`).
- **Forgetting `EXT_color_buffer_float`** → FBO incomplete on RGBA16F. Check it, fall back to RGBA8.
- **Fireflies when animated** → that’s the Karis average’s job; if still present, clamp scene luminance pre-bloom.