# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

OpenGL 3.3 Core renderer that draws an animated neon-style glow along the perimeter of a rounded rectangle, plus companion layers (rain droplets, lens flare). macOS arm64, CMake + GLFW + GLAD + GLM, ImGui for the debug UI.

The legacy stroke/particle/path system was removed in favour of a smaller neon-focused pipeline.

Docs, in reading order. The three neon documents are tiers of the same material - pick one by how much depth you need, not all three:

- [`docs/implementation.md`](docs/implementation.md) - brief: how the library is put together on the C++ side and how a frame runs. Start here.
- [`docs/neon-renderer-overview.html`](docs/neon-renderer-overview.html) - the renderer in ~7 minutes: one quad, two measurements, three layers of light.
- [`docs/neon-renderer-explained.html`](docs/neon-renderer-explained.html) - the same ground at length, with the reasoning and the bugs behind each decision.
- [`docs/neon-renderer-reference.html`](docs/neon-renderer-reference.html) - full mechanism reference: every uniform, constant, derivation and gating rule, plus the droplet and flare term stacks. Sections 18 and 19 are flow charts - what the CPU rebuilds and which GL object each bake writes, one frame's draw sequence for both renderers, and the fragment program as annotated GLSL dataflow. Read this before changing a shader.
- [`docs/effect-reference.md`](docs/effect-reference.md) - per-parameter reference and recipes.
- [`docs/architecture-design.md`](docs/architecture-design.md) - full architecture. Note it predates the droplets and lens-flare renderers.
- [`docs/coordinate-system.md`](docs/coordinate-system.md), [`docs/multiple-arcs-design.md`](docs/multiple-arcs-design.md).
- [`docs/neon-unification-plan.md`](docs/neon-unification-plan.md) - how the half-res neon fork was folded into `NeonRenderer` as a resolution scale, and the debug overlays split into `DebugRenderer`. Read it if a doc or comment still refers to `NeonOptimizedRenderer`.
- [`docs/neon-unification-comparison.md`](docs/neon-unification-comparison.md) - the evidence for that plan: eleven scenes rendered on both sides of the merge, nine byte-identical, the two that differ confined to the bounding box's compositing order.
- [`docs/lens-flare-unification-comparison.md`](docs/lens-flare-unification-comparison.md) - the same for the lens flare pair, the last fork in the tree: twelve scenes, all byte-identical, plus the two defects the merge closed (an unclamped resolution scale and the double-draw).
- [`docs/review-findings.md`](docs/review-findings.md) - open defects and rough edges, visual ones with offscreen repros. Check here before assuming a behaviour is intended.
- [`docs/naming-review.md`](docs/naming-review.md) - identifier audit against `AGENTS.md`, plus the names that describe mechanisms the code no longer has. Read before renaming anything.

When the docs go out of date, treat the headers under `lib/include/` as the source of truth.

## Build & run

Configure once, then rebuild from `build/`:

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/demo/edge-lighting-demo
```

There is no test target. The build produces four artifacts:

- `build/lib/libedge-lighting.a` - the C++ static library
- `build/lib/libedge-lighting-c.dylib` - flat `extern "C"` shared library for FFI / P-Invoke
- `build/demo/edge-lighting-demo` - demo driving the C++ library directly
- `build/demo-capi/edge-lighting-capi-demo` - the same UI driving only the C ABI

`RES_DIR` is baked into both demo binaries as a compile definition pointing at the in-tree `res/` directory, so they can be launched from anywhere.

Third-party image assets under `res/` (the `.jpg` files from Unsplash) are covered by the [Unsplash License](https://unsplash.com/license); see `res/CREDITS.md` for the per-file attribution table.

GLFW is an imported shared library at `external/lib/<arch>/libglfw.3.dylib` (arch picked from `CMAKE_OSX_ARCHITECTURES` or the host); GLAD is built from `external/src/glad.c`. ImGui sources are compiled directly into both demo targets.

The root `CMakeLists.txt` has `PLATFORM_WINDOWS` / `PLATFORM_LINUX` branches that select `#version 300 es`, but only macOS is actually buildable today - `external/lib/` ships macOS binaries only and the imported GLFW location is hardcoded to `libglfw.3.dylib`. Treat the non-Apple branches as unfinished scaffolding.

## Shaders are embedded at configure time

