# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

OpenGL 3.3 Core renderer that draws an animated neon-style glow along the perimeter of a rounded rectangle, plus companion layers (rain droplets, lens flare). macOS arm64, CMake + GLFW + GLAD + GLM, ImGui for the debug UI.

The legacy stroke/particle/path system was removed in favour of a smaller neon-focused pipeline.

Docs, in reading order. The three neon documents are tiers of the same material - pick one by how much depth you need, not all three:

- [`docs/implementation.md`](docs/implementation.md) - brief: how the library is put together on the C++ side and how a frame runs. Start here.
- [`docs/neon-onboarding-guide.md`](docs/neon-onboarding-guide.md) - the long on-ramp for someone new to graphics: the GPU concepts the neon relies on (pipeline, coordinate spaces, textures, std140 blocks, FBOs, premultiplied alpha, SDFs, tone mapping, derivatives), then every `NeonConfig` field traced to its uniform, pass and dirty flag, every GL object, every pass with its inputs / state / outputs, and each shader stage by stage. Ends with pitfalls, recipes and exercises. Illustrated throughout with renders from the library itself - each layer and config field on screen, and what every pass writes, captured from a real frame. Read it before the three neon documents below if the vocabulary is new.
- [`docs/neon-shader-outputs.html`](docs/neon-shader-outputs.html) - one frame at resolution scale 0.5, then the same scene at 1.0, pass by pass: what each of the neon's nine programs (`mEmissionShader`, `mGlowCoverShader`, `mNeonGatherShader`, `mNeonShadeShader`, the blit, `mNeonRingShader`, the fill, and the field bake `mNeonFieldShader` and its composite `mFieldCompositeShader` - at 1.0, and below it under a rotating hue) actually wrote, at the resolution it runs, and why each is its own program - the field bake's case at length. The pictures for Part 6 of the guide on one page.
- [`docs/neon-renderer-overview.html`](docs/neon-renderer-overview.html) - the renderer in ~7 minutes: one quad, two measurements, three layers of light.
- [`docs/neon-renderer-explained.html`](docs/neon-renderer-explained.html) - the same ground at length, with the reasoning and the bugs behind each decision.
- [`docs/neon-renderer-reference.html`](docs/neon-renderer-reference.html) - full mechanism reference: every uniform, constant, derivation and gating rule, plus the droplet and flare term stacks. Sections 18 and 19 are flow charts - what the CPU rebuilds and which GL object each bake writes, one frame's draw sequence for both renderers, and the fragment program as annotated GLSL dataflow. Read this before changing a shader.
- [`docs/effect-reference.md`](docs/effect-reference.md) - per-parameter reference and recipes.
- [`docs/spotlight-renderer.md`](docs/spotlight-renderer.md) - the same for the spotlight layer: every `SpotLight` field, the app-coordinate convention, the animatable surface, and the measured cost of its resolution scale.
- [`docs/architecture-design.md`](docs/architecture-design.md) - full architecture. Note it predates the droplets and lens-flare renderers.
- [`docs/coordinate-system.md`](docs/coordinate-system.md), [`docs/multiple-arcs-design.md`](docs/multiple-arcs-design.md).
- [`docs/neon-unification-plan.md`](docs/neon-unification-plan.md) - how the half-res neon fork was folded into `NeonRenderer` as a resolution scale, and the debug overlays split into `DebugRenderer`. Read it if a doc or comment still refers to `NeonOptimizedRenderer`.
- [`docs/neon-unification-comparison.md`](docs/neon-unification-comparison.md) - the evidence for that plan: eleven scenes rendered on both sides of the merge, nine byte-identical, the two that differ confined to the bounding box's compositing order.
- [`docs/lens-flare-unification-comparison.md`](docs/lens-flare-unification-comparison.md) - the same for the lens flare pair, the last fork in the tree: twelve scenes, all byte-identical, plus the two defects the merge closed (an unclamped resolution scale and the double-draw).
- [`docs/lens-flare-perf-review.md`](docs/lens-flare-perf-review.md) - why the flare was the pipeline's most expensive layer and what the five changes that halved it did. Per-term cost attribution, the tuning header those changes forced, and the reason one of them cannot be byte-identical on any GPU. Read before touching `lens-flare.frag`'s ghost loop.
- [`docs/neon-perf-review.md`](docs/neon-perf-review.md) - the same for the neon layer, which is 81% of the frame and whose gather loop is 95% of that. Per-term attribution, the three changes that took it to 1.46x, and the two directions that were measured and set aside (a windowed gather; an interior hole in the draw quad - since built without the inner fade it called for, as `GetGlowInnerReach`; see `docs/glow-inner-reach.md`). Section 8 is the measured shape of `resolutionScale` / `numSamples` / `glowRadius` - read it before tuning any of them for speed. Section 10 is the later round that took scale 1.0 another 2.3-4.4x at 1080p (the gather split out at 1.0, the single-`atan` bloom, and the exact / bounded piece skips), with the attribution of the shading pass that is now the cost. Read before touching `neon.frag`'s gather loop or its halo / bloom pieces.
- [`docs/glow-inner-reach.md`](docs/glow-inner-reach.md) - the glow quad's interior hole with no cutoff set: why the interior was shaded to the centre, why the hole cannot sit at `reach` (the halo and the corner arcs' bloom are still lit past it), the closed-form bound that places it, the byte-for-byte proof it changes no pixel, and before/after frame times. Read before touching the halo / bloom terms in `neon.frag`, which `GetGlowInnerReach` mirrors.
- [`docs/neon-perf-plan.md`](docs/neon-perf-plan.md) - what is left after the 1.0 gather split, measured on an Apple M2 Pro at `0cec9f4`: where a neon frame's time and memory go by frame type (still, hue rotating, intensity pulse, arc wipe, travelling segment), the attribution of the partly lit premium, and a ranked plan of thirteen items with prototype numbers for the two largest - skipping the offscreen passes on frames where nothing moved (byte-identical, 2-6x on still frames at 0.5) and re-baking only the glow coverage table's changed pieces (exact, up to 1.91x on travelling-segment frames). Its section 11 is the follow-up review and what it built: `perimeterPosition` skipped on a uniform ring (I45) and the hue-invariant field at 1.0 (I46). Read before starting any neon perf or memory work.
- [`docs/neon-shader-cleanup-plan.md`](docs/neon-shader-cleanup-plan.md) - the neon's program slots, `neon.frag` variants and hand-copied shader functions as of `1488648`, and a stepwise plan to cut them to what earns its place without moving a pixel: one copy of the SDF / tone-map / edge-mask code, one predicate for a uniformly lit ring, the never-built inline-gather path removed (an owner's decision), and the measured reasons behind each. Read before adding a shader variant or program.
- [`docs/neon-frag-notes.md`](docs/neon-frag-notes.md) - the measured history behind `neon.frag`'s lines, moved verbatim out of the shader in that plan's step 6 (one section per block, in shader order): the sweeps, before/after numbers and bug narratives its comments used to carry inline. The shader keeps the invariant, the CPU mirror and the warning at each line; read the matching section here before changing a block whose reason the shader only summarises.
- [`docs/spotlight-renderer-plan.md`](docs/spotlight-renderer-plan.md) - the `SpotlightRenderer` design and the offscreen verification behind it, including the solved strip bound and the one real defect that verification caught.
- [`docs/corner-crease-and-filament-nyquist.md`](docs/corner-crease-and-filament-nyquist.md) - the analytic emission's measured defects and their fixes: the dark diagonal wedges at the corners (halo and bloom were the field of ONE infinite edge, now a sum over the emitter's pieces), the `resolutionScale` 0.5 mismatch at thin line widths (the filament's floor was in the wrong units, and then - section 2.8 - was a fixed half width when what decides the blit is the profile's SHAPE, so a soft `filamentFalloff` rendered twice as wide), and the corner over-extension the first fix introduced (the straights ran to the SHARP corner, so a phantom emitter lit the outside of every rounded corner; they now stop at the tangent points and each arc is developed onto its own tangent), and the crease THAT fix introduced (the arc was developed at rate `r`, which is right only for a fragment on the arc, so every arc's centre of curvature carried a C1 kink and an under-count - a dark cross at the middle of a circle; section 1.9). Section 1.10 is the one level up: all of that fixed the halo/bloom FIELD, while the coverage that SCALES it was still read at the fragment's NEAREST perimeter point - so any partly lit perimeter (a half-ring arc, a segment boost) cut the glow to a hard-edged polygon along the medial axis until the glow took a gathered coverage instead (itself replaced since V20 by each piece's own coverage, read from a baked table). Includes the per-edge bloom pedestal the first fix forced and the one shared pedestal the arcs are allowed instead, the small-rect and INTERIOR brightness changes the segment sum causes - section 1.5.2 is the one to read if someone reports "the glow got bigger" - and the offscreen probes behind every number. Read before touching the halo/bloom or filament blocks.
- [`docs/glow-side-comparison.md`](docs/glow-side-comparison.md) - what changed when every edge the neon draws became COVERAGE applied to the graded output rather than a multiply into the linear emission, and when the one-sided cut moved off the reduced-resolution buffer into `neon-blit.frag`. Magnified before/after crops plus the sub-pixel sweeps behind them. Read it before retuning `glowSideSoftness` or `Cutoff::softness`, and before assuming a mask belongs above the tone map.
- [`docs/neon-resolution-scale-proposal.md`](docs/neon-resolution-scale-proposal.md), [`docs/neon-resolution-scale-plan.md`](docs/neon-resolution-scale-plan.md) and [`docs/neon-resolution-scale-comparison.html`](docs/neon-resolution-scale-comparison.html) - the edge ring: why the reduced-resolution neon re-shades a thin ring around the edge at full resolution (the proposal), how it was built in six steps and what each measured (the plan; section 7 has the re-verification and the cost caveats, section 11 the open decisions), and twelve scenes at six scales against their 1.0 renders, before and after (the comparison page). [`docs/neon-resolution-scale-perf-comparison.md`](docs/neon-resolution-scale-perf-comparison.md) is the price: frame time before and after at every scale, attributed step by step (the ring is three quarters of it), plus startup and memory - and, in section 9, the bottom line against `main` at 0.5 on an Apple M2 Pro (2.3x faster at 720p, 2.8x at 1080p, and within 2/255 of 1.0 where `main` is up to 77 off). Section 10 is what the glow-coverage table (V19/V20) added on top, which sections 1-9 predate. Read the plan before touching the scaled path, the ring's width rule or the blit/ring partition.
- [`docs/upgrade-notes.md`](docs/upgrade-notes.md) - what a host moving from `main` has to change: the opaque fill's own cutoffs (default off - a fill that leaned on the glow's cutoffs grows to the viewport), `Cutoff::softness` running outward from `size`, the deprecated C shims for the old opaque softness, and the per-path shader compile. Read before telling anyone an upgrade is drop-in.
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

**That configure defaults to `Release`.** The root `CMakeLists.txt` sets `CMAKE_BUILD_TYPE` to `Release` when the caller has not chosen one, because an empty build type contributes no `-O` flag at all and the command above used to ship an unoptimised library (11.6 MB of `.a`, and every CPU path in it - LUT bakes, colour conversion, the contour tracer, the whole C ABI - built at `-O0`). Pass `-DCMAKE_BUILD_TYPE=Debug` explicitly when you want that; the default only applies when nothing is set, and multi-config generators are left alone. See [`docs/neon-perf-review.md`](docs/neon-perf-review.md) section 3.

There is no test target. The build produces four artifacts:

- `build/lib/libedge-lighting.a` - the C++ static library
- `build/lib/libedge-lighting-c.dylib` - flat `extern "C"` shared library for FFI / P-Invoke
- `build/demo/edge-lighting-demo` - demo driving the C++ library directly
- `build/demo-capi/edge-lighting-capi-demo` - the same UI driving only the C ABI

Two optional tools, off by default: configure with `-DEDGE_LIGHTING_BUILD_TOOLS=ON` and `build/tools/neon-scale-check/neon-scale-check` appears - the harness behind `docs/neon-resolution-scale-comparison.html`. `neon-scale-check check` is the closest thing to a regression test the neon has: it renders twelve fixed scenes at six resolution scales and exits non-zero if scale 1.0 drifts from the page's committed images, if a reduced scale exceeds its error bound, or if a moving hairline wanders off its edge. Run it after touching `neon.frag`, `neon-blit.frag`, `neon.vert` or the scaled path in `NeonRenderer`. `neon-scale-check time` times every scene at every scale; `--mode hue|intensity|arc-wipe|segment-travel` times the frames a host draws while something moves (`SetConfig` + `Update` inside the timed loop), which the default `still` cannot see - re-bakes, and anything skipped on an unchanged frame. Measure a change that saves work on unchanged frames in `still` AND the animated modes. `neon-scale-check partition` is the second gate: across a thousand random configs (seeded, reproducible on any platform) it fails if the blit and the edge ring do not tile the frame exactly - a pixel drawn by both or, between them, by neither. It also builds standalone against another checkout's library, which is how "before" numbers are measured. See [`tools/neon-scale-check/README.md`](tools/neon-scale-check/README.md). The same option builds `build/tools/neon-guide-figures/neon-guide-figures`, which renders every image in `docs/neon-onboarding-guide.md` into `docs/images/neon-onboarding/` - rerun it after a change to how the neon looks, and diff the directory to see which figures moved. Its pass figures come from `PassRecorder` (`tools/common/`, shared with `neon-scale-check partition`), which wraps `glad_glDrawArrays` to read the renderer's private buffers between draws without any library hook; that only works against the static library. See [`tools/neon-guide-figures/README.md`](tools/neon-guide-figures/README.md).

`RES_DIR` is baked into both demo binaries as a compile definition pointing at the in-tree `res/` directory, so they can be launched from anywhere.

Third-party image assets under `res/` (the `.jpg` files from Unsplash) are covered by the [Unsplash License](https://unsplash.com/license); see `res/CREDITS.md` for the per-file attribution table.

GLFW is an imported shared library at `external/lib/<arch>/libglfw.3.dylib` (arch picked from `CMAKE_OSX_ARCHITECTURES` or the host); GLAD is built from `external/src/glad.c`. ImGui sources are compiled directly into both demo targets.

The root `CMakeLists.txt` has `PLATFORM_WINDOWS` / `PLATFORM_LINUX` branches that select `#version 300 es`, but only macOS is actually buildable today - `external/lib/` ships macOS binaries only and the imported GLFW location is hardcoded to `libglfw.3.dylib`. Treat the non-Apple branches as unfinished scaffolding.

## Shaders are embedded at configure time

Shader sources under `lib/shaders/*.{vert,frag}` are read by `lib/CMakeLists.txt` and substituted into `shaders.h.in` via `configure_file()`, producing `build/lib/generated/shaders.h` with each shader as a `const char* const` raw string literal in `EdgeLighting::ShaderSource::*`. There is no runtime file I/O for shaders. `@GLSL_VERSION@` supplies the version line and three tuning headers are injected verbatim so their constants are shared between the shaders and the C++ renderers: `@NEON_TUNING@` injects `lib/include/renderer/neon-tuning.h` into the neon shaders (`neon-glow-cover.frag` included - it reads the `GLOW_COVER_*` layout constants, `MAX_ARCS` and `ARC_FEATHER_MAX_SHARE` from there), `@DROPLETS_TUNING@` injects `lib/include/renderer/droplets-tuning.h` into `droplets.frag`, and `@LENS_FLARE_TUNING@` injects `lib/include/renderer/lens-flare-tuning.h` into `lens-flare.frag`, and `@SPOTLIGHT_TUNING@` injects `lib/include/renderer/spotlight-tuning.h` into `spotlight.frag`. Four GLSL chunks are injected the same way. `@NEON_COMMON@` puts `lib/shaders/neon-common.glsl` - what the gather pass and the shading both declare: the fragment position, the rect's shape, the segment block, `rectPerimeter` - into `neon.frag` and `neon-gather.frag`, after the tuning header; the gather loop itself is `neon-gather.frag`'s alone. `@NEON_PIECES@` puts `lib/shaders/neon-pieces.glsl` - `minimaxAtan`, `arcTangentSegment` and the glow coverage table's layout - into `neon.frag` (after `@NEON_COMMON@`) and `neon-glow-cover.frag` (after the tuning header), so the table's read and its bake agree to the texel. `@NEON_SDF@` puts `lib/shaders/neon-sdf.glsl` - `sdRoundBox` and the band boundaries - into `neon.frag`, `neon-blit.frag` and `black-rect.frag`, the three programs that draw the glow's edge; `@NEON_GRADE@` puts `lib/shaders/neon-grade.glsl` - the tone map, `neonToneMap` - into `neon.frag` and `neon-field.frag`. None is a shader (no `main()`); each declares its own `precision`, since it lands ahead of the file's own precision line (a `#version 300 es` fragment shader has no default float precision), and `neon-pieces.glsl` names its own `PIECES_PI` / `PIECES_HALF_PI`, since only one of the two programs it goes into has `neon-common.glsl`'s. GLSL has no `#include`; this is the substitute, so a chunk shared by several shaders goes in a `.glsl` file injected through `shaders.h.in`, not copied. Never write an `@NAME@` placeholder in a comment in `shaders.h.in` itself - `configure_file` expands it there and the generated header stops compiling (placeholders inside the shader FILES are safe: substituted values are not re-scanned).

`CMAKE_CONFIGURE_DEPENDS` lists every shader file, every `.glsl` chunk *and* all four tuning headers, so editing any of them triggers a re-configure on the next build. **If you add a new shader you must update three places**: `lib/CMakeLists.txt` (both the `CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)` lists) and `lib/shaders/shaders.h.in`.

**Never declare a bare uniform array** (`uniform vec4 uFoo[N]`) in a shader. The form is not available on the restricted GL targets this library ships against, and it will compile and run correctly on desktop GL, so testing will not catch it. Per-index data goes in a `layout(std140) uniform` block, uploaded through the `UniformBuffer` wrapper and bound to its own binding point - `LoopSamplesBlock`, `SegmentBlock`, `ArcBlock` (neon) and `GhostBlock` (lens flare) are the existing examples (`SpotlightRenderer` sidesteps the question entirely - see below), and their array bounds are compile-time constants from the tuning headers. `ShaderProgram`'s array `SetUniform` overloads and its `UNIFORM_ARRAY_DIRECT` fallback exist for the upload path only; neither makes a bare array declaration portable.

## Architecture

### Orchestrator + renderer plugins

`EdgeLighting::EdgeLightingEffect` (`lib/include/core/edge-lighting.h`) owns:

- a **base** `Config` (geometry + per-renderer sub-configs) - what `SetConfig` writes
- an **active** `Config` - base + animation overlays, what renderers actually see
- a `Clock` (play/pause time accumulator)
- an `AnimationManager` (held by `unique_ptr`)
- a `vector<shared_ptr<BaseRenderer>>` registered by the host

Per-frame contract: `Update(dt)` ticks the clock, advances every attached animation by the clock delta, rebuilds the active config, then forwards `(dt, clockTime, activeConfig)` to every renderer; `Render(w, h)` does the same for drawing. Both `SetConfig` and the per-frame refresh notify renderers via `OnConfigChanged` - but only when the composited config actually changed, so renderers can rely on that call being meaningful. Renderers are independent visual layers and composite by blending - enable any subset.

Five renderers, all under `lib/include/renderer/`, all registered by the demo in this order:
- `NeonRenderer` - the neon stroke. Analytic rounded-box SDF plus a gather loop over `NeonConfig::numSamples` perimeter samples (positions in a UBO), reading three baked LUT textures: `uGradientLUT` (base colour ring), `uSegmentLUT` (per-segment gradient atlas, one row per segment), `uArcLUT` (per-arc atlas). All LUTs are baked on the CPU as **RGBA8** - float textures are deliberately avoided for edge-device compatibility. Also owns the opaque-fill pass (`black-rect.frag`).

  **One renderer, two resolution paths**, selected by `NeonConfig::resolutionScale`: at `1.0` the glow draws straight onto the framebuffer it was handed (no reduced buffer, no blit); below `1.0` the gather runs alone into a small buffer of its own, then the glow is shaded from it into a buffer at that fraction of the viewport, which is bilinear-blitted back (`neon-blit.frag`) everywhere EXCEPT a thin **edge ring** around the rect edge, which is re-shaded at full resolution. The paths share one pass schedule - every pixel-valued uniform is multiplied by the scale unconditionally (a no-op at `1.0`) and the shader converts `neon-tuning.h`'s own full-res px constants with `uResolutionScale`. Only the render target, the shading programs, the blit, the ring and the buffer allocations are conditional. The opaque fill is the exception: always full-res on the caller's framebuffer, since its analytic SDF edge is the whole point of it.

  **At `1.0` the gather is a pass of its own too** (`docs/neon-perf-review.md` section 10): pass 1a (`neon-gather.frag`) runs onto the same coarse grid as below `1.0`, and the edge ring's program (`mNeonRingShader`) shades the WHOLE glow quad from it at full resolution, on the caller's framebuffer - reusing that program is safe because it still draws one target in one blend state per frame. Within 1/255 of running the loop inline in every fragment, 2-4x cheaper on a large quad. The inline loop, and the gate that kept it wherever the split did not pay (`FULL_RES_SPLIT_*`, tuned on an AMD 5300M whose first offscreen pass costs ~0.15 ms), are gone ([`docs/neon-shader-cleanup-plan.md`](docs/neon-shader-cleanup-plan.md) step 3): an Apple M2 Pro measured the split faster at every size and in every mode, and the AMD itself, re-measured with every rect split against its own gate, 0.98-1.02x in every animated mode with one loss (a cutoff band under an intensity pulse, 0.84x). What replaced the gate is a memory bound: `GATHER_MAX_SCALE` (0.5) caps the gather's resolution at every scale, so a rect small enough to want a near-full-res gather (perimeter under ~454 px) gathers at 0.5 - for a 40 x 24 rect with `glowRadius` 20 at 1080p, 20 -> 8 MB of textures (33 -> 9 with a segment) and a 3.3x faster frame, at up to 9 levels on a few hundred channels near its line (5 on a 64 x 36 rect, 1 on 128 x 72; no check scene moves). `setupRingGeometry` always builds the gather quad, and `mGatherBuffer` is released only when the layer is disabled.

  **On a frame where nothing moved, passes 1a and 1b do not run at all**
  (`mOffscreenCurrent`, [`docs/neon-perf-plan.md`](docs/neon-perf-plan.md)
  item 4). Both are pure functions of the config, the viewport, the two tables
  they read and - only through the hue - the time, and both buffers persist
  between frames. The flag is cleared by `OnConfigChanged` on ANY change to
  `config.neon` or `config.geometry` (wide on purpose: pass 1b reads most of
  the neon's config) - and, with the field's two flags, ONLY on those: the call
  comes for any change to the composited config, so gated on "any change" an
  animation of another layer (the lens flare's sun, an `AnimatableField`)
  cleared all three every frame and cost the neon 4.9x with the hue rotating,
  15x still (I47). It is honoured only when the
  emission table is current (which carries the time and a cross-fade), the
  glow coverage table needs no bake, and the viewport is the one recorded with
  it. Such a frame never leaves the caller's framebuffer and takes no
  `RenderTargetState` capture; the regions and their UV maps are still
  computed, because the passes on the caller's framebuffer read through them.
  Byte-identical; 2-6x on a still frame at scale 0.5 on an Apple M2 Pro. It
  does not change which path a config takes: the split's gate stays, since an
  animated frame still pays the offscreen pass.

  **At `1.0`, pass 1 is usually factored into a hue-invariant field** (pass
  1f / 1c, I46, `docs/neon-perf-plan.md` section 11): with no segments and
  nothing applied after the tone map, `neon.frag`'s output is
  `tonemap(col * Fa)` where only the gathered hue `col` moves with time. So
  `neon.frag` compiled with `NEON_FIELD_BAKE` (`mNeonFieldShader`; it sets
  `col` to 1 and writes the pre-tone-map `Fa`) bakes `mFieldBuffer` offscreen,
  after the gather, and `neon-field.frag` (`mFieldCompositeShader`, pass 1c)
  draws the glow quad in pass 1's place: read the field, read the hue,
  tone-map - 4x on the default frame with the hue rotating, 9x still, within
  1/255. Eligible (`IsFieldEligible`) with no segments; under a rotating hue,
  a ring opaque at every texel (else time moves the alpha `neon.frag` reads);
  not at 1.0 with a one-sided glow or a cutoff (`MasksAfterGrade` - `neon.frag`
  multiplies those in after its tone map there, and such a config shades
  directly; the field used to carry them as a second channel, I51); and only
  where the quad fills at least half the field's box
  (`FIELD_MIN_FILL` - a thin band's box is the whole screen). The bake is
  LAZY: a frame that changed the config (or the viewport, or uploaded the
  gradient ring) draws pass 1 directly, and the field is baked only once that
  held for a frame (`mFieldSettled`) - an animation changing the config every
  frame never pays a bake it cannot reuse. The one exception is the FIRST
  frame a config the field can serve is drawn: it builds both programs AND
  bakes the field, while still shading directly. This driver finishes a
  program's compile on its first draw in its real target and blend state, so
  programs only linked there left a ~125 ms stall on the bake frame, and a
  first frame that composited instead of shading deferred the shading
  program's own first-draw stall to the first config change - keep all three
  first draws on that frame. R16F, over the glow quad's box on the viewport's
  pixel grid (3.3 MB for a 960 x 540 rect at 1080p); no 8-bit fallback - a driver
  that cannot render half float draws pass 1 directly (`mFieldUnavailable`).
  The composite finds its texel from `vPos` (`floor((vPos - uFieldOrigin) *
  uFieldTexelScale)`), not `gl_FragCoord`, so a host viewport offset is fine;
  a select between the two cost the 1.0 composite ~30% on the AMD.

  **Below `1.0` the same field replaces pass 1b, under a rotating hue only**
  (I50). Pass 1b is skipped on every frame where nothing moved already
  (`mOffscreenCurrent`), so the frames left are the ones where only the time
  moved: the hue. There the bake runs at the reduced scale on the reduced
  buffer's own region and texel grid (`GetFieldRegion`), and the composite
  writes `mScaledBuffer`, unblended, in pass 1b's place; the blit and the ring
  are unchanged. The blit applies the cut and the cutoffs below 1.0, so every
  config the field takes there is unmasked. R16F at half the reduced buffer's
  size (0.84 MB at 0.5 for that rect). 1.1-2.0x on hue frames on the AMD 5300M (1.55x at 0.5),
  within 1/255; every other frame type unchanged. `IsFieldEligible` requires
  the rotating hue below 1.0, so a still config there never compiles or
  allocates it. **The factorisation is an invariant of
  `neon.frag`:** anything new there that reads `col` non-linearly, reads time,
  or multiplies after the tone map on a config the field takes breaks it -
  extend `NEON_FIELD_BAKE` and `neon-field.frag` together, or narrow
  `IsFieldEligible`. `neon-scale-check` captures the SECOND frame after a
  config change for this reason: the first draws pass 1 directly.

  **A uniformly lit ring skips its perimeter position** (`uPerimeterUnread`,
  I45): with no segments, every lit arc over the whole ring and no stops of its
  own, and a gradient ring opaque at every texel (`GradientRingLUT::IsOpaque`),
  nothing reads `sPos`, so `perimeterPosition` and the gradient alpha read are
  skipped - 1.11x on the default frame. `IsPerimeterUnread` on the CPU must
  stay at least as strict as every reader of `sPos` in `neon.frag`: add a
  reader and it has to know.

  **Two cheap paths through the halo / bloom pieces**, which are now most of the cost at 1.0: `bloomSegment` is ONE two-argument `atan` (the exact identity for a difference of two) - and that `atan`, with the corner development's `th`, is `minimaxAtan` from `neon-pieces.glsl` rather than the driver's (error under 1.7e-6 rad; 1.07-1.11x on frames whose config animates on an AMD 5300M, unmeasured on the M2 - I48) - and both kinds of piece are skipped where they add nothing. A straight's bloom is skipped past `reach` (exactly 0 there; each pair of edges shares its pedestal). A corner arc is skipped WHOLE past `uCornerSkip` from its circle (`GetCornerSkip`): a CPU bound (`GetCornerArcBound`, the same mirror `GetGlowInnerReach` uses, via `GlowBoundTerms`) where its bloom is exactly 0 and its halo under a quarter of half a level, so four skipped arcs stay under half a level - at most 1/255 on rounding-boundary pixels. Change the halo / bloom terms and `GlowBoundTerms` has to follow, for both bounds.

  **The decoupled gather and the edge ring** (`docs/neon-resolution-scale-plan.md`, sections 7 and 13). The loop itself is `gatherPerimeter`, in `neon-gather.frag` - pass 1a at every scale: the loop and nothing else - no culls, deliberately, since other resolutions read its texels - and stores its four results (colour, arc coverage, segment colour and coverage, coverages encoded `c / (1 + c)`; `neon.frag`'s read decodes them, so the two change together) in `mGatherBuffer` - 1-2 attachments, **RGBA16F** with an RGBA8 fallback (`GATHER_FORMATS`, `resizeGatherBuffer`; 8 bits read 3/255 off `1.0`, half float 1-2) - drawn **unblended**; and `neon.frag`, which reads those back with a bilinear fetch and shades everything else - pass 1b at the reduced scale into `mScaledBuffer` (one RGBA8 attachment, unblended), and pass 2c, the ring, at full resolution (at scale `1.0` the same program shades the whole glow quad) - compiled into TWO program objects (`mNeonShadeShader`, `mNeonRingShader`) so that each draws one target in one blend state per frame. The gather's outputs are Lorentzian-weighted means whose kernel is never narrower than `kc = perimeter * COLOR_BLEND_PERIM_FRAC`, so pass 1a runs at `GetGatherScale` - `GATHER_TEXELS_PER_KERNEL` (2) texels per `kc`, floored at `GATHER_MIN_SCALE`, never above `resolutionScale` or `GATHER_MAX_SCALE` - which for any rect much bigger than a thumbnail is far coarser than the reduced buffer: the loop, ~95% of the cost, runs on a few thousand texels. Do not merge those two objects as a cleanup: one program drawing an offscreen buffer unblended and the caller's framebuffer blended in the same frame is build AB of the plan's section 7, measured 93-624x slower on the AMD macOS driver. Both offscreen buffers cover a **region**, not the viewport (`GetBufferRegion`): the reduced buffer what the blit reads (`mScaledOuter`), capped per axis at the viewport-sized buffer so a full-screen rect keeps exactly its old buffer; the gather buffer the gather quad's box clipped to the viewport plus the bilinear footprint, NEVER capped (a gather texel is ~10 px, and ending the buffer at the viewport edge read 5/255 off). Both keep the viewport-anchored texel grid and size their texel count from the unsnapped box plus one, rounded up to `REGION_ALLOC_STEP` (16) - so a moving or resizing rect does not reallocate a buffer every frame. `uploadShapeUniforms` is the gather program's whole uniform set; handing it the shading uniforms logs one ERROR per missing uniform. Its uniform blocks differ too - segment and sample blocks, no arc block - so `buildNeonProgram` takes `gathers` / `shades` flags for which blocks to bind. The ring's half-width R (`GetRingWidth`) is the wider of the filament's reach at full res and as pass 1 drew it, plus `RING_GUARD_TEXELS / scale` - uncapped, so a soft filament makes it wide. Pass 2b (blit) and 2c (ring) draw disjoint vertex arrays emitted from the same floats in `setupRingGeometry` (`PushAnnulus`), so no pixel is composited by both; that partition also relies on the two drawing through ONE full-res transform, which `Render` builds once and passes to `renderBlitPass` and `renderRingPass` (do not let either rebuild its own), and on `invariant gl_Position` in `neon.vert`, which every renderer compiles - do not remove it as a cleanup. `neon-scale-check partition` tests the tiling across random configs; run it after touching any of these, and on each target GPU. Neither covers more than it must: the ring is clipped to the band the glow can still be lit in (`GetLitExtent` - past the one-sided cut and the cutoffs every mask is an exact 0), and the blit covers only that band outside the ring, out to the glow's fade margin and in to `GetGlowInnerReach`, so it can be EMPTY (no fallback - drawing anything would draw over the ring). Rebuilt under `geometryDirty`, after `setupGeometry`, because the blit's frame reads `mQuadMargin`; it also builds the gather pass's quad (`mGatherVertexArray`) and the two region boxes. Quality: within 2/255 of `1.0` down to `0.125` on `docs/neon-resolution-scale-comparison.html`'s scenes except a tiny rect. **Cost is NOT a guaranteed saving**: the ring, the blit and now the shading pass are a fixed cost, so a layer already cheap at `1.0` (a tight cutoff band, a one-sided glow) can render slower below it; measured cost differs by GPU (see the plan's sections 7 and 13).

  **The glow quad's interior hole needs no cutoff** (`GetGlowInnerReach`, both paths). Past neon.frag's `reach` the filament and the straights' bloom are exact zeros, but the halo (no pedestal) and the corner arcs' bloom (one shared pedestal, short on the concave side) are not, so the hole is cut at the smallest depth past `reach` where closed-form upper bounds on those two fall under half an 8-bit level for the brightest emission the config can carry (`GetGlowEmissionBound`: intensity, the brightest arc, the summed segment boosts). It changes no pixel - the old library given an inside cutoff cutting the same hole renders byte-identically - and pays only on rects larger than twice the reach (screen-sized ones: 1.14x at the defaults, ~2x at glowRadius 2 - see [`docs/glow-inner-reach.md`](docs/glow-inner-reach.md)). It is a CPU mirror of the shader's halo / bloom terms: change those (a gain, the pedestals, `arcTangentSegment`'s weight or span, the emission's colour range) and the bound has to follow, or the hole clips a faint tail. Its emission input is gated by value (`mGlowEmission` in `OnConfigChanged`), not by the arc and segment lists, so a travelling segment does not rebuild the quad. A hole with zero extent on EITHER axis draws the plain quad: the reach routinely lands between the two half-extents, and the strips there buy nothing.

  Runs an **emission pre-pass** (`neon-emission.frag`): the gather's per-sample
  work (arc winner-take-all, segment bells, LUT fetches) is a pure function of
  `(si, uTime, config)`, so it is baked into an `N x 2` RGBA16F table and the
  gather reads it with `texelFetch`. Per-fragment cost is `O(samples)` instead
  of `O(samples * (arcs + segments))`. The invariant to preserve: **pure
  function of `(si, uTime, config)` goes in the pre-pass; anything reading
  `vPos` stays in the main shader.**

  That same purity is why the pass is **skipped on frames none of its inputs
  moved** (`isEmissionTableStale`) - the buffer is allocated once and nothing
  else writes it, so a still ring costs nothing after the frame it changes. The
  key is what the pass BINDS, not a list of config fields (`EmissionInputs`,
  `currentEmissionInputs`): `uHueRotationRate` and `uNumSamples` by value, and
  the three LUTs and two light blocks by upload count
  (`BaseLUT::GetUploadCount`, `UniformBuffer::GetUploadCount`, which counts
  only uploads that changed the bytes). So a cross-fade frame re-bakes it
  through the ring's count, an intensity, bloom, glow or geometry animation
  re-bakes nothing, and no field list can fall out of step with the shader.
  Plus `uTime`, which reaches the shader only as `si - uTime *
  uHueRotationRate`, so at rate 0 it drops out entirely. It used to be "any
  config change" (`mEmissionDirty`, removed). Adding an input to that pass
  means adding it to `currentEmissionInputs` - see
  [`docs/emission-prepass.md`](docs/emission-prepass.md) §3.

  The table is `NEON_MAX_LOOP_SAMPLES x 2` - the sample-count CEILING, with
  `numSamples` bounding only how much of it the gather reads - so its size is a
  compile-time constant and it is allocated **once, in `Initialize`**, not per
  frame. `Initialize` fails if no candidate format (`RGBA16F`, then `RGBA8`)
  allocates. Size it to the live count instead and it goes straight back onto
  the per-frame path.

  A second pre-pass, **pass 0b, the glow coverage table**
  (`neon-glow-cover.frag`, `renderGlowCoverPass`), bakes for each PIECE of the
  emitter - the four straights and four corner arcs the halo and bloom are
  summed over - how lit that piece is as each layer sees it from a fragment:
  its own arc coverage (.r halo, .g bloom) and segment bells (.b, .a) over its
  own extent, weighted by the layer's kernel about the fragment's foot, as a
  ratio to the kernel's mass over the piece. Arcs in closed form (a feathered
  arc's trapezoid against the kernel's mass and first moment, written so
  nothing cancels; `logRatio` for the bloom's moment, which lost three digits
  at a circle's centre as a plain log), segments by 16-point Gauss-Legendre in
  `theta = atan(t / c)`. `neon.frag` scales each piece's halo and bloom by one
  linear fetch of its own table (`addStraightGlowFix` / `addCornerGlowFix`)
  rather than by the coverage gathered around the fragment, which drew a thin
  line along a stretch no arc covers and a groove along it under a strong bloom
  (V19 and V20 in [`docs/review-findings.md`](docs/review-findings.md)), and
  rather than by V20's one table per PERIMETER, which ran the outline straight
  on past each piece's ends and spilled light round corners (V21). The layout
  is in `lib/shaders/neon-pieces.glsl`, injected into both the bake and
  `neon.frag` with `arcTangentSegment`: four bands of `GLOW_COVER_ROWS` rows,
  each holding one straight at its left - the fragment's projection along it
  across, its distance down - and one corner at its right - a diamond angle
  round the arc's centre across, the radius down, the arc itself halfway, a
  guard texel at each end where the diagonal behind the centre joins the two
  sides. The two share the band's columns in proportion to the straight's and
  the quarter arc's lengths: `GetGlowCoverSplit`, whole columns computed ONCE
  on the CPU and passed to the bake and every neon program as
  `uGlowCoverSplit`, because the two shaders work in different units and a
  split each rounded for itself could read a band from the wrong texels. A
  fixed share starved a large radius's corners (12 levels off on a 4K circle)
  and wasted a circle's straights. The forward maps (`glowCoverStraightUV` /
  `glowCoverCornerUV`, the read) and their inverses (`glowCoverStraightAt` /
  `glowCoverCornerAt`, the bake) sit side by side there and must stay exact
  inverses; `NEON_GLOW_COVER_BAKE` (defined in `shaders.h.in` for the bake
  only) keeps each program to its own half, since this compiler builds every
  function in a source whether `main()` reaches it or not. Lengths enter only as
  ratios, so both resolution paths share one table. 1024 x 128
  (`GLOW_COVER_WIDTH` x `GLOW_COVER_HEIGHT`), RGBA16F (1.0 MB, half V20's 2)
  with an RGBA8 fallback - or RG16F / RG8 (0.5 MB) on a config with no
  segments, whose .b / .a would only hold zeros (`GLOW_COVER_FORMATS_RG`; a
  two-channel texture samples .a as 1, so `glowCoverAt` zeroes .b / .a on
  `uSegmentCount == 0` - keep the two together; promoted to four channels the
  first frame segments appear and kept there, I43) - encoded `c / (1 + c)`, and re-baked only when one of
  its inputs moves (`mGlowCoverDirty`, gated in `OnConfigChanged` on the arcs,
  the effective segments, width, height, cornerRadius, winding and glowRadius -
  NOT any config change, I34): never on time, and not under an intensity,
  colour or other-layer animation, but EVERY frame under an animation of the
  arcs, segments, shape or glow radius, which costs 0.14-0.27 ms a frame on an
  AMD Radeon Pro 5300M - about half what V20's table did (0.17-0.55 ms) -
  everything a config change costs included: the cost to watch on a slower GPU.
  Add a uniform to `renderGlowCoverPass` and its field joins that gate. The
  flag is a mask, one bit per PIECE (I42): every texel integrates the lights
  over its own piece alone, so a changed arc or segment re-bakes only the
  pieces its old and new supports reach (`GetGlowCoverDirtyPieces`, compared
  on the PACKED blocks, abut flags included), drawn as rectangles of the table
  by geometry rather than a scissor. Every piece on a shape, winding or glow
  radius change, on a change to the brightest arc's intensity (which clamps
  every piece) and for a light covering the whole ring. Exact - verified texel
  for texel against a full bake - and 1.06-1.29x on a travelling segment's
  frames on an Apple M2 Pro; less than one band per segment would give,
  because the bake cuts a bell at 5 sigma, so a segment of length 0.1 reaches
  +/-0.18 of the ring. `GetGlowCoverPieceSpans` mirrors the bake's
  `straightStart` / `cornerStart`: move where the bake places a piece and it
  has to follow. A ring lit
  uniformly (one full arc, no segments) never reads the table, so it is neither
  baked (`IsGlowCoverUnread` - the ONE test: the shader takes it as
  `uUniformCover` rather than deciding for itself, so the bake it skips and the
  reads the shader skips cannot disagree), nor its program
  compiled, nor the table allocated: `ensureGlowCoverProgram` and
  `ensureGlowCoverBuffer` build both on the first frame that needs them, and the
  table is released when the layer is disabled (I33), and by `Update` once it
  has gone unread for `GLOW_COVER_RELEASE_SECONDS` (5 s of frame time - long
  enough for an arc animation passing through a full ring each loop, I43).
  Until then unit 5 holds the
  gradient ring as a stand-in - never read, but a texture-0 sampler draws a
  warning from Apple's driver. Things there that are load-bearing and must stay: behind a
  corner's centre `arcTangentSegment` flips which end it develops about across
  the diagonal, so the bake blends both developments there
  (`arcTangentSegmentAbout`), or a corner's table creases along its inner
  diagonal; the forward maps are divisions only - with an `atan` and `log`s in
  them a fully lit ring, which skips every read, rendered 1.16x slower at scale
  1.0 on the AMD GPU, the program carrying the gather loop there; `neon.frag`'s
  corner block runs one arc at a time (`addCornerPiece`) - developing all four
  first left sixteen floats live at the block's peak and cost another ~8% on
  every scene at 1.0, sharp-cornered ones included; and every integral in the
  bake is called from ONE place, in a loop whose bound comes from the data so
  it is not unrolled back, because each inlined copy is compile time on that
  first frame (the bake went from ~110 ms to ~70 ms on the AMD). Do not evaluate
  the integrals in `neon.frag` instead: measured, 2.0x / 3.3x on a partly lit
  ring. Verified within 2 levels of a brute-force per-fragment reference
  (samples placed by `perimeterPosition`, sharing no code with the bake) on 25
  scenes, but for 62 pixels of one segment scene at 3: the 2s are a 1 px halo on
  the largest shapes (columns) and a segment's bell (rows). `GLOW_COVER_ROWS` is
  32; 48 reads within 2 everywhere for 0.5 MB more and 1.3x the animated bake,
  and the width is the knob not to cut (768 read 6 off a thin line on a 4K
  rect).

  `Render` is a **pass schedule**: derive the transform,
  then one call per `render*Pass` method, in TWO PHASES on both paths - every
  offscreen pass first (emission table, coverage table, and below 1.0 pass 1), then everything
  on the caller's framebuffer in one run (fill, then the glow), so a tiler never
  stores and reloads the caller's target mid-frame. `Render` owns blend state; a pass owns
  its shader and, if it retargets, restores the framebuffer / viewport / blend
  it was handed - never framebuffer 0, never a forced `glEnable` (an
  `OffscreenCapture` hands the renderer a real FBO). Header declaration order,
  .cpp definition order and the pass numbering all agree; the one deliberate
  exception is documented at the declaration.

  **Shader programs are built per path, on first use** (`ensurePathPrograms`): `Initialize` builds only the emission and fill programs; the first frame at 1.0 builds `neon-gather.frag` and the ring's `neon.frag` program, the first frame below it those two plus pass 1b's `neon.frag` program and the blit, the first frame that bakes the glow coverage table builds its bake (`ensureGlowCoverProgram`; a ring lit uniformly never does), and the first frame of a config the hue-invariant field can serve builds the field's two (`ensureFieldPrograms`; never with segments, nor below 1.0 without a rotating hue). So a `neon.frag` that fails to compile no longer fails `Initialize`: it is logged once, recorded in `mFailedPrograms` and never retried, and that path draws the fill only. `OnConfigChanged` gates its rebuilds on `mInitialized`, NOT on a program's validity - do not go back to testing a program's `IsValid()`, which is false on a host that has not drawn that path yet. See
  [`docs/emission-prepass.md`](docs/emission-prepass.md) for the pass tables and
  [`docs/emission-prepass-comparison.md`](docs/emission-prepass-comparison.md)
  for the measured before/after of the pre-pass commit alone.
  [`docs/branch-vs-main-comparison.md`](docs/branch-vs-main-comparison.md) is
  the wider view: the whole branch against `main`, so it also covers the
  colour-stop alpha and stop-sorting behaviour changes that ship with it.
- `DebugRenderer` - every debug annotation, in one layer: the baked ring as a LUT strip (`neon-lut-debug.frag`), one disc per colour stop (`neon-stop-marker.frag`), and the 1px `GL_LINE_LOOP` bounding box (`wireframe.frag`, absorbed from the old `WireframeRenderer`), behind `DebugConfig::showGradientLUT` / `showColorStops` / `showWireframe`. Register it **last** - it annotates what the layers under it drew, so its overlays have to sit above all of them (the C ABI registers it last too, and gives it the last flag bit). Always full-res, whatever the neon's resolution scale. Reads `Config::debug` for what to draw and `Config::neon` for what it is describing. The strip and the markers annotate the GLOW and are suppressed when it is absent (neon off, or `debug.opaqueOnly`); the box annotates the GEOMETRY and survives both. Note the box now draws **over** the glow - `WireframeRenderer` was registered first and drew under it, and the overlays that annotate the glow have to follow it. Bakes its **own** `GradientRingLUT` from the same inputs, which is what keeps `NeonRenderer` free of every debug member. That ring is maintained only while the strip is actually on screen - one predicate, `IsStripVisible`, gates the bake, the cross-fade tick and the draw, so they cannot drift. Because stop changes made while it is hidden are therefore never baked, the catch-up bake on re-show **snaps** instead of cross-fading (`mStripVisible`): a fade from a ring nobody has seen for the last however-many seconds would leave the strip previewing colours the glow settled away from long ago.
- `DropletsRenderer` - rain-on-glass droplets in a band hugging the perimeter; screen-space gravity, self-lit drops, no framebuffer capture. Draws a **band-fitted ring** - four strips bounding the band itself - rather than a fullscreen quad, so the pass costs what the perimeter and the band width cost rather than what the display costs. `droplets-tuning.h` holds the one constant the ring and the shader's discard must agree on; widen one without the other and the band clips to a straight line. The strips must tile without overlapping: this pass blends premultiplied, so a double-covered pixel composites twice. The other invariant is in the shader: the height-field gradient costs two more full evaluations of the droplet field, so it is **gated on `c.x > 0`** - exact, because `c.x = 0` annihilates both terms the normal reaches (`rim` and `spec`). Ungate it and the pass costs ~1.6x more for identical output. No resolution scale: unlike neon and the flare this pass never shaded the whole viewport, and its rims and speculars are single-pixel features a half-res blit would erase.
- `LensFlareRenderer` - sun + hex-aperture flare (rays, chromatic ghosts) as a fullscreen premultiplied-alpha pass. The sun rides the perimeter in the same parameter space as neon segments/arcs. Like `NeonRenderer` it is **one renderer with two resolution paths**, selected by `LensFlareConfig::resolutionScale`; unlike the neon's, only two uniforms differ between them (`uResolution` and `uSunPos`), because the flare shader normalises every term by the resolution and is therefore scale invariant.

  The ghost loop is ~88% of the fragment cost, so three things about it are load-bearing. **Per-ghost distance and colour are CPU work**, baked into the std140 `GhostBlock` by `BakeGhostTable` - they are pure functions of the ghost index and the config, the same invariant the neon emission pre-pass rests on, one tier cheaper (one 160-byte block, no texture, no pass). It consumes `ghostOffset` / `ghostColor` / `ghostTint` entirely, which is why the shader declares no uniforms for them - and it runs in `OnConfigChanged`, gated on exactly those three fields, so `Render` only binds. (Narrow rather than wide, unlike the neon table's gate, because the three inputs are visible in the same file; `rotationRate` and `spread` move every frame under an animation and change nothing here. `Initialize` bakes unconditionally so the block is filled whichever order registration and initialisation happen in.) **The bloom and ring terms are gated** on exact support bounds derived from `ghostSize` in `GetGhostBloomRadius` / `GetGhostRingFloor`; the constants those share with the shader terms live in `lens-flare-tuning.h`, and changing one there without rebuilding the derivation silently clips ghost pixels. **The hex sprite is deliberately NOT gated the same way** - `hexCoverage` calls `fwidth`, and a derivative in non-uniform control flow is undefined; the `uSpread` guard around the whole loop is safe only because it branches on a uniform. See [`docs/lens-flare-perf-review.md`](docs/lens-flare-perf-review.md) for the measurements and the one open defect.

- `SpotlightRenderer` - freely placed and aimed cones of light, as ONE screened pass. The odd one out: it is **not a perimeter effect** and reads nothing but `Config::spotlight` - a lamp sits at a `SpotLight::position` in **app coordinates (origin top-left, +y down**, the same space as `RectGeometry::position`) and points at a `SpotLight::angle` in degrees, 0 = right, increasing clockwise. Moving the rect therefore does NOT move the rig. Light only: no backdrop, no fixture housings, no capture, no LUT, and nothing occludes a cone - the one thing that stops light is `SpotlightConfig::clipArea`, a rounded-rect area in the same app coordinates, kept on whichever side `ClipMode` names and opted into per lamp by `SpotLight::clipped` - which is the ONLY gate, since `ClipArea` deliberately carries no enable flag (the trade: its default is 0x0, and a zero-size `KEEP_INSIDE` area keeps nothing, so a lamp opted in before the rect is set goes dark silently). Its type is `ClipArea` (`ClipMode` beside it), a **shared** shape in the top-of-file types block of `config.h` next to `Cutoff` and `BlendSpace` - nothing about a region and which side of it to keep is spotlight-specific, so any layer that wants an explicit bound can take one; the spotlight is currently the only consumer. That is a coverage multiply on the finished shading, not a shadow and not a change to the falloff, so a clipped lamp is the same lamp with part of it missing; under `KEEP_INSIDE` the strip solve ALSO intersects each clipped lamp's bound with the area's box in that lamp's frame, so the clip buys fragments back rather than only hiding them (`KEEP_OUTSIDE` keeps an unbounded region, so it cannot). Light only, but NOT alpha-free: the pass SCREENS, in ALPHA as well as colour (`glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE)`, i.e. `dst + src * (1 - dst)`), and the shader writes the same max-of-channels coverage neon and the flare do. A screen rather than an add because the highlight shoulder is per lamp and cannot bound a SUM: added, two crossing beams at ~0.6 clipped to a flat white disc with a rim (V12a in `docs/review-findings.md`). Putting the shoulder on the sum instead was built and rejected - 2.5x to 9.3x the GPU time, and a crease where differently coloured beams cross - so do not go back to it without reading V12a. It used to emit a literal alpha 0, which costs nothing on an opaque desktop window and erases the entire layer on an embedded surface a compositor or hardware video plane blends by alpha - the Tizen "spotlight missing over a playing video" bug. Any new layer here must write a coverage alpha for the same reason.

  **The geometry is the interesting part.** Each lamp gets a strip of `SPOT_STRIP_SEGMENTS` quads hugging the region where it writes anything at all, all lamps in one VBO drawn with one `glDrawArrays`. The bound is **solved, not guessed**: `SolveConeAcross` inverts `spotlight.frag`'s own falloff to find where it drops below half an 8-bit step, shared across the enabled lamps so the rig's whole clipped remainder stays under one half step. That the strip never clips a lit fragment rests on **two** things, not just the sample widening the code is written around: the widening does not bound the chord at the corner the support carries at `along = 0` (measured cuts of up to 1.7 px at a long throw or a boosted tint), and what covers that is the near-end fade `SolveConeAcross` deliberately does not invert. So `SPOT_NEAR_FADE` and `SPOT_STRIP_SEGMENTS` are load bearing in ways their own comments used to understate - read the widening note in `buildStrips` before moving either, and re-run the grid check it describes.

  **One renderer, two resolution paths**, selected by `SpotlightConfig::resolutionScale`, exactly as in `NeonRenderer` and `LensFlareRenderer` - and **not one uniform differs between them**, fewer than the flare's two. The fragment stage reads only interpolated full-res lamp-local coordinates and flat per-lamp pixel values, so the scale lives entirely in the viewport transform; only the render target, the blit and the buffer allocation are conditional. 1.0 is verified bit-identical to the single-path renderer this grew out of.

  **An enabled clipped lamp pins the scale to 1.0** (`HasClippedLamp` / `GetClampedSpotScale`), so the scaled path is only ever reached with no lamp clipped. The clip is a per-fragment mask, and a reduced buffer resolves its boundary at that buffer's texel pitch: measured on-device, requesting 0.5 with a lamp clipped deviated up to 65/255 from 1.0 and 0.25 up to 101/255, against 11/255 for the same scale change with nothing clipped. This is the defect `neon-blit.frag` documents and the neon fixed by moving its cut into the blit; that fix does not transfer, because `clipped` is per lamp while the blitted buffer holds every lamp's light summed, so a blit-time mask would cut the lamps that opted out. The configured value is kept rather than rewritten, both transitions are logged, and both demo UIs say so beside the slider. See [`docs/spotlight-renderer.md`](docs/spotlight-renderer.md) section 4.6 and I21 in [`docs/review-findings.md`](docs/review-findings.md).

  **But the scale is NOT the bargain it is for the other two, and the default is 1.0 for that reason.** Neon and the flare shade the whole viewport, so quartering their fragments always beats a blit. This layer already bounds its geometry to what the lamps light, so the blit's ~922k fragments are a FIXED floor it may never earn back. Measured with an occlusion query at 1280x720:

  | scene | scale 1.0 | scale 0.5 | scale 0.25 |
  | ----- | --------- | --------- | ---------- |
  | one lamp | 183,868 (20%) | 967,599 (105%) | 933,076 (101%) |
  | five-lamp fan | 1,131,073 (123%) | 1,204,301 (131%) | 992,285 (108%) |
  | eight lamps | 1,900,287 (206%) | **1,396,661 (152%)** | **1,040,340 (113%)** |

  A single lamp at 0.5 costs **5.3x more** than at 1.0. The crossover is where the full-res strips exceed `blit / (1 - scale^2)`: at 0.5 that is ~1.23M fragments, which the five-lamp fan (1.13M) sits just under and the eight-lamp rig clears - winning 1.36x at 0.5 and 1.83x at 0.25. At 0.75 nothing in range wins at all. Lower it only for a large rig, and measure.

  Against a fullscreen pass looping over the lamps, the strips are a 4.1x saving at five lamps. Flattening the taper to an oriented bounding box of the same solve costs **1.16x to 1.50x** more. The pathological case for area is a large `bloomRadius`, not a long throw - the aperture bloom's support is `bloomRadius * SPOT_BLOOM_WINDOW_OUTER`, so radius 90 fills a 1280x720 frame on its own.

  **Per-lamp scalars ride as vertex attributes** (`flat`-qualified), constant across each strip, rather than in a std140 block - so there is no per-index array in either stage and the no-bare-uniform-arrays rule is satisfied by construction. The vertex stage pre-rotates each corner into the lamp's frame, so the fragment program never touches a sin, a cos, or `gl_FragCoord`.

  **The VBO holds app coordinates verbatim**; the y-flip lives in a flipped ortho (`glm::ortho(0, w, h, 0, ...)`), so the buffer depends only on `Config::spotlight` and a resize costs one uniform rather than a rebuild. The flip reverses winding, which `buildStrips` compensates for by winding its triangles the other way round; it used to be written off as harmless "because nothing here enables `GL_CULL_FACE`", which was a claim about the library where the host owns the state. `EdgeLightingEffect::Render` **no longer forces any host-owned state**: it used to take a `GLUtils::NoCullScope` and a `GLUtils::CompositeStateScope` around the whole fan-out, and both are gone from the call site. The guards still exist in `lib/include/util/gl-utils.h` for a host or a layer that wants them, but **the caller is now responsible for the state this library assumes and never sets** - `GL_CULL_FACE` off (or a front face the strips' CCW winding survives), a full colour+alpha write mask, `GL_FUNC_ADD`, and depth test/write off. Measured, and still true: a host alpha write mask zeroes the neon emission pre-pass (whose alpha channel IS the arc weight) and every layer's coverage alpha, a stray `GL_MAX` changes the composite, and culling with a reversed winding renders every layer but the debug bounding box at 0 pixels. Rebuilds happen in `OnConfigChanged`, into a ceiling-sized `GL_DYNAMIC_DRAW` allocation made once in `Initialize`. See [`docs/spotlight-renderer-plan.md`](docs/spotlight-renderer-plan.md) for the design and the verification.

There are no longer any forked renderer pairs. `NeonOptimizedRenderer` / `neon-optimized.frag` were folded into `NeonRenderer` / `neon.frag` as a resolution scale (see [`docs/neon-unification-plan.md`](docs/neon-unification-plan.md)), and `LensFlareOptimizedRenderer` was folded into `LensFlareRenderer` the same way. Both merges are byte-identical at every scale tested. A change to neon or flare appearance now lands in exactly one place.

To add a renderer, subclass `BaseRenderer` (`Initialize` / `Update` / `Render` / `OnConfigChanged` / `GetLayer`), give it a `RendererLayer` (see the C ABI note below), add a sub-config struct to `Config` with `operator==`, register it in `demo/src/main.cpp`, and add an ImGui section in `DebugUI`.

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
- **Layer order is host-selectable, and the core owns it.** Every renderer names its layer (`BaseRenderer::GetLayer`, pure virtual, returning a `RendererLayer` from `renderer/base-renderer.h`), and `EdgeLightingEffect` builds and reorders the stack by layer: `AddRenderer(RendererLayer)`, `GetLayerOrder`, `SetLayerOrder`, `GetDefaultLayerOrder` (neon, droplets, flare, spotlight, debug). `RendererLayer`'s values ARE `el_renderer_flags_e`'s bits - static_asserts in `capi-internal.h` - so the C ABI keeps no bookkeeping: `el_effect_init_with_renderers(mask)` filters the default order, `el_effect_init_with_renderer_order` takes one flag per entry, bottom first, and `el_effect_set_renderer_order` / `el_effect_get_renderer_count` / `el_effect_get_renderer_at` cast and forward. A host's own renderer returns `RendererLayer::CUSTOM`; the layer calls skip it and it keeps its slot through every reorder. A new layer is added to `RendererLayer`, `CreateRenderer` and `GetDefaultLayerOrder` in `edge-lighting.cpp`, and mirrored by an `el_renderer_flags_e` bit plus its static_assert.
- Symbols are hidden by default (`CXX_VISIBILITY_PRESET hidden`); only `EL_API`-marked `el_*` functions are exported.

### RAII GL wrappers

`lib/include/gl/` provides move-only RAII wrappers: `ShaderProgram` (with uniform-location and last-value caching), `VertexArray`, `Framebuffer`, `UniformBuffer`, and `Texture` (base) + `Texture2D`. `lib/include/util/gl-utils.h` adds the scope guards for host-owned state: `NoScissorScope` (per offscreen pass), `NoCullScope` and `CompositeStateScope` (both available but **taken by nothing** - `EdgeLightingEffect::Render` no longer takes them, so their state is the host's to supply). `gl-header.h` is the single include for `<glad/glad.h>`. Use these wrappers - do not call `glGen*` / `glDelete*` directly in renderer code. A multi-pass renderer must return to the framebuffer it was handed (`Framebuffer::GetBoundId()` before its offscreen pass, `Framebuffer::BindId(prev)` after), not to `BindDefault()`: during a frame capture the "backbuffer" is a real FBO.

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
