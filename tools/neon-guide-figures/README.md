# neon-guide-figures

Renders every image in
[`docs/neon-onboarding-guide.md`](../../docs/neon-onboarding-guide.md) into
`docs/images/neon-onboarding/`, which
[`docs/neon-shader-outputs.html`](../../docs/neon-shader-outputs.html) also
reads its pass images from - plus two only that page shows, the edge ring's
field and its composite below 1.0 (`pass-p1r-ring-field.png`,
`pass-p2r-ring-composite.png`). Off in the default build, like
[`neon-scale-check`](../neon-scale-check/README.md).

## Build and run

```bash
cmake -S . -B build -G Ninja -DEDGE_LIGHTING_BUILD_TOOLS=ON
cmake --build build --target neon-guide-figures
./build/tools/neon-guide-figures/neon-guide-figures
```

With no argument it writes into this checkout's `docs/images/neon-onboarding/`;
pass a directory to write somewhere else (to compare against the committed
set, say). It takes about two seconds and prints every file it writes. The
library's own log is filtered to its WARN and ERROR lines, on stderr.

Rerun it after any change that alters how the neon looks, or the guide's
figures stop describing the code. Renders are deterministic - fixed configs,
`hueRotationRate` 0, `colorTransitionDuration` 0, one `Update(0)` per frame -
so two runs on one GPU write the same bytes, and a diff of the directory shows
exactly which figures a change moved. Another GPU can differ by a level or two
in places, as `neon-scale-check`'s images do.

## What it draws, and how

Three kinds of figure, one function per group in
[`src/figures.cpp`](src/figures.cpp), grouped by the guide's parts:

- **Renders.** The library through its public API: an `EdgeLightingEffect`
  with the neon and debug layers, `SetConfig`, `Update(0)`, `Render` inside an
  `OffscreenCapture`, then cropped and magnified on the CPU. Most of Parts 3
  and 4.
- **Diagrams.** Drawn on the CPU from the shaders' own formulas, ported line
  for line: `sdRoundBox`, `perimeterPosition`, `rectPerimeter`, the gather's
  Lorentzian weight and the grade's curve. Plots are written as SVG with their
  own white background.
- **Pass captures.** What the renderer's private buffers hold between passes,
  and the triangles each pass draws - Parts 5, 6 and 8.

The pass captures come from [`PassRecorder`](../common/pass-recorder.h),
shared with `neon-scale-check partition`, which watches a frame the way a GPU
debugger would, without the library knowing.
GLAD reaches every GL entry point through a global function pointer, and the
library draws only with `glDrawArrays`, so the recorder swaps
`glad_glDrawArrays` for a wrapper. Each draw goes through unchanged; after it,
the wrapper names the pass from the uniforms its program declares (`uSource`
is the blit, `uGather` is the reduced shading or the ring, told apart by
target, and so on), reads every colour attachment of the target back as
floats, and records the vertex array and `uMVP`. `Redraw` replays a recorded
draw's triangles in a flat colour - filled or as edges through
`glPolygonMode` - which is how the geometry figures are made.

Two consequences:

- It needs the **static** library. The C ABI dylib carries a private copy of
  GLAD whose pointers the tool cannot reach.
- It restores every piece of state it touches (read framebuffer and read
  buffer, texture binding, pack alignment, program, vertex array, polygon
  mode). Checked by rendering the pass scene with and without recording: the
  two frames are byte-identical.

## Adding a figure

Add it to the right group in `src/figures.cpp`, name the file the way the
others are named, and reference it from the guide with a caption that says
what config it shows. A recorded frame needs a fresh effect (`FreshEffect`):
the emission pre-pass is skipped on a frame whose config did not change, and a
recorded frame has to contain it. Measure anything a caption quantifies from
the rendered pixels rather than reading it off the picture.