Shader sources under `lib/shaders/*.{vert,frag}` are read by `lib/CMakeLists.txt` and substituted into `shaders.h.in` via `configure_file()`, producing `build/lib/generated/shaders.h` with each shader as a `const char* const` raw string literal in `EdgeLighting::ShaderSource::*`. There is no runtime file I/O for shaders. `@GLSL_VERSION@` supplies the version line and `@NEON_TUNING@` injects `lib/include/renderer/neon-tuning.h`, so the tuning constants are shared verbatim between the shaders and the C++ renderers.

`CMAKE_CONFIGURE_DEPENDS` lists every shader file *and* `neon-tuning.h`, so editing any of them triggers a re-configure on the next build. **If you add a new shader you must update three places**: `lib/CMakeLists.txt` (both the `CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)` lists) and `lib/shaders/shaders.h.in`.

## Architecture

### Orchestrator + renderer plugins

`EdgeLighting::EdgeLightingEffect` (`lib/include/core/edge-lighting.h`) owns:

- a **base** `Config` (geometry + per-renderer sub-configs) - what `SetConfig` writes
- an **active** `Config` - base + animation overlays, what renderers actually see
- a `Clock` (play/pause time accumulator)
- an `AnimationManager` (held by `unique_ptr`)
- a `vector<shared_ptr<BaseRenderer>>` registered by the host

Per-frame contract: `Update(dt)` ticks the clock, advances every attached animation by the clock delta, rebuilds the active config, then forwards `(dt, clockTime, activeConfig)` to every renderer; `Render(w, h)` does the same for drawing. Both `SetConfig` and the per-frame refresh notify renderers via `OnConfigChanged` - but only when the composited config actually changed, so renderers can rely on that call being meaningful. Renderers are independent visual layers and composite by blending - enable any subset.

Four renderers, all under `lib/include/renderer/`, all registered by the demo in this order:
- `NeonRenderer` - the neon stroke. Analytic rounded-box SDF plus a gather loop over `NeonConfig::numSamples` perimeter samples (positions in a UBO), reading three baked LUT textures: `uGradientLUT` (base colour ring), `uSegmentLUT` (per-segment gradient atlas, one row per segment), `uArcLUT` (per-arc atlas). All LUTs are baked on the CPU as **RGBA8** - float textures are deliberately avoided for edge-device compatibility. Also owns the opaque-fill pass (`black-rect.frag`).

  **One renderer, two resolution paths**, selected by `NeonConfig::resolutionScale`: at `1.0` the gather draws straight onto the framebuffer it was handed (no offscreen buffer, no blit); below `1.0` it draws into a buffer of that fraction of the viewport and is bilinear-blitted back (`neon-blit.frag`). The paths share one pass schedule - every pixel-valued uniform is multiplied by the scale unconditionally (a no-op at `1.0`) and the shader converts `neon-tuning.h`'s own full-res px constants with `uResolutionScale`. Only the render target, the blit and the buffer allocation are conditional, which is what keeps `1.0` bit-identical to the dedicated full-res renderer this replaced. The opaque fill is the exception: always full-res on the caller's framebuffer, since its analytic SDF edge is the whole point of it.

  Runs an **emission pre-pass** (`neon-emission.frag`): the gather's per-sample
  work (arc winner-take-all, segment bells, LUT fetches) is a pure function of
  `(si, uTime, config)`, so it is baked once per frame into an `N x 2` RGBA16F
  table and the gather reads it with `texelFetch`. Per-fragment cost is
  `O(samples)` instead of `O(samples * (arcs + segments))`. The invariant to
  preserve: **pure function of `(si, uTime, config)` goes in the pre-pass;
  anything reading `vPos` stays in the main shader.**

  The table is `NEON_MAX_LOOP_SAMPLES x 2` - the sample-count CEILING, with
  `numSamples` bounding only how much of it the gather reads - so its size is a
  compile-time constant and it is allocated **once, in `Initialize`**, not per
  frame. `Initialize` fails if no candidate format (`RGBA16F`, then `RGBA8`)
  allocates. Size it to the live count instead and it goes straight back onto
  the per-frame path.

  `Render` is a **pass schedule**: derive the transform,
  then one call per `render*Pass` method. `Render` owns blend state; a pass owns
  its shader and, if it retargets, restores the framebuffer / viewport / blend
  it was handed - never framebuffer 0, never a forced `glEnable` (an
  `OffscreenCapture` hands the renderer a real FBO). Header declaration order,
  .cpp definition order and the pass numbering all agree; the one deliberate
  exception is documented at the declaration. See
  [`docs/emission-prepass.md`](docs/emission-prepass.md) for the pass tables and
  [`docs/emission-prepass-comparison.md`](docs/emission-prepass-comparison.md)
  for the measured before/after of the pre-pass commit alone.
  [`docs/branch-vs-main-comparison.md`](docs/branch-vs-main-comparison.md) is
  the wider view: the whole branch against `main`, so it also covers the
  colour-stop alpha and stop-sorting behaviour changes that ship with it.
