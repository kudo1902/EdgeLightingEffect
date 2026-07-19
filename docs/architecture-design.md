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

Two demo applications live at the top level:

- `demo/` drives the C++ library directly with an ImGui debug UI.
- `demo-capi/` mirrors that UI but is compiled against **only** the flat C
  ABI header (`lib/capi/edge-lighting-capi.h`) - the guard that the ABI is
  self-sufficient for real UI-shaped hosts. See §12.

Both open a main render window plus a floating debug window that shares its GL
context.

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

The orchestrator holds a `Config`, a `Clock`, an `AnimationManager`, and a
vector of `shared_ptr<BaseRenderer>`. It advances the clock, lets the
manager composite the attached animations onto an *active* config each
frame, and forwards that active config to the renderers. Hosts only ever
author the *base* config; the manager owns the base↔active split (§5).

## 2. Directory layout

```
lib/
  include/core/       Config + Effect orchestrator + Clock
  include/renderer/   BaseRenderer + concrete renderers + neon-tuning.h
  include/animation/  Modulator family + Animation presets + AnimationManager
                      + FieldBoundAnimation
  include/gl/         Move-only RAII wrappers: ShaderProgram, VertexArray,
                      Texture(1D/2D), Framebuffer, UniformBuffer
  include/util/       geometry-utils, color-utils, contour-tracer, stb-image,
                      screenshot-util, log-util
  shaders/            .vert/.frag sources + shaders.h.in template
  src/                Renderer + effect + animation implementations
  capi/               Flat extern "C" ABI (edge-lighting-capi.{h,cpp})

demo/                 C++ demo - links libedge-lighting directly
  src/                Entry point + ImGui debug window + border-color-picker
                      + image-quad backdrop + background-quad

demo-capi/            C-API-only twin - links libedge-lighting-c and includes
                      lib/capi/ only. Feature parity with demo/.
  src/                main.cpp, debug-ui.{h,cpp}, background/image quads in
                      raw GL, border-color-picker rewired to el_effect_set_*

external/             GLFW binary, GLAD, GLM, ImGui, stb (vendored)
res/                  Demo images (see res/CREDITS.md)
```

## 3. `Config` - single-shot state

`Config` is a plain aggregate. All fields have sensible defaults, all sub-
structs implement `operator==` / `operator!=` so change detection is a
one-liner. The reader-facing enumeration of every field, its default, and
its visual effect lives in [`effect-reference.md`](effect-reference.md); the
brief here focuses on structure.

```
Config
 ├── RectGeometry        width, height, position, cornerRadius, winding
 ├── NeonConfig          full-res neon knobs:
 │                       - filament: lineWidth, filamentFalloff
 │                       - glow: intensity, glowRadius, bloomStrength,
 │                         glowSide, glowSideSoftness
 │                       - colour: blendSpace, colorStops, hueRotationRate,
 │                         colorTransitionDuration
 │                       - arcs: vector<Arc> (each with start, length,
 │                         intensity, own colorStops + blendSpace).
 │                         Default = one arc covering the whole perimeter.
 │                       - segments: vector<SegmentBoost> (position, length,
 │                         boost, own colorStops + blendSpace). Default empty.
 │                       - compositing: opaque + opaqueColor
 │                       - debug: showGradientLUT, showColorStops
 ├── OptimizedNeonConfig half-res knobs (enable, resolutionScale, numSamples,
 │                       gradientLutSize, showHalfRes). *Shares* NeonConfig
 │                       for every visual param above.
 └── WireframeConfig     enable + color
```

