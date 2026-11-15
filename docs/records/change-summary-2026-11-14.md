# What the uncommitted work changes, and why (2026-11-09 to 2026-11-14)

The working tree of `add_renderable_target_formats` carries a week of work on
top of `d6904ea`, none of it committed. This page is the one place that says,
for all of it, what changed and why. It is a summary: the dated entries in
[`progress-log.md`](progress-log.md) hold the measurements in full, and
[`upgrade-notes.md`](../guides/upgrade-notes.md) sections 10-13 say what a host
has to do. When this page and the headers under `lib/include/` disagree, the
headers win.

Size of the change: 53 tracked files, +1,717 / -3,357 lines (code and tools
alone: 39 files, +1,220 / -3,245), plus three new files -
`lib/include/core/config-validation.h`, `lib/src/core/config-validation.cpp`
and [`docs/design/neon-renderer-notes.md`](../design/neon-renderer-notes.md) -
and one removed, `lib/include/gl/texture.h`.

Where it came from: three reviews of the whole tree - a design and
implementation review with a config fuzzer (2026-11-09), a simplification
review in two passes (2026-11-10, 2026-11-13) whose rule was that a finding
must remove a mechanism and be measured where a number is possible, and an
architecture review (2026-11-14). Each change below was either a defect those
reviews reproduced or the owner's pick from their lists.

## At a glance

| # | change | why | does a host see it? |
| - | ------ | --- | ------------------- |
| 1 | Every config value is held to a range before a renderer sees it (`ConfigValidation`) | Finite but absurd values hung the CPU, hung the GPU and hit undefined behaviour (I105-I108, V38) | Only with absurd values: they are clamped instead of hanging. `upgrade-notes.md` section 10 |
| 2 | The neon's history moved out of its comments; the C ABI's checks name the function themselves | Comments carried measurements, not invariants; 538 hand-typed function names could drift | No |
| 3 | Shader `//` comments stripped at configure time; one list of shader files | 0.79 MB of the 2.15 MB dylib was shader comments; two lists of the same 35 files (I84) | Smaller binaries only |
| 4 | Dead public C++ API removed; `Texture` folded into `Texture2D` | No caller anywhere in the library, C ABI, demos or tools | A C++ host that called one of them stops compiling. `upgrade-notes.md` section 11 |
| 5 | A failed buffer allocation is final; the out-of-memory retry is gone | A retry clock, refresh lists and five gate cases for a shortage neither Mac reaches | Only after a failed allocation. `upgrade-notes.md` section 12 |
| 6 | The spotlight's resolution scale: removed, then restored on measured frame times | The removal rested on fragment counts; wall-clock time showed the path pays on large rigs | No (net); its documented cost is corrected |
| 7 | `SetConfig` no longer calls the renderers | It did GL work wherever the host called it, once per call | A host's own renderer hears of a change at the next `Update` / `Render`. `upgrade-notes.md` section 13 |

Verified for all of it, on an Apple M2 Pro in Release: the page set, the
`cover` set and the guide figures byte-identical to the build before
(582 images); all seven `neon-scale-check` gates pass (`check`, `partition`,
`shared-buffer`, `clock`, `flare-reuse`, `sequence`, `formats`); Release and
Debug build. Re-checked on 2026-11-14 against a Release build of `d6904ea` in a
scratch worktree: the page set (210 images and its metrics), the `cover` set
(306), the guide figures (69 files) and the `clock` frames (30) byte-identical;
a probe of 215 frames over the layers the tools do not image - droplets, the
flare and five spotlight rigs at 1.0, 0.5 and 0.25, the debug overlays, the
full stack at 720p and 1080p, nine animation presets, resizes, layer toggles
and `SetConfig` sequences - byte-identical but for six frames of one sequence,
1 level apart, explained in section 7. And a seeded fuzzer of the neon (with the
debug layer, its shared-buffer inset on in about a third of the configs): every
`NeonConfig` field, the geometry, six viewports from 640 x 1136 to 4K and the
background colour drawn at random, run as 600 unrelated configs on one effect
(three frames each), 120 on a fresh effect, eight runs of 150 frames of small
changes, and 50 random configs under one to three presets. Six seeds, 22,440
frames, all byte-identical; no warning or error logged by either build. That
fuzzer is now a gate, `neon-scale-check fuzz` (its README section), which also
draws the base gradient's stops at random; seeds 1 to 7 of it against
`d6904ea` (26,180 frames, both builds Release) are byte-identical too. A
re-audit on 2026-11-15 also ran every gate and a fuzz seed under ASan and UBSan
(no report, all pass), and found the exported C symbols identical to
`d6904ea`'s (264). A Debug build against a Release one differs by 1 level on a
few frames from CPU rounding alone - compare within one build type. Not run: the AMD Radeon Pro 5300M, the Tizen device, the demos by
hand.

