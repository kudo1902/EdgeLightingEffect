# layer-check

The five-layer baseline of [`docs/resource-sharing-plan.md`](../../docs/resource-sharing-plan.md)
(its S0). `neon-scale-check` judges the neon alone, against committed images.
This tool judges every layer at once, against itself. Each step of that plan
has to leave the pixels alone, and two reports from the same machine and build
type show whether it did.

## Build

Configure the repo with `-DEDGE_LIGHTING_BUILD_TOOLS=ON`; the binary is
`build/tools/layer-check/layer-check`. It can also build standalone against
another checkout's library, as `neon-scale-check` does. See `CMakeLists.txt`.

## Modes

```bash
layer-check report --out after.txt [--only SUBSTRING] [--flush]
```

Renders every scene through all five renderers, at 1280 x 720, and writes one
line per fact:

| line | what |
| ---- | ---- |
| `hash SCENE FRAME HEX` | FNV-1a 64 of the frame's RGBA, for each of the 4 frames after the config is set (dt 1/60) |
| `mem SCENE TEX BUF` | bytes of GL textures and buffers alive after those frames, from `glTexImage2D` / `glBufferData` |
| `alloc SCENE TOTAL F0 ... F7` | C++ heap allocations (`operator new`) the library made in each of 8 more frames |
| `alloc-driver SCENE TOTAL F0 ... F7` | the same for the GL driver, inside the library's GL calls (below) |
| `published NAME#N WxH chC` | what the all-layers frame published to the buffer registry |

The scenes:

- `neon-scale-check`'s twelve neon scenes at scales 1.0, 0.5 and 0.25.
- The neon with something moving: hue, intensity, an arc wipe, a travelling
  segment.
- The lens flare and a five-lamp spotlight fan, each at 1.0 and 0.5, plus a
  clipped lamp.
- Droplets.
- The debug overlays, still and during a colour-stop cross-fade.
- All five layers at once below 1.0.
- The debug view on every buffer that frame publishes.

**Allocations are counted by caller.** The tool replaces the global
`operator new`, and on macOS that also catches Apple's GL stack, which is
C++. So each allocation is filed by the first frame above `operator new`
outside libc++:
- in the main image - the library, linked in statically - on `alloc`;
- in any other image, on `alloc-driver`. On an Apple M2 Pro these are all
  Apple's GL stack: `AppleMetalOpenGLRenderer`, `AGXMetalG14X` and `Metal`.

Only the thread that runs the frame counts; the driver also allocates on
worker threads of its own. `alloc` should read 0 on every scene: steady
frames allocate nothing in the library (review finding I75). `alloc-driver`
follows the GL work (I75 again):
- 24 B per draw;
- 40 B per change of framebuffer;
- a burst whenever the driver starts a command buffer: 128 B, then 24 B per
  GL object it touches.

Without a flush the driver starts a command buffer every few frames, on its
own schedule, so the driver line is periodic. A still frame then counts its
draws. `--flush` calls `glFlush` after each counted frame, as a host's swap
would, and every frame starts one. The driver line is then steady, and higher:
15 a frame for a still neon frame, 40 under a rotating hue, 72 with all five
layers. Without `--flush` the colour-stop cross-fade scene's second counted
frame reads 157: the process's first ring upload builds a Metal pipeline. That
count used to vary between runs (160-237), because the driver builds part of
the pipeline on a worker thread; counted on the calling thread alone it is the
same every run.

`--only SUBSTRING` runs only the scenes whose id contains it. The library's
own log goes to stdout, so give `--out`.

```bash
layer-check diff before.txt after.txt
```

Exits 1 if any frame hash differs or a scene is missing from either report.
It also lists every change in memory or allocations, but those do not fail it.
It warns when the two reports counted allocations differently: one with
`--flush` and one without, or one from before the count was split by caller,
whose `alloc` lines hold the driver's too.
Hashes are only comparable within one GPU and one build type: Debug and
Release libraries differ on a few frames, from CPU float math in uniforms.

```bash
layer-check startup [--fail-stage NAME]
```

One effect with every layer, timed through `Initialize` and its first frame.
That frame has every layer on, segments, the hue rotating and the debug view
up, so it builds every program a host can need, except the opaque fill's. It
reports each phase's compile and link count and time, and how often each
source was compiled. Sources are named by their `ShaderSource` constant; `+def`
marks a variant with a `#define` spliced in. `--fail-stage NEON_VERT_SRC` makes
every compile of that source report failure, to check that a failed stage is
compiled and logged once, and what still draws.

Run it a few times: the first run after a build pays the driver's cold shader
cache. On an M2 Pro, in a Debug build, that first frame took ~500 ms against
~120 once warm.

## How a "before" is made

The tool links the library statically, so a copy of the binary is a frozen
library. Copy `build/tools/layer-check/layer-check` aside before the change,
and report with both. If the change alters the layout of a public class the
tool constructs (`EdgeLightingEffect`), build the frozen copy against the old
headers too. A binary built on the new headers against the old library reads
past the old object.
