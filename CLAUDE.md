# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

OpenGL 3.3 Core renderer that draws an animated neon-style glow along the perimeter of a rounded rectangle. macOS arm64, CMake + GLFW + GLAD + GLM, ImGui for the debug UI.

The repo is currently on the `big_refactor` branch — the legacy stroke/particle/path/animation system was removed in favour of a smaller neon-focused pipeline. `docs/architecture-design.md` describes the old design and is stale; treat the headers under `lib/include/` as the source of truth.

## Build & run

Configure once, then rebuild from `build/`:

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/demo/edge-lighting-demo
```

There is no test target. The build produces:

- `build/lib/libedge-lighting.a` — the library
- `build/demo/edge-lighting-demo` — the demo executable

`RES_DIR` is baked into the demo binary as a compile definition pointing at the in-tree `res/` directory, so the demo can be launched from anywhere.

GLFW is an imported shared library at `external/lib/arm64/libglfw.3.dylib`; GLAD is built from `external/src/glad.c`. ImGui sources are compiled directly into the demo target (see `demo/CMakeLists.txt`).

## Shaders are embedded at configure time

Shader sources under `lib/shaders/*.{vert,frag}` are read by `lib/CMakeLists.txt` and substituted into `shaders.h.in` via `configure_file()`, producing `build/lib/generated/shaders.h` with each shader as a `const char* const` raw string literal in `EdgeLighting::ShaderSource::*`. There is no runtime file I/O for shaders.

`CMAKE_CONFIGURE_DEPENDS` lists every shader file, so editing a `.vert`/`.frag` triggers a re-configure on the next build. **If you add a new shader you must update three places**: `lib/CMakeLists.txt` (both the `CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)` lists) and `lib/shaders/shaders.h.in`.

## Architecture

### Orchestrator + renderer plugins

`EdgeLighting::EdgeLightingEffect` (`lib/include/core/edge-lighting.h`) owns:

- a `Config` (geometry + per-renderer sub-configs)
- a `Clock` (play/pause time accumulator)
- a `vector<shared_ptr<BaseRenderer>>` registered by the host

Per-frame contract: `Update(dt)` ticks the clock and forwards `(dt, clockTime, config)` to every renderer; `Render(w, h)` does the same for drawing. `SetConfig` replaces the config and notifies all renderers via `OnConfigChanged`. Renderers are independent visual layers and composite by additive blending — enable any subset.

Current renderers (all under `lib/include/renderer/`):

- `WireframeRenderer` — 1px `GL_LINE_LOOP` debug box, blending temporarily disabled.
- `NeonRenderer` — single-pass neon stroke. Uses an analytic rounded-box SDF + a precomputed 1D `GRADIENT_LUT` texture (RGBA32F, 256px, REPEAT wrap) so each shader sample is one texture lookup instead of an in-shader colour-stops loop. Also precomputes `NUM_LOOP_SAMPLES` (128) sample positions on the perimeter.
- `NeonMultiPassRenderer` — 4-pass version: (1) bake the perimeter colour band to an FBO, (2) horizontal Gaussian blur, (3) vertical Gaussian blur, (4) composite sharp filament + smooth halo + tone-map. Avoids the medial-axis seams of single-pass closest-point projection. Uses two quads — a tight one for pass 1 (perimeter-only rasterisation) and a wide one for blur/composite (covers the bloom extent).

To add a renderer, subclass `BaseRenderer`, add a sub-config struct to `Config`, register it in `demo/src/main.cpp`, and add an ImGui section in `DebugUI`.

### Animation: Clock + Modulators (pure functions of time)

`EdgeLightingEffect` only advances the clock; it does **not** mutate config values. Parameter animation lives entirely outside in the `Modulator` family (`lib/include/animation/modulator.h`) — header-only, pure functions `time -> float`. Concrete shapes: `Constant`, `Oscillator` (SINE/TRIANGLE/SQUARE/SAWTOOTH), `Ease` (with an `Easing::Curve` function pointer), `Sequence`, `Multiplier`, `Adder`, `Remap`. The intended pattern is: the host computes each frame's animated value with `modulator.Evaluate(clock.GetTime())`, writes it into a `Config` copy, and calls `SetConfig`. No coupling between modulators and `Config`.

### RAII GL wrappers

`lib/include/gl/` provides move-only RAII wrappers: `ShaderProgram`, `VertexArray`, `Framebuffer`, `Texture` (base) + `Texture1D` / `Texture2D`. `gl-header.h` is the single include for `<glad/glad.h>`. Use these wrappers — do not call `glGen*` / `glDelete*` directly in renderer code.

### Demo

`demo/src/main.cpp` opens two GLFW windows that share a GL context: the main render window and a separate ImGui debug window (`DebugUI` in `demo/src/debug-ui.{h,cpp}`). The render loop calls `debugUI.Build(cfg, *gEffect)` to lay out widgets, then `debugUI.Render()` draws into the debug window, then makes the main window's context current to draw the effect. Hotkeys (defined in `OnKey` in `main.cpp`) mutate `Config` directly and round-trip through `SetConfig`.

## Conventions

Naming and formatting are defined in `AGENTS.md` and enforced by hand — there is no formatter config. Key points:

- Files: `kebab-case.{h,cpp}`. Namespaces, classes, structs, enums, public methods, event callbacks: `PascalCase` (callbacks prefixed with `On`). Private methods, locals, parameters: `camelCase`. Enum values and constants: `ALL_CAPS_WITH_UNDERSCORES`.
- Variables: members `mFoo`, globals `gFoo`. Header guards `_NAME_OF_FILE_H_`.
- Structs/enums always get a `typedef` self-alias: `typedef struct Config { ... } Config;`, `typedef enum class Winding { ... } Winding;`.
- Always brace single-statement bodies, including every `case` body inside a `switch`. See `AGENTS.md` for the canonical examples.