## 1. One validation layer for the config (I105-I108, V38)

**What was wrong.** Each field's range lived in prose and in each renderer's
own clamps. The C ABI refuses NaN and infinity and nothing else, and an
animation's overlay reaches the config without even that. A fuzzer feeding
random finite values found:

- **I105** - a spotlight throw of 1e10 px: the 1 px bisection in
  `SolveConeReach` never finished once its two ends were adjacent floats, so
  `SetConfig` and `el_effect_update` never returned.
- **V38** - a negative width larger than the height gave a negative perimeter;
  with segments on, every frame that baked the glow coverage table hung the
  GPU.
- **I106** - `GetSubWindow` converted an out-of-range texel index to `int`
  before clamping it (undefined behaviour).
- **I107** - the same unmeetable tolerance in `GetGlowInnerReach` and
  `GetCornerSkip` past about 4e6 px.
- **I108** - `RgbToHsl` divided by zero for channels above 1, and the NaN
  reached `ToByte` (undefined behaviour).

**What changed.** `ConfigValidation::Sanitize`
(`core/config-validation.{h,cpp}`, new) holds every field to a range: lengths
and positions to +/-1e5 px, a rect's sides to 0 or more, every other float to
+/-1e6, the gradient ring to 2048 texels, NaN to 0. The effect applies it to
the composited config - after the animation overlays, so both ways in are
covered - and keeps a sanitised copy of the base, refreshed by `SetConfig`, so
a frame with no animation still compares without copying. `GetConfig` still
returns the base as set. For a renderer driven without the effect, the local
guards are fixed too: every float bisection stops when its midpoint stops
moving (`BisectionStalled`), `GetSubWindow` clamps before converting, the neon
draws no glow for a rect with a negative side, and the two colour functions are
safe.

**Why this way.** One place that knows every range, applied where every value
passes, rather than a clamp per renderer that each new field has to remember.
The bounds are far outside every gate scene, so no frame a gate draws can move.

**Verified.** Every reproduction returns or draws; the fuzzer runs four seeds
of 1,500 frames with no hang, no GPU or GL error and no ASan or UBSan report.

## 2. History out of the neon's comments; the C ABI names itself

**What changed.**

- 21 comment blocks in `neon-renderer.{h,cpp}` cut down to the invariant they
  protect; the measurements and history moved verbatim to
  [`neon-renderer-notes.md`](../design/neon-renderer-notes.md), as was done for
  the shader in `neon-frag-notes.md`.
- The C ABI's validation macros (`VALIDATE_*`, `REQUIRE_FINITE`,
  `REQUIRE_DURATION`) take the entry point's name from `__func__`: 538 name
  literals removed from `lib/capi/`. None differed from its function, so every
  log line reads as before.
- Removed with no caller: `ShaderProgram`'s four array `SetUniform` overloads
  and `UNIFORM_ARRAY_DIRECT`, two unused parameters of `renderEmissionPass`.
  `FieldBoundAnimation`'s parallel read and write switches became one
  (`scalarSlot`).

**Why.** A comment that retells a measurement goes stale and hides the rule
the reader needs; a hand-typed function name can be copied wrong and nothing
checks it; two switches over the same enum drift.

**Tried and dropped.** Splitting `neon-renderer.cpp` into four files with three
internal headers was built and was byte-identical, but it added seven files and
removed no concept, so it was reverted.

## 3. Shader comments stripped at configure time (I84)

**What changed.** `lib/CMakeLists.txt` had two hand-written lists of the same
35 files (`CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)`). Now one list,
`EDGE_LIGHTING_SHADER_TEXTS`, and one loop read each file, strip its `//`
comments - keeping every newline, so a compile error's line number still points
into the file - and register the dependency.

**Why.** The comments shipped in the binary and were lexed by the driver at
every program build, and a new shader had to be added in three places.
Measured: `shaders.h` 936 -> 162 KB, the C ABI dylib 2.15 -> 1.38 MB, the static
library 1.98 -> 1.20 MB. Adding a shader is now two places (that list and
`shaders.h.in`).

