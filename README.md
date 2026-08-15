# Edge Lighting Effect

An OpenGL 3.3 Core renderer that draws an animated neon-style glow along the
perimeter of a rounded rectangle. macOS arm64, CMake + GLFW + GLAD + GLM, with
ImGui for the debug UI.

The library is embeddable - a static C++ library (`libedge-lighting.a`) plus a
`extern "C"` shared library (`libedge-lighting-c.dylib`) for FFI. The demo app
under `demo/` drives it with a live ImGui control panel.

## Build & run

Configure once, then rebuild from `build/`:

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/demo/edge-lighting-demo
```

Build outputs:

- `build/lib/libedge-lighting.a` - the static library
- `build/lib/libedge-lighting-c.dylib` - flat C ABI surface for FFI / P/Invoke
- `build/demo/edge-lighting-demo` - demo driving the C++ library directly
- `build/demo-capi/edge-lighting-capi-demo` - the same UI driving only the C ABI

The demo opens two windows sharing a GL context: the main render surface and a
floating debug panel with all the sliders. `RES_DIR` is baked into the demo
binary at compile time, so the demo can be launched from anywhere.

## Renderers

Six visual layers, independent and additive. Enable any subset via `Config`.

- **WireframeRenderer** - 1 px `GL_LINE_LOOP` debug outline of the rect.
- **NeonRenderer** - single-pass neon stroke. Analytic rounded-box SDF + a
  precomputed gradient LUT (RGBA8, 256 px, REPEAT-wrapped) so each shader
  sample is one texture lookup. Precomputes 128 sample positions on the
  perimeter. Float textures are deliberately avoided - many edge devices
  lack support - so the LUTs are baked to 8-bit on the CPU.
- **NeonOptimizedRenderer** - half-resolution variant that renders to a scaled
  FBO and bilinear-blits back to full res. Shares visual params with the
  single-pass renderer; its own sub-config carries only perf knobs
  (resolution scale, sample count, LUT width).
- **DropletsRenderer** - rain-on-glass droplets in a band that follows the
  perimeter. Screen-space gravity, self-lit drops, no framebuffer capture.
- **LensFlareRenderer** - sun + hex-aperture flare (rays, chromatic ghosts) in
  one fullscreen pass. The sun rides the perimeter in the same parameter space
  as neon segments and arcs, so the same modulators drive it.
- **LensFlareOptimizedRenderer** - half-res variant of the lens flare. Don't
  enable it alongside `LensFlareRenderer`; they draw the same flare and would
  double it.

## Debug UI (ImGui)

- **Geometry** - width/height/position/corner radius/winding
- **Neon** - line width, filament falloff, intensity, glow radius, bloom,
  glow side + softness, opaque mode + colour, inside/outside cutoffs, blend
  space (RGB / HSV / HSL), color stops (up to 128), hue rotation rate, segment
  boosts (travelling brightness peaks), arc gating
- **Optimized Neon (½-res)** - internal resolution scale, sample count, LUT
  size, plus reuses the Neon section's visual params
- **Droplets (rain on glass)** - rain amount, speed, lanes, band width/offset,
  tint
- **Lens Flare** - perimeter position/offset, size, intensity, ray density,
  rotation, and the ghost controls (spacing, size, offset, tint, centre)
- **Border Color Picker** - pick any image from `res/`, sample colors from its
  border, and apply them as neon color stops. See below.
- **Animation** - add / remove presets from an animation group; play / pause /
  reset. Presets include `HueRotationReverse`, `SegmentTravel`, `SegmentBounce`,
  `OutlineTracer`, `Breathing`, etc.
- **Background (debug)** - optional checker pattern behind the effect to
  verify blend vs. occlude compositing.

## Border color picker

Load an image (JPG / PNG / BMP / TGA), sample colors from the pixels along its
border, and apply them as color stops so the neon "wears" the image's edge
palette. Walking is parameterized on the *target rectangle's* perimeter - not
the image's - so aspect ratios don't matter.

Sliders:

- **Stop Count** (2–128) - more stops → the LUT interpolation between adjacent
  samples is tighter, so sharp image transitions render sharply.
- **Contrast (gamma)** - non-linearly compresses dark stops toward 0 without
  touching bright stops.
- **Auto-adjust intensity** - sets `neon.intensity` so the brightest sampled
  color lands at the tonemap knee, keeping dark stops readably dark and bright
  stops vivid.

Also renders the picked image as a backdrop inside the rect (opt-in checkbox)
so you can visually verify the sampled stops against the source.

## Hotkeys (main window)

Same actions are also on the debug UI sliders.

| Key | Action |
| --- | --- |
| `R` / `F` | inc / dec Neon line width |
| `I` / `O` | inc / dec Neon intensity |
| `[` / `]` | dec / inc Neon glow radius |
| `P` / `L` | inc / dec hue rotation rate |
| `N` | toggle Neon |
| `G` | toggle wireframe outline |
| `W` | toggle winding (CW / CCW) |
| `SPACE` | pause / resume animation |
| `ESC` | quit |

## Layout

```
lib/                        core library
  include/core/config.h     top-level Config + per-renderer sub-configs
  include/renderer/         BaseRenderer + concrete renderers
  include/animation/        Modulator family + Animation presets
  include/gl/               RAII wrappers (ShaderProgram, VAO, FBO, Texture)
  include/util/             log, color, frame capture, contour tracer, stb-image
  shaders/*.{vert,frag}     GLSL sources, embedded at configure time
  capi/                     extern "C" ABI for FFI

demo/
  src/main.cpp              entry point + hotkey handler
  src/debug-ui.{h,cpp}      ImGui debug window
  src/border-color-picker.{h,cpp}   image-border sampling
  src/image-quad.h          textured-quad backdrop
  src/background-quad.h     checker background
  src/ui-controls.h         terminal readout + hotkey list

external/                   GLFW binary, GLAD, GLM, ImGui, stb_image
res/                        demo image assets (see res/CREDITS.md)
```

## Conventions

Naming and formatting are documented in `AGENTS.md` and enforced by hand:

- Files: `kebab-case.{h,cpp}`
- Types: `PascalCase`, with `typedef` self-alias
  (`typedef struct Foo { ... } Foo;`)
- Public methods, event callbacks: `PascalCase` (callbacks prefixed `On`)
- Private methods, locals, parameters: `camelCase`
- Enum values, constants: `ALL_CAPS_WITH_UNDERSCORES`
- Member variables: `mFoo`; globals: `gFoo`
- Header guards: `_NAME_OF_FILE_H_`
- Every `case` body inside a `switch` is braced; single-statement bodies are
  always braced.

There is no formatter config in the repo.

## License

This project is released under the [MIT License](LICENSE).

Bundled image assets under `res/` are photographs from Unsplash under the
[Unsplash License](https://unsplash.com/license). See
[`res/CREDITS.md`](res/CREDITS.md) for per-file attribution.

`external/` vendors GLFW, GLAD, GLM, ImGui, and stb - each under its own
upstream license.

## Working on this repo

See [`CLAUDE.md`](CLAUDE.md) for architecture notes aimed at Claude Code and
other AI assistants, and [`AGENTS.md`](AGENTS.md) for the naming conventions in
full detail.
