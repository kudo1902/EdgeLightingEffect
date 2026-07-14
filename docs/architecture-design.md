# Architecture

A short tour of how the pieces fit together and why. For build/run and hotkeys
see [`README.md`](../README.md); for naming/formatting rules see
[`AGENTS.md`](../AGENTS.md).

## 1. Big picture

`EdgeLightingEffect` is an **orchestrator + renderer plugins** system built
around one `Config` struct. The library exposes:

- A C++ static library (`libedge-lighting.a`) intended to be embedded.
- A flat `extern "C"` shared library (`libedge-lighting-c.dylib`) for FFI/
  P/Invoke.

A demo application under `demo/` drives it with an ImGui debug UI (in its own
window sharing a GL context with the main render window).

```
             ┌──────────────────────┐
Config ────► │ EdgeLightingEffect   │◄──── Clock (Play/Pause/Reset)
             │                      │
             │  vector<Renderer>    │  ─── per frame ───►  GL context
             └──────────────────────┘
                       │
                       │  Update(dt, t, config)
                       │  Render(w, h, t, config)
                       ▼
       ┌─────────────────────────────────────────┐
       │  WireframeRenderer  (debug outline)     │
       │  NeonRenderer       (full-res neon)     │
       │  NeonOptimizedRenderer (half-res neon)  │
       └─────────────────────────────────────────┘
```

The orchestrator is deliberately dumb: it holds a `Config`, a `Clock`, and a
vector of `shared_ptr<BaseRenderer>`. It does not mutate config values, does
not animate anything, and does not know what the renderers do. Everything
interesting lives in the renderers or in the host.

## 2. Directory layout

```
lib/
  include/core/       Config + Effect orchestrator + Clock
  include/renderer/   BaseRenderer + concrete renderers + neon-tuning.h
  include/animation/  Modulator family + Animation presets (host-side)
  include/gl/         Move-only RAII wrappers: ShaderProgram, VertexArray,
                      Texture(1D/2D), Framebuffer, UniformBuffer
  include/util/       geometry-utils, color-utils, contour-tracer, stb-image,
                      screenshot-util, log-util
  shaders/            .vert/.frag sources + shaders.h.in template
  src/                Renderer + effect implementations
  capi/               extern "C" ABI + C# interop scaffolding

demo/
  src/                Entry point + ImGui debug window + border-color-picker
                      + image-quad backdrop + background-quad

external/             GLFW binary, GLAD, GLM, ImGui, stb (vendored)
res/                  Demo images (see res/CREDITS.md)
```

## 3. `Config` - single-shot state

`Config` is a plain aggregate. All fields have sensible defaults, all sub-
structs implement `operator==` / `operator!=` so change detection is a
one-liner.

```
Config
 ├── RectGeometry        width, height, position, cornerRadius, winding
 ├── NeonConfig          full-res neon knobs (enable, lineWidth, intensity,
 │                       glow radius, bloom, colorStops, hueRotationRate,
 │                       arcStart/arcLength, segmentBoosts, opaque + color)
 ├── OptimizedNeonConfig half-res knobs (enable, resolutionScale, numSamples,
 │                       gradientLutSize). *Shares* NeonConfig for visual
 │                       params (line width, colour stops, etc.).
 └── WireframeConfig     enable + color
```

Host code produces a `Config` (usually a copy of `effect.GetConfig()`,
mutated, and pushed back via `SetConfig`). This round-trip is the *only*
way state changes reach renderers.

## 4. Rendering pipeline

Per frame the host calls `Update(dt)` then `Render(w, h)`. The effect ticks
its `Clock` and forwards to each renderer:

```
effect.Update(dt)
  clock.Update(dt)                         # only advances if playing
  for r in renderers: r.Update(dt, clock.time, config)

effect.Render(w, h)
  for r in renderers: r.Render(w, h, clock.time, config)
```

Renderers composite via **additive blending** - enable any subset and layers
sum in HDR.

### 4.1 NeonRenderer (`lib/src/renderer/neon-renderer.cpp` + `shaders/neon.frag`)

Single-pass full-resolution neon stroke. Highlights:

- **Analytic rounded-box SDF** for the filament shape.
- **Precomputed 1D LUT texture** (RGBA8, 256 px, REPEAT wrap) holding the
  colour ring. Each shader sample is one texture lookup instead of an
  in-shader stops loop.