- `DebugRenderer` - every debug annotation, in one layer: the baked ring as a LUT strip (`neon-lut-debug.frag`), one disc per colour stop (`neon-stop-marker.frag`), and the 1px `GL_LINE_LOOP` bounding box (`wireframe.frag`, absorbed from the old `WireframeRenderer`), behind `DebugConfig::showGradientLUT` / `showColorStops` / `showWireframe`. Register it **after** `NeonRenderer` - it annotates what that layer drew. Always full-res, whatever the neon's resolution scale. Reads `Config::debug` for what to draw and `Config::neon` for what it is describing. The strip and the markers annotate the GLOW and are suppressed when it is absent (neon off, or `debug.opaqueOnly`); the box annotates the GEOMETRY and survives both. Note the box now draws **over** the glow - `WireframeRenderer` was registered first and drew under it, and the overlays that annotate the glow have to follow it. Bakes its **own** `GradientRingLUT` from the same inputs, which is what keeps `NeonRenderer` free of every debug member.
- `DropletsRenderer` - rain-on-glass droplets in a band hugging the perimeter; screen-space gravity, self-lit drops, no framebuffer capture.
- `LensFlareRenderer` - sun + hex-aperture flare (rays, chromatic ghosts) as a fullscreen premultiplied-alpha pass. The sun rides the perimeter in the same parameter space as neon segments/arcs. Like `NeonRenderer` it is **one renderer with two resolution paths**, selected by `LensFlareConfig::resolutionScale`; unlike the neon's, only two uniforms differ between them (`uResolution` and `uSunPos`), because the flare shader normalises every term by the resolution and is therefore scale invariant.

There are no longer any forked renderer pairs. `NeonOptimizedRenderer` / `neon-optimized.frag` were folded into `NeonRenderer` / `neon.frag` as a resolution scale (see [`docs/neon-unification-plan.md`](docs/neon-unification-plan.md)), and `LensFlareOptimizedRenderer` was folded into `LensFlareRenderer` the same way. Both merges are byte-identical at every scale tested. A change to neon or flare appearance now lands in exactly one place.

To add a renderer, subclass `BaseRenderer` (`Initialize` / `Update` / `Render` / `OnConfigChanged`), add a sub-config struct to `Config` with `operator==`, register it in `demo/src/main.cpp`, and add an ImGui section in `DebugUI`.

### Animation: Clock + Modulators + Animations

Three layers, low to high:

- **`Modulator`** (`animation/modulator.h`) - header-only pure functions `time -> float`. `Constant`, `Oscillator` (SINE/TRIANGLE/SQUARE/SAWTOOTH), `Ease` (with an `EasingFunction::Curve` pointer), `Sequence`, `Multiplier`, `Adder`, `Remap`. No coupling to `Config`.
- **`Animation`** (`animation/animation.h`) - owns modulator(s) *plus* its own play state (`AnimationState`), elapsed accumulator, `PlaybackMode` (LOOP / ONE_SHOT) and `EndAction` (HOLD_CURRENT / HOLD_END / HOLD_START / RESTORE). Two-phase: `Update(dt)` advances time, `Apply(cfg)` writes into a config. `AnimationGroup` is itself an `Animation`. Concrete subclasses live in `animation/neon-animations.h` (`IntensityPulse`, `ArcWipe`, `SegmentTravel`, `OutlineTracer`, ...); `FieldBoundAnimation` (`animation/field-bound-animation.h`) instead binds modulators to `AnimatableField` / `SegmentField` / `ArcField` / `ColorStopField` targets at runtime, phase-locked on one shared clock.
- **`AnimationManager`** (`animation/animation-manager.h`) - the attach list. `Attach` / `Detach` / `DetachAll`, then `Update(dt)` and `Apply(target)` fan out to every attached animation in attach order.

The effect embeds the manager, so the host does **not** hand-composite animations: attach via `effect.Attach(anim)`, call `anim->Play()`, and each `Update` rebuilds the active config as base + overlays. Read the composited result with `GetActiveConfig()` (useful for UI sliders that should follow animated values); `GetConfig()` returns the untouched base. There is no "revert to base" end action - `Detach` gives that behaviour.