**The rule it brings.** The strip is plain text: no injected file may hold a
string literal or a `/* */` comment. None does today.

**Open.** The largest literal is still 34.7 KB, over MSVC's 16 KB limit.

## 4. Dead public C++ API removed

**What changed.** Removed: `GLUtils::CheckExtension`, `CheckGLError` /
`GL_CHECK_ERROR`, `LogExtensions`; `Framebuffer::BindDefault`;
`Texture2D::SetDataFromFile` (the library's only `stbi_load` in a header); the
presets `GlowRadiusCurve`, `BloomStrengthCurve`, `HueRotationRateCurve`;
`FieldBoundAnimation`'s eight binding getters, `GetBindingCount` and
`ClearBindings`. `Texture`, whose only subclass was `Texture2D`, is folded into
it, so `gl/texture.h` is gone and a LUT no longer carries a vtable pointer.
`Initialize` now logs the GL vendor, renderer, version and extension count
instead of one line per extension.

**Why.** Nothing in the library, the C ABI, the demos or the tools called any
of it; each was surface to keep compiling and documenting.
`BindDefault` was already documented as never to be used.

**Kept on purpose.** `NoCullScope` and `CompositeStateScope` have no caller
either, but stay for a host that wants them. The C ABI is unchanged (264
exports).

## 5. A failed allocation is final, out of memory included

**What was there.** An out-of-memory failure expired after 2 s of frame time
(doubling to 512 s), and every renderer then moved each buffer back up its
format list once a frame. That needed frame time fed through
`EdgeLightingEffect::Update`, a refresh list in each renderer, a check that a
hue-invariant field was not composited after one of its inputs changed format,
and five `formats` gate cases to prove it all.

**What changed.** A full-size allocation that fails, for any reason, holds that
format at that attachment count and above, at every size, for the effect's
life. A buffer that fell back stays where it landed; recreating the effect is
the way back. Removed: `FormatAvailability::Advance` / `GetTime` /
`SetOutOfMemoryRetry`, the retry constants, `isBelowBestRow`,
`AdaptiveFormatTarget::Refresh`, `SharedBuffer::Refresh`,
`BaseRenderer::AdvanceOwnFormatAvailability`, `NeonRenderer::refreshTargets`,
`FieldInputFormats`, `LazyBake::DropIfInputsMoved`, the flare's per-frame
refresh, and gate cases D2, D3, D7 and D8. `format-availability.{h,cpp}`
1,258 -> 1,010 lines; the `formats` gate's files 2,177 -> 1,450 lines and about
70 s -> 5 s.

**Why.** All of that served a shortage neither Mac ever reaches, and its cost
was paid in code on every frame and in the gate.

**The cost, accepted by the owner.** One failure, on any buffer, puts every
buffer of that format on its next row until the effect is recreated. Measured:
a failure forced on the flare's RGBA16F buffer moved the neon's shared and
gather buffers to RGBA8, 3 levels from a run with no failure. Whether the Tizen
device ever fails a full-size allocation is still unmeasured.

## 6. The spotlight's resolution scale: removed, then restored

**What happened.** The simplification review proposed removing the spotlight's
reduced-resolution path on the spotlight plan's fragment counts (one lamp at
0.5 cost 5.3x its 1.0 frame), and it was removed. It was then measured in
wall-clock frame time instead (the layer alone, no timer query in the loop):
one lamp is 5-17x slower at 0.5, five lamps break even, eight lamps run at
0.60-0.72x, and eight wide lamps with a large bloom at 0.34-0.41x (0.14-0.22x at
0.25). A blit fragment is far cheaper than a lamp fragment, so the counts had
overstated its cost. The path was restored, with the final-failure rule of
section 5.

**Net change against `d6904ea`.** No feature change: the field, its default of
1.0, the clip pin, the blit and both C exports are as they were. What changed is
what the docs and comments say it costs - the break-even is about six lamps,
given in frame times (`spotlight-renderer.md` section 6) - and the renderer no
longer advances a format clock.

**Why it is recorded.** It is the reason for a rule worth keeping: measure
frame time on the old tree before cutting a path, never fragment counts.

## 7. `SetConfig` no longer calls the renderers

**What was there.** `SetConfig` notified every renderer before it returned, and
each `OnConfigChanged` rebuilds on the spot - LUT bakes, uniform blocks, vertex
buffers, buffer releases. Setting a config was GL work wherever the host
happened to call it, and N calls in a frame were N rebuilds.