- **128 pre-computed perimeter loop samples** in a std140 UBO. Each fragment
  gathers all 128 with distance-weighted contribution → halo, filament core,
  bloom. Iteration count is compile-time constant (128) so the driver can
  unroll.
- **Hue-preserving Reinhard tonemap** - peak channel drives compression,
  R/G/B scale by the same ratio. Prevents "orange → peach" desaturation.
- **Opaque-mode background pass** - a fullscreen NDC quad drawn *behind* the
  neon with `NeonConfig::opaqueColor`. Shape from an SDF read off
  `gl_FragCoord`; corners AA cleanly via `fwidth`.

### 4.2 NeonOptimizedRenderer

Two-pass half-resolution variant. Pass 1 renders into a scaled RGBA8 FBO
with a dynamic shader loop bound (`uNumSamples = optimizedNeon.numSamples`,
1..128). Pass 2 bilinear-blits back to full res. Shares all visual params
with `NeonConfig`. Meant for edge devices - the resolution-scale + sample-
count sliders are the primary perf knobs.

### 4.3 WireframeRenderer

A `GL_LINE_LOOP` debug outline. Blending briefly disabled for crisp 1 px
lines.

## 5. Animation - Clock + Modulators

The effect only advances its clock; it never touches `Config`. Parameter
animation lives entirely in the host, via the **Modulator** family
(`lib/include/animation/modulator.h`).

Modulators are pure functions `float(time) → float`:
`Constant`, `Oscillator` (SINE/TRIANGLE/SQUARE/SAWTOOTH), `Ease` (with an
`Easing::Curve` function pointer), `Sequence`, `Multiplier`, `Adder`,
`Remap`. They compose freely.

Intended host pattern:

```cpp
Config cfg = effect.GetConfig();
cfg.neon.intensity = intensityMod.Evaluate(clock.GetTime());
cfg.neon.arcLength = arcMod.Evaluate(clock.GetTime());
effect.SetConfig(cfg);
```

This decoupling means pausing the clock freezes every animation without
special-casing anywhere.

## 6. Change detection - equality-gated rebuilds

Every `Config` sub-struct defines `operator==` field-wise. Renderers use it
in `OnConfigChanged` to skip rebuilds when the fields the rebuild reads
haven't changed:

```cpp
void NeonRenderer::OnConfigChanged(const Config &config)
{
    const bool samplesDirty  = config.geometry != mCurrentConfig.geometry;
    const bool geometryDirty = samplesDirty
                            || config.neon.glowRadius    != mCurrentConfig.neon.glowRadius
                            || config.neon.bloomStrength != mCurrentConfig.neon.bloomStrength
                            || config.neon.intensity     != mCurrentConfig.neon.intensity;
    const bool lutDirty      = config.neon.colorStops != mCurrentConfig.neon.colorStops
                            || config.neon.blendSpace != mCurrentConfig.neon.blendSpace;

    mCurrentConfig = config;
    if (!mShaderProgram.IsValid()) { return; }

    if (samplesDirty)  { rebuildLoopSamples(config); }  // updates mSampleSpacing
    if (geometryDirty) { setupGeometry(config); }
    if (lutDirty)      { rebuildGradientLUT(config); }
}
```

A slider drag on an unrelated field (e.g. `bloomStrength`) triggers *only*
the geometry-quad refresh, not the 256-entry LUT rebake or the perimeter walk.

## 7. Shader ↔ C++ interop

### 7.1 Shared tuning constants

`lib/include/renderer/neon-tuning.h` is `#define`-only and text-injected into
both the fragment shaders (via `@NEON_TUNING@` in `shaders.h.in`) and the C++
renderers (via ordinary `#include`). This is the only way to share literal
values between GLSL ES 3.0 (no `constexpr`, no `f` suffix) and C++ from one
source of truth. Currently holds:

- Filament / halo / bloom gains (`FILAMENT_GAIN`, `HALO_GAIN`, `BLOOM_*`)
- Tone map shoulder + gamma
- Early-out quad-sizing factors
- `MAX_SEGMENT_BOOSTS` (segment array size)
- `NEON_MAX_LOOP_SAMPLES` (perimeter-gather loop / UBO array size)

### 7.2 UBOs

Both the segment-boost array and the perimeter loop samples cross the
CPU/GPU boundary through **std140 UBOs**:

- `SegmentBlock` - `int count + vec3[8]` (padded to vec4 stride).
- `LoopSamplesBlock` - `vec4[NEON_MAX_LOOP_SAMPLES]` where `.xy` is the
  perimeter point in rect-local pixels.