Host code produces a `Config` (usually a copy of `effect.GetConfig()`,
mutated, and pushed back via `SetConfig`). This round-trip is the *only*
way base-config state changes reach renderers. The active config (base +
animation overlays) is computed by the effect each frame - hosts never
author it directly (§5).

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
- **Three baked LUTs** stored as GL textures, each with `REPEAT` wrap on U
  so the gradient loops seamlessly at `s = 1 → 0`:
  - `mGradientLUT` (1D, 256 texels) - the base perimeter gradient built
    from `NeonConfig::colorStops`.
  - `mSegmentLUT` (2D, W × `MAX_SEGMENT_BOOSTS`) - row `i` holds
    segment `i`'s own gradient; empty rows are zero-filled and the
    shader falls back to the base LUT (§4.1a).
  - `mArcLUT` (2D, W × `MAX_ARCS`) - row `a` holds arc `a`'s own
    gradient; same "empty row → fall back to base" convention.
- **128 pre-computed perimeter loop samples** in a std140 UBO. Each fragment
  gathers all 128 with distance-weighted contribution → halo, filament core,
  bloom. Iteration count is compile-time constant (128) so the driver can
  unroll.
- **Winner-take-all arc gating** - for each perimeter sample the shader
  scans up to `MAX_ARCS` entries in `ArcBlock`, picks the arc with the
  largest `arcInside * intensity`, and uses *that* arc's colour + mask at
  the sample. Because `arcInside` is smoothstepped one-sample-wide at each
  end, adjacent arcs of different colours crossfade smoothly at the seam
  without any special blend logic (§4.1a).
- **Hue-preserving Reinhard tonemap** - peak channel drives compression,
  R/G/B scale by the same ratio. Prevents "orange → peach" desaturation.
- **Colour transition** - `mGradientLUT` cross-fades between the old and new
  baked textures over `NeonConfig::colorTransitionDuration` seconds when
  colour stops or blend space change. The fade blends the whole 256-texel
  LUT (not per-stop pairing), so it works even when stop counts differ.
- **Opaque-mode background pass** - a fullscreen NDC quad drawn *behind* the
  neon with `NeonConfig::opaqueColor`. Shape from an SDF read off
  `gl_FragCoord`; corners AA cleanly via `fwidth`.

### 4.1a Two colour-sampling spaces

The shader samples the LUTs in **two different spaces** depending on
which layer needs the colour:

- **Base gradient (`mGradientLUT`, and any arc/segment with empty stops)**
  is sampled in *perimeter space*: sample coordinate `ti = si + timeOffset`
  where `si ∈ [0, 1)` is the sample's position on the perimeter and
  `timeOffset = -uTime * uHueRotationRate` scrolls the ring. Empty-stops
  arcs / segments stay visually continuous with the rest of the perimeter.
- **Per-arc gradient (`mArcLUT` with non-empty stops)** is sampled in
  *arc-local* space: coordinate `uArc = (si - arc.start) / arc.length`,
  so stop `position=0` = arc start, `position=1` = arc end. The same
  `timeOffset` is added on top, so hue rotation "marches" the arc's
  gradient through the arc window (REPEAT wrap makes any coordinate valid).
- **Per-segment gradient (`mSegmentLUT` with non-empty stops)** is sampled
  in *segment-local* space with the same head-to-tail convention: the
  signed wrap-distance from the segment's centre maps `[-length/2, +length/2]`
  to `[0, 1]` on the segment's LUT row.

This split was the design outcome recorded in
[`multiple-arcs-design.md`](multiple-arcs-design.md); the perimeter-space
fallback for empty arcs preserves the pre-multi-arc single-slice behaviour.

### 4.2 NeonOptimizedRenderer

Two-pass half-resolution variant. Pass 1 renders into a scaled RGBA8 FBO
with a dynamic shader loop bound (`uNumSamples = optimizedNeon.numSamples`,
1..128). Pass 2 bilinear-blits back to full res. Shares all visual params
with `NeonConfig`. Meant for edge devices - the resolution-scale + sample-
count sliders are the primary perf knobs.

### 4.3 WireframeRenderer

A `GL_LINE_LOOP` debug outline. Blending briefly disabled for crisp 1 px
lines.

## 5. Animation - Clock + Modulators + AnimationManager

