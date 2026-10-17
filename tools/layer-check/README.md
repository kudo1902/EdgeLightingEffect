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

Standalone, it builds against `main` and against any checkout from S2 of the
sharing plan on. `main` has no buffer registry, so against it the tool leaves
out the `published` lines and the `view/` scenes, and its report says
`# registry: none`; `diff` then skips the other report's `view/` frames and
says how many. The branch's first registry, between `main` and S2, had another
API and is not supported: for a "before" there, copy a binary aside (below).
Measured on an AMD Radeon Pro 5300M, Release: all 208 frames `main` renders
are byte-identical to this branch's at `bcf51e2`.

## Modes

```bash
layer-check report --out after.txt
```

Renders every scene through all five renderers, at 1280 x 720, and writes one
line per fact:

| line | what |
| ---- | ---- |
| `hash SCENE FRAME HEX` | FNV-1a 64 of the frame's RGBA, for each of the 4 frames after the config is set (dt 1/60) |
| `mem SCENE TEX BUF` | bytes of GL textures and buffers alive after those frames, from `glTexImage2D` / `glBufferData` |
| `alloc SCENE TOTAL F0 ... F7` | C++ heap allocations (`operator new`) in each of 8 more frames - the GL driver's included (below) |
| `published NAME#N WxH chC` | each buffer the all-layers frame left current in the buffer registry - key `NAME[N]`, or `NAME` for N = 0; the `#N` form is S0's, kept so reports from either side of the string keys compare |
| `published-in SCENE NAME#N WxH chC` | for the `shared/` scenes, `NeonRenderer.Composite` when that scene left it current |

The scenes:

- `neon-scale-check`'s twelve neon scenes at scales 1.0, 0.5 and 0.25.
- The neon with something moving: hue, intensity, an arc wipe, a travelling
  segment.
- The lens flare and a five-lamp spotlight fan, each at 1.0 and 0.5, plus a
  clipped lamp.
- Droplets.
- The debug overlays, still and during a colour-stop cross-fade.
- All five layers at once below 1.0.
- The neon's shared buffer (`compositeScale`, `NeonRenderer.Composite`), each
  scene twice - the main frame, and `+view`, the debug view of the buffer over
  it, so the buffer's own texels are hashed:
  - `shared/default@0.5`: the scale set, but nothing reading the buffer, so
    it is not drawn (no `published-in` line). `+view` reads it.
  - `shared/default@0.5+host-read`: the same scene read by the host
    (`EdgeLightingEffect::AddHostRead`) rather than a layer. It has a
    `published-in` line for `NeonRenderer.Composite#0`, and its main frame
    must hash as `neon/default@0.5`'s - drawing the buffer moves no
    main-frame pixel.
  - `shared/default@1`, still, and `+hue+view`: the buffer at full
    resolution. Unread, `shared/default@1` must hash as `neon/default@0.5`:
    the main frame is never drawn from the buffer.
  - `shared/default@0.5+step-to-1+view`: the shared scale alone changing
    under a reader - 0.5, 1, 0.5, 0.5. Frame 0 must hash as
    `shared/default@0.5+view`'s frame 0, frame 1 as `shared/default@1+view`'s,
    frames 2 and 3 as `shared/default@0.5+view`'s.
  - `shared/outside@0.4` and `@0.25`: a one-sided glow over a shaped fill at
    the floor scale (`NeonConfig::COMPOSITE_MIN_SCALE`). 0.25 is drawn at the
    floor, so every frame of the pair must hash alike.

  Those equalities are within one report; `diff` against another report only
  says whether a frame moved. Nothing here forces the RGBA8 fallback, so the
  scale-1 copy's half-float-only rule is not covered.
- The debug view on every buffer current in that frame.

**The allocation counts are not the library's alone.** The tool replaces the
global `operator new`, and on macOS that also catches Apple's GL stack, which
is C++. It counts on the rendering thread only - the window's flag is
`thread_local`, so the driver's own threads count nothing (before that, they
were counted, racily, which is part of why counts varied between runs). On
that thread it counts:
- 24 B per draw;
- 40 B per change of framebuffer;
- a burst whenever the driver starts a command buffer. That happens every few
  frames here, since the tool never flushes.

A still frame's count equals its number of draws. The colour-stop cross-fade
scene's first counted frame also varies between runs: the process's first
ring upload builds a Metal pipeline. So `diff` lists that line however little
changed. Separating the library's share is proposed in review finding I75.

`--only SUBSTRING` runs only the scenes whose id contains it. The library's
own log goes to stdout, so give `--out`.

```bash
layer-check diff before.txt after.txt
```

Exits 1 if any frame hash differs or a scene is missing from either report -
but for the `view/` scenes when one report was made without a registry.
It also lists every change in memory or allocations, but those do not fail it.
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