Each renderer defines a POD mirror in its `.cpp`'s anonymous namespace with a
`static_assert` on `sizeof` against the expected std140 stride, so any drift
between the C++ layout and the shader array trips at compile time.

Bindings:
- `SEGMENT_BLOCK_BINDING = 0`
- `LOOP_SAMPLES_BLOCK_BINDING = 1`

### 7.3 Shader embedding

Shaders are text-injected at CMake **configure** time. `lib/CMakeLists.txt`
`file(READ ...)`s each `.vert/.frag`, `configure_file()`s them into
`build/lib/generated/shaders.h` (via `shaders/shaders.h.in`), and lists them
in `CMAKE_CONFIGURE_DEPENDS` so editing a shader triggers a re-configure on
the next build. Adding a new shader means updating three spots:
`CMAKE_CONFIGURE_DEPENDS`, `file(READ ...)`, and `shaders.h.in`.

Runtime shader loading is *not* implemented - every shader edit currently
requires a rebuild of the TU that includes `shaders.h`.

## 8. RAII GL wrappers

Everything under `lib/include/gl/` wraps a GL handle in a move-only class
that generates on construction and deletes on destruction. Renderer code
must not touch `glGen*` / `glDelete*` directly.

- `ShaderProgram` - compile + link a vert/frag pair. Typed `SetUniform`
  overloads (int, float, vec2/3/4, mat4, arrays). Per-uniform value cache
  skips redundant GL calls.
- `VertexArray` - VAO + VBO with `SetVertexData` / `SetAttribPointer` /
  `DrawArrays`.
- `Texture` (base) + `Texture1D` / `Texture2D` - `Bind(unit)`, `SetData`,
  `SetParams`, plus `Texture2D::SetDataFromFile` (stb_image).
- `Framebuffer` - with a colour texture, resize-idempotent.
- `UniformBuffer` - std140-shaped buffer with a byte-level upload cache
  (skips `glBufferData` when the block bytes are unchanged).

## 9. Coordinate spaces

Four spaces show up in the code. Conversions collect at renderer boundaries.

| Space | Origin | +X | +Y | Used by |
|---|---|---|---|---|
| **App** | viewport top-left | right | down | `Config::geometry::position`, `RectGeometry`, mouse input |
| **Local** | rect center | right | up | SDF, `GetPointOnRectangle`, loop samples in the UBO |
| **OpenGL viewport** | viewport bottom-left | right | up | model matrix `translate` |
| **NDC** | screen center | right | up | `gl_Position` |

The app→OpenGL Y flip is one line:

```cpp
center_ogl.y = viewportH - position.y - halfH;
```

All renderers use the same MVP formula so local-space vertices (origin at
rect center, +Y up) render correctly on screen (origin at top-left, +Y down).

## 10. Demo - the ImGui side

The demo opens two GLFW windows sharing a GL context: the main render surface
and a floating **Debug Controls** panel. Per frame:

1. Copy `effect.GetConfig()` into a local `cfg`.
2. `debugUI.Build(cfg, effect)` - ImGui widgets mutate `cfg` in place.
3. `debugUI.ApplyActiveAnimation(cfg, clockTime)` - any preset Modulators
   overwrite fields they animate.
4. `effect.SetConfig(cfg)` - round-trip triggers `OnConfigChanged`, which
   gates rebuilds by dirtiness.
5. `debugUI.Render()` draws into the debug window.
6. Main context current → optional backdrops (checker / picker image) →
   `effect.Render(fbW, fbH)` → `glfwSwapBuffers`.

The picker (`demo/src/border-color-picker.{h,cpp}`) samples pixels along an
image's border and emits `ColorStop`s that snap to the rect's own perimeter
parameterisation. See the section in `README.md` for the user-facing feature
and its knobs (Stop Count, Contrast gamma, Auto-adjust intensity).

## 11. C API surface

`lib/capi/edge-lighting-c.{h,cpp}` mirrors `Config` as a POD `EL_Config`
(flat fields, fixed-size arrays for stops/segments). Marshalling functions
copy between the two representations; the C++ effect is held behind an
opaque `EL_Effect*`.

`EL_ABI_VERSION` (currently 8) bumps on any struct-layout or enum-value
change. The C# interop scaffolding under `lib/capi/csharp/` targets this
surface but has not been kept in lockstep with recent ABI bumps - treat it
as stale until refreshed.