Animation is a three-layer stack, from raw math up to the finished frame:

1. **`Modulator`** (`lib/include/animation/modulator.h`) - pure functions
   `float(time) → float`. Concrete shapes: `Constant`, `Oscillator`
   (SINE/TRIANGLE/SQUARE/SAWTOOTH), `Ease` (with an `Easing::Curve`
   function pointer), `Sequence`, `Multiplier`, `Adder`, `Remap`. They
   compose freely; a modulator has no knowledge of `Config`.
2. **`Animation`** (`lib/include/animation/animation.h`) - a stateful
   playhead over a modulator. Owns `elapsed`, `duration`, `speed`, playback
   mode (`LOOP` / `ONE_SHOT`), and an end-action (`HOLD_CURRENT`,
   `HOLD_END`, `HOLD_START`, `RESTORE`). Exposes `Play` / `Pause` / `Stop`
   / `Reset` + `GetProgress` / `SetProgress` (elapsed / duration in [0, 1]).
   `ApplyAt(cfg, elapsed)` writes into a target field. Concrete presets
   (`IntensityPulse`, `SegmentTravel`, `ArcWipe`, `OutlineTracer`, ...)
   live in `neon-animations.h`.
3. **`AnimationManager`** (`lib/include/animation/animation-manager.h`) -
   owns the vector of attached animations and drives the base↔active
   `Config` split described below.

The effect owns one `AnimationManager` and holds both a **base** config
(the last thing `SetConfig` received - the "authored" values) and an
**active** config (the base with animation overlays applied). Each frame,
`Update(dt)` does:

```
clock.Update(dt)                        # returns 0 if paused
active = base                            # start from a fresh copy
manager.Apply(active, clock.GetTime())   # each attached animation writes its fields
for r in renderers: r.Update(dt, clock.GetTime(), active)
```

