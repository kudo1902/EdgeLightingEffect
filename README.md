# Edge Lighting Effect

An OpenGL 3.3 Core renderer that draws an animated neon-style glow along the
perimeter of a rounded rectangle. macOS arm64, CMake + GLFW + GLAD + GLM, with
ImGui for the debug UI.

The library is embeddable - a static C++ library (`libedge-lighting.a`) plus a
`extern "C"` shared library (`libedge-lighting-c.dylib`) for FFI. There are
two demo apps, both with a live ImGui control panel: `demo/` drives the C++
library directly, and `demo-capi/` is the same UI compiled against *only* the
C ABI - its include path deliberately excludes `lib/include/`, which is the
guard proving the ABI is self-sufficient for a real host.

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

Four visual layers, independent - enable any subset via `Config`, and they
composite by blending. The demo registers them in the order below.

- **NeonRenderer** - the neon stroke. Analytic rounded-box SDF plus a gather
  loop over `numSamples` perimeter positions (128 by default), reading baked
  gradient LUTs (RGBA8, REPEAT-wrapped) so each sample is one texture lookup.
  Float textures are deliberately avoided - many edge devices lack support -
  so the LUTs are baked to 8-bit on the CPU. A per-sample emission pre-pass
  bakes the work that depends only on the sample index and time, and is
  skipped entirely on frames neither input moved. Also owns the opaque fill
  that can sit behind the glow.
- **DropletsRenderer** - rain-on-glass droplets in a band that follows the
  perimeter. Screen-space gravity, self-lit drops, no framebuffer capture, and
  a band-fitted ring rather than a fullscreen quad.
- **LensFlareRenderer** - sun + hex-aperture flare (rays, chromatic ghosts) as
  a fullscreen premultiplied pass. The sun rides the perimeter in the same
  parameter space as neon segments and arcs, so the same modulators drive it.
- **DebugRenderer** - every debug annotation in one layer: the baked ring as a
  LUT strip, a disc per colour stop, and the 1 px `GL_LINE_LOOP` bounding box.
  Register it **last** - it annotates what the layers under it drew.

**There are no `*Optimized` renderers.** The half-res forks of the neon and
lens-flare renderers were folded into their originals as a
`resolutionScale` field (`1.0` draws straight onto the target with no
offscreen buffer or blit; below that it draws into a scaled buffer and
bilinear-blits back). `WireframeRenderer` was likewise absorbed into
`DebugRenderer` as `debug.showWireframe`. See
[`docs/neon-unification-plan.md`](docs/neon-unification-plan.md).

## Debug UI (ImGui)

- **Geometry** - width/height/position/corner radius/winding
- **Neon** - line width, filament falloff, intensity, glow radius, bloom,
  glow side + softness, blend space (RGB / HSV / HSL), color stops (up to 128),
  hue rotation rate, segment boosts (travelling brightness peaks), arc gating,
  plus **two independent pairs of cutoffs**:
  - *Glow Inside / Outside Cutoff* - how far the light may travel, with a
    per-side feather.
  - *Opaque Inside / Outside Cutoff* - how far the opaque fill reaches, shown
    under **Opaque** (mode + colour + softness) and only for the sides the
    selected mode actually reads. These share one feather, "Opaque Softness".

  The two pairs do not track each other: a tight glow can sit on a wide fill or
  the reverse. Note that *Opaque* mode `Both` with neither fill cutoff enabled
  fills the whole viewport.
- **Neon > Performance** - resolution scale, gather sample count, gradient LUT
  size. (This is where the old "Optimized Neon (½-res)" section went; a scale
  of `0.5` is what enabling that renderer used to mean.)
- **Debug** - the overlay layer: bounding box + its colour, gradient LUT strip,
  colour-stop markers, and `opaqueOnly` for isolating the fill
- **Droplets (rain on glass)** - rain amount, speed, lanes, band width/offset,
  tint
- **Lens Flare** - perimeter position/offset, size, intensity, ray density,
  rotation, and the ghost controls (spacing, size, offset, tint, centre)
- **Animations** - add / remove presets from an animation group; play / pause /
  reset. Presets include `HueRotationReverse`, `SegmentTravel`, `SegmentBounce`,
  `OutlineTracer`, `Breathing`, etc.
- **Background (debug)** - optional checker pattern behind the effect to
  verify blend vs. occlude compositing.
- **Border Color Picker** - pick any image from `res/`, sample colors from its
  border, and apply them as neon color stops. See below.

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
| `D` | toggle Droplets |
| `G` | toggle wireframe outline (`debug.showWireframe`) |
| `SHIFT` + `O` | toggle Neon resolution scale between 1.0 and 0.5 |
| `W` | toggle winding (CW / CCW) |
| `SPACE` | pause / resume animation |
| `ESC` | quit |

## Layout

```
lib/                        core library
  include/core/config.h     top-level Config + per-renderer sub-configs
  include/renderer/         BaseRenderer + the four renderers, + tuning headers
                            shared verbatim with the shaders
  include/animation/        Modulator family + Animation presets
  include/gl/               RAII wrappers (ShaderProgram, VertexArray,
                            Framebuffer, UniformBuffer, Texture2D)
  include/util/             log, color, compare, time, geometry, frame capture,
                            contour tracer, stb-image
  shaders/*.{vert,frag}     GLSL sources, embedded at configure time
  capi/                     extern "C" ABI for FFI

demo/                       drives the C++ library directly
  src/main.cpp              entry point + hotkey handler
  src/debug-ui.{h,cpp}      ImGui debug window
  src/border-color-picker.{h,cpp}   image-border sampling
  src/animation-presets.h   the preset list the Animations section offers
  src/image-quad.h          textured-quad backdrop
  src/background-quad.h     checker background
  src/ui-controls.h         terminal readout + hotkey list

demo-capi/                  the same UI against the C ABI only - a
                            hand-maintained fork of demo/, so a change to one
                            usually needs the same change in the other.
                            src/gl-mini.h stands in for lib/include/gl/, which
                            it deliberately cannot see.

docs/                       architecture, per-parameter reference, perf
                            reviews, and review-findings.md (open defects)
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