### C ABI (`lib/capi/`)

`libedge-lighting-c` wraps the static library in a flat `extern "C"` surface for P-Invoke / ctypes / cgo. `edge-lighting-capi.h` is the single public include, aggregating `el-types.h` (enums, result codes, `EL_API`), `el-effect.h`, `el-animation.h`, `el-modulator.h`. Key points:

- Three opaque handle families - effect, animation, modulator - defined in `capi-internal.h`. Attaching an animation does not transfer ownership.
- Each effect handle carries a **staging `Config`**: every `el_effect_set_*` mutates staging and nothing else; every `el_effect_get_*` reads staging back, *not* the animation-overlaid active config. Staging reaches the effect in `el_effect_update`, which is the only place that calls `SetConfig` - so a host that sets config and then calls only `el_effect_render` renders the previous frame's config. `el_effect_capture` re-syncs staging from the effect's base.
- No C++ exception escapes the boundary; everything maps to an `el_result_e`.
- Enum ABI parity between the C++ enums and their `el_*` mirrors is enforced by a wall of `static_assert`s at the top of `capi-internal.h`. **If you reorder or renumber a C++ enum that has an `el_*` mirror, add/adjust the assert there** - append new values at the end to stay forward-compatible.
- Symbols are hidden by default (`CXX_VISIBILITY_PRESET hidden`); only `EL_API`-marked `el_*` functions are exported.

### RAII GL wrappers

`lib/include/gl/` provides move-only RAII wrappers: `ShaderProgram` (with uniform-location and last-value caching), `VertexArray`, `Framebuffer`, `UniformBuffer`, and `Texture` (base) + `Texture2D`. `gl-header.h` is the single include for `<glad/glad.h>`. Use these wrappers - do not call `glGen*` / `glDelete*` directly in renderer code. A multi-pass renderer must return to the framebuffer it was handed (`Framebuffer::GetBoundId()` before its offscreen pass, `Framebuffer::BindId(prev)` after), not to `BindDefault()`: during a frame capture the "backbuffer" is a real FBO.

To grab pixels, use `util/capture-util.h` (`OffscreenCapture` + `CaptureUtil::Read*` / `WritePNG`), never `glReadPixels` on the window's default framebuffer - post-swap backbuffer contents are undefined, and its size follows the platform's HiDPI backing scale. (`demo-capi/src/` is the exception: it deliberately cannot see `lib/include/`, so it has its own minimal GL helpers in `gl-mini.h`.)

### Demos

`demo/src/main.cpp` opens two GLFW windows that share a GL context: the main render window and a separate ImGui debug window (`DebugUI` in `demo/src/debug-ui.{h,cpp}`). The render loop calls `debugUI.Build(cfg, *gEffect)` to lay out widgets, then `debugUI.Render()` draws into the debug window, then makes the main window's context current to draw the effect. Hotkeys (`OnKey` in `main.cpp`) mutate `Config` directly and round-trip through `SetConfig`.

`demo-capi/` mirrors that UI but its include path deliberately excludes `lib/include/`, so it can only compile against the flat C ABI - that is the guard proving the ABI is self-sufficient for a real UI-shaped host. It is a hand-maintained fork of `demo/`: a change to one usually needs the same change in the other.

## Conventions

Naming and formatting are defined in `AGENTS.md` and enforced by hand - there is no formatter config. Key points:

- Files: `kebab-case.{h,cpp}`. Namespaces, classes, structs, enums, public methods, event callbacks: `PascalCase` (callbacks prefixed with `On`). Private methods, locals, parameters: `camelCase`. Enum values and constants: `ALL_CAPS_WITH_UNDERSCORES`.
- Variables: members `mFoo`, globals `gFoo`. Header guards `_NAME_OF_FILE_H_`.
- Structs/enums always get a `typedef` self-alias: `typedef struct Config { ... } Config;`, `typedef enum class Winding { ... } Winding;`.
- Every `Config` sub-struct needs `operator==` / `operator!=` covering all its fields - the effect's change detection and the renderers' dirty-flag gating both depend on it. Adding a field without adding it to `operator==` silently breaks rebuilds.
- Always brace single-statement bodies, including every `case` body inside a `switch`. See `AGENTS.md` for the canonical examples.
- Never use the em-dash character (Unicode U+2014); use a plain hyphen `-` instead (comments, doc comments, log strings, Markdown). Keep shader sources ASCII-only.