`Render(w, h)` forwards the *active* config. Hosts read `GetActiveConfig`
when they want the live per-frame value (e.g. a slider that follows an
oscillator's current output); they call `SetConfig` only to change the
authored base. Pausing the clock returns 0 from `Update`, so animations
freeze in lockstep without any per-animation state changes.

**`FieldBoundAnimation`** (`lib/include/animation/field-bound-animation.h`)
is a generic Animation subclass that binds one or more `Modulator`s to
enumerated `Config` fields (`AnimatableField`, `SegmentField`, `ArcField`,
`ColorStopField`). It's what the C API's `el_animation_add_*_field` calls
build, letting FFI hosts assemble arbitrary field→modulator bindings
without needing a per-field concrete Animation subclass.

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
- `MAX_ARCS` (arc array size)
- `NEON_MAX_LOOP_SAMPLES` (perimeter-gather loop / UBO array size)

### 7.2 UBOs

Three arrays cross the CPU/GPU boundary through **std140 UBOs**:

- `SegmentBlock` - one `vec4` per segment: `.xy` = `(position, invSigma)`,
  `.z` = `boost`, `.w` = `hasStops` flag (nonzero → sample the segment's
  own row in `mSegmentLUT`; zero → fall back to the base gradient).
- `ArcBlock` - one `vec4` per arc: `.x` = `start`, `.y` = `length`,
  `.z` = `intensity`, `.w` = `hasStops` (same convention as segments).
- `LoopSamplesBlock` - `vec4[NEON_MAX_LOOP_SAMPLES]` where `.xy` is the
  perimeter point in rect-local pixels.

Each renderer defines a POD mirror in its `.cpp`'s anonymous namespace with
a `static_assert` on `sizeof` against the expected std140 stride, so any
drift between the C++ layout and the shader array trips at compile time.

Bindings:
- `SEGMENT_BLOCK_BINDING = 0`
- `LOOP_SAMPLES_BLOCK_BINDING = 1`
- `ARC_BLOCK_BINDING = 2`

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

The C++ demo opens two GLFW windows sharing a GL context: the main render
surface and a floating **Debug Controls** panel. Per frame:

1. `base = effect.GetConfig()` - snapshot the authored config.
2. `debugUI.Build(base, effect)` - ImGui widgets mutate `base` in place;
   the Animations section attaches / detaches presets on
   `effect.GetAnimationManager()` and per-row controls drive `Play` /
   `Pause` / `Stop` / speed / duration / progress on each animation.
3. `effect.SetConfig(base)` - round-trip triggers `OnConfigChanged`, which
   gates rebuilds by dirtiness (§6). The manager is untouched here.
4. `debugUI.Render()` draws into the debug window.
5. Main context current → optional backdrops (checker / picker image) →
   `effect.Update(dt)` (advances clock, composites animations into the
   active config, forwards to renderers) → `effect.Render(fbW, fbH)` →
   `glfwSwapBuffers`.

Sliders that display an animated field read from `effect.GetActiveConfig()`
so the knob follows the live value; while the user is dragging the same
slider, it pins to the base to avoid a tug-of-war with the per-frame
overlay.

The picker (`demo/src/border-color-picker.{h,cpp}`) samples pixels along
an image's border and emits `ColorStop`s that snap to the rect's own
perimeter parameterisation. See `README.md` for user-facing knobs
(Stop Count, Contrast gamma, Auto-adjust intensity).

## 11. C API surface

`lib/capi/edge-lighting-capi.{h,cpp}` is a flat `extern "C"` ABI over the
same `EdgeLighting::EdgeLightingEffect` orchestrator. It is compiled into
`libedge-lighting-c.{dylib,so,dll}` for FFI / P/Invoke hosts.

**Handles** are opaque: `el_effect_handle_t`, `el_animation_handle_t`,
`el_modulator_handle_t`. Handle lifecycle:

- `el_effect_create` returns a handle that only owns a staging `Config` -
  no GL state, no renderers. Deliberately cheap so a host can abort before
  a live GL context is available.
- `el_effect_init` lazily constructs the underlying
  `EdgeLightingEffect`, attaches the three built-in renderers, and calls
  `Initialize()`. Must be called on the GL thread with a current context.
- `el_effect_destroy` tears everything down.

**Config staging.** The API uses field-by-field setters/getters against the
staging `Config` held on the handle, rather than a flat POD mirror. Each
`el_effect_set_*` call mutates the staging config; `el_effect_update`
flushes it into the effect via `SetConfig` and then ticks the clock +
animations + renderers. `el_effect_capture` pulls the effect's current
base config back into staging when the host needs to re-sync.

**Setters short-circuit unchanged writes.** The `SET_AND_LOG` helper
compares the incoming value to the staging field first and returns
`EL_OK` without logging when they match. This keeps a debug UI's
"get → widget → set every frame" pattern from flooding the log.

**Getters are quiet by default on macOS + Windows.** The reads use `LOG_D`;
`edge-lighting-capi.cpp` platform-conditionally undefines and redefines
`LOG_D` locally as a no-op on `PLATFORM_MACOS` / `PLATFORM_WINDOWS`. On
Linux the log-util.h definition is left intact (DEBUG-level print). The
override is `#undef`'d at the file's bottom so it doesn't leak.

**Multi-arc / per-segment stops surface.** For every array in the base
`Config` there is a `_count` getter/setter and per-index accessor:

- `el_effect_set_color_stop_count` + `el_effect_set_color_stop(i, ...)`
- `el_effect_set_segment_boost_count` + `el_effect_set_segment_boost(i, ...)`
  + `el_effect_set_segment_blend_space(i, ...)`
  + `el_effect_set_segment_color_stop_count(i, ...)` +
    `el_effect_set_segment_color_stop(i, j, ...)`
- `el_effect_set_arc_count` + `el_effect_set_arc(i, start, length, intensity, blendSpace)`
  + `el_effect_set_arc_color_stop_count(i, ...)` +
    `el_effect_set_arc_color_stop(i, j, ...)`

Growing an array is `set_..._count(n+1)` followed by
`set_..._at(n, ...)`; removing entry `k` is a shift-down loop plus
`set_..._count(n-1)` (both C++ and capi demos do this).

**Clock.** `el_effect_clock_play` / `el_effect_clock_pause` /
`el_effect_clock_is_playing` mirror `Clock::Play` / `Pause` /
`IsPlaying`. Needed because the C++ `Clock&` reference is not exposed.

**Animations.** `el_animation_create(EL_ANIM_*)` builds one of the built-in
presets. Lifecycle: `el_animation_play` / `_pause` / `_stop` /
`_reset` / `_destroy`. Playhead: `_get_elapsed` / `_set_elapsed` /
`_get_progress` / `_set_progress` (progress = elapsed / duration in
[0, 1]; one-shot completion clamps elapsed to duration so progress
returns 1.0 after the animation lands, not 0.0). Params: speed,
duration, playback-mode, end-action. Callbacks:
`el_animation_set_on_complete_callback` and
`el_animation_set_on_state_changed_callback`, both with `void *userData`.

The manager is reached via `el_effect_attach_animation` /
`_detach_animation` / `_detach_all_animations` /
`_get_animation_count` / `_contains_animation`.

For hosts that want to bind arbitrary fields without a per-field preset,
`el_animation_create_field_bound()` + `el_animation_add_field(...)` /
`_add_segment_field(...)` / `_add_arc_field(...)` /
`_add_arc_stop_field(...)` / `_add_segment_stop_field(...)` build a
`FieldBoundAnimation` (§5) from `Modulator` handles.

**Modulators.** `el_modulator_create_constant` / `_oscillator` / `_ease` /
`_sequence` (+ `_sequence_append`) / `_multiplier` / `_adder` / `_remap`.
`el_modulator_evaluate` reads a raw value at a given time (useful for
tests and previews).

**ABI safety net.** Enum parity between the C++ enums and their `el_*_e`
mirrors is enforced by `static_assert` blocks at the top of
`edge-lighting-capi.cpp`. `el_bool_t` = `int32_t` typedef gives the
ABI a fixed-width boolean.

## 12. `demo-capi/` - the C-API-only twin

`demo-capi/` is a full-feature-parity demo compiled against **only**
`lib/capi/edge-lighting-capi.h`. Its CMake target deliberately omits
`lib/include/` from the include path so any accidental
`#include "core/..."` fails to build - the guard that the ABI covers what a
real UI-shaped consumer needs.

Structure:

- `main.cpp` - GLFW window, GLAD load, effect lifecycle, hotkeys - all
  through `el_effect_*`.
- `debug-ui.{h,cpp}` - the ImGui panel. Every widget reads via the getter
  and writes via the setter on change. Segment / arc rows use the
  count-based shift-down pattern above for add / remove.
- `background-quad.h`, `image-quad.h`, `gl-mini.h` - raw-GLAD ports of
  the C++ demo's helpers (no `EdgeLighting::ShaderProgram` /
  `VertexArray` wrappers, since those live under `lib/include/gl/`).
- `border-color-picker.{h,cpp}` - stb_image loader + border sampler.
  `SampleBorder` returns plain `SampledStop` structs that the debug UI
  pushes through `el_effect_set_color_stop_count` +
  `el_effect_set_color_stop`. Auto-intensity computes a fresh
  `intensity` from the brightest sampled channel and writes it via
  `el_effect_set_intensity`.
- `stb-image-impl.c` - one C translation unit that instantiates
  `stb_image.h`'s implementation for this target.

`demo-capi/CMakeLists.txt` links `edge-lighting-c` + `glfw` + `glad`.
Enable getter logs with `-DEL_ENABLE_LOG_D=1` when compiling
`edge-lighting-capi.cpp` (see §11).