**What changed.** `SetConfig` writes the base and recomposes the active config
(CPU only, so `GetActiveConfig` follows at once) and marks the renderers stale
(`mRenderersStale`). `notifyRenderers` hands the active config to every renderer
from `Update`, or from `Render` when no `Update` came first - once, however many
`SetConfig` calls came between.

**Why.** It was the architecture review's first lifecycle finding: keep every
renderer's GL work on the frame, and coalesce repeated sets. The C ABI already
behaved this way (staging reaches the effect in `el_effect_update`), so this
brings the C++ API in line with it.

**What a host sees.** A host's own renderer hears of a change at the next
`Update` or `Render`, not inside `SetConfig`. A change set and set back before
then may notify once with an equal config, which every renderer already
tolerates. `AddRenderer` still hands a new renderer the current config at once.

**Verified.** A GL-free probe with a counting renderer (three `SetConfig` calls
notify nothing, `Update` notifies once with the last config, `Render` with no
`Update` notifies first, an equal `SetConfig` notifies nothing), plus the image
sets and gates above.

**The one frame it moves.** An A/B probe against `d6904ea` (215 frames over
every layer, nine animation presets and four `SetConfig` sequences) found it in
one sequence only: a host that sets a change and sets it back before the frame
(`SetConfig(b); SetConfig(a); Update`). `d6904ea` told the renderers of `b`
and then `a`, so the neon took the frame as a config change and shaded it
directly; now the renderers hear of `a` alone, nothing moved, and the neon
composites its hue-invariant field. 1.5-2.7% of the pixels differ, each by
exactly 1 level, and the new frames are byte-identical to the frames of a host
that set `a` once - which are themselves byte-identical between the two
builds. Every other probe frame is byte-identical.

## Where the changes are, by area

| area | files | what |
| ---- | ----- | ---- |
| Effect core | `core/edge-lighting.{h,cpp}`, `core/config-validation.{h,cpp}` (new), `core/config.h` | Sections 1 and 7; the startup log (section 4) |
| Formats | `core/format-availability.{h,cpp}`, `core/adaptive-format-target.{h,cpp}`, `core/shared-buffer.{h,cpp}`, `renderer/base-renderer.{h,cpp}` | Section 5 |
| Renderers | `neon-renderer.{h,cpp}`, `spotlight-renderer.{h,cpp}`, `lens-flare-renderer.cpp` | Sections 1, 2, 5, 6 |
| GL wrappers and utilities | `gl/framebuffer.h`, `gl/shader-program.h`, `gl/texture-2d.h`, `gl/texture.h` (removed), `renderer/base-lut.h`, `util/gl-utils.h`, `util/color-utils.h` | Sections 1, 2, 4 |
| Animation | `animation/field-bound-animation.{h,cpp}`, `animation/neon-animations.h` | Sections 2 and 4 |
| C ABI | `lib/capi/*.cpp`, `capi-internal.h`, `el-effect.h` | Section 2 (`__func__`), section 6 (comments) |
| Build | `lib/CMakeLists.txt` | Section 3, and the new validation source |
| Gates | `tools/neon-scale-check/src/format-check*.{h,cpp}`, its `README.md`; `src/fuzz.{h,cpp}` (new) | Section 5; the `fuzz` gate that checked all of it |
| Docs | `CLAUDE.md`, `docs/` (14 files), `neon-renderer-notes.md` (new) | Every section; this page |

## Open after this work

- **Not measured or run:** the AMD 5300M, the Tizen device (including whether it
  ever fails a full-size allocation), the demos by hand.
- **Not done:** no gate runs the config fuzzer; the C ABI's `set_*_count`
  setters are uncapped; MSVC's literal limit (section 3); the neon sources are
  still about half comment.
- **The architecture review's other 23 items are untouched.** In short:
  packaging (the C ABI cannot load GL itself and exports about 4,000 glad
  symbols; no version or install rules; test hooks are public API); lifecycle
  (no teardown or context-loss path; the host GL state contract is implicit;
  failures reach stdout only); the config model (one field lives in about ten
  hand-kept places; change detection is hand-written at three levels; layering
  cycles between `core`, `renderer` and `util`); renderer internals
  (`NeonRenderer`'s size; CPU mirrors of shader maths kept in step by prose; the
  flare's and spotlight's scaled paths are twins; speculative generality in the
  shared buffer registry); animation presets duplicating `FieldBoundAnimation`;
  and verification (no unit tests, CTest or CI; two hand-forked demo UIs).
  Each waits on an owner's decision.
