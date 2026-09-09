# Unify the neon renderers, extract a DebugRenderer

**Status: done.** All seven parts landed. This is kept as the record of what the
merge involved and, more usefully, what was verified about it - other docs point
here when they refer to the fork that used to exist.

The headline result, measured rather than argued: the unified renderer's output
is **byte-identical** to the renderers it replaced, at every scale tested.
Six cases were rendered offscreen through `OffscreenCapture` against both this
tree and a worktree at the pre-merge commit:

| case | `resolutionScale` | samples | result |
| ---- | ----------------- | ------- | ------ |
| full | 1.0 | 128 | identical to the old `NeonRenderer` |
| half | 0.5 | 64 | identical to the old `NeonOptimizedRenderer` |
| quarter | 0.25 | 32 | identical to the old `NeonOptimizedRenderer` |
| debug overlays | 1.0 | 128 | identical - `DebugRenderer` reproduces the in-renderer overlays exactly |
| opaque-only | 1.0 | 128 | identical |
| debug overlays, scaled | 0.5 | 64 | **differs, by design** - see below |

The last row is a new capability, not a regression. The overlays used to live
inside the full-res renderer only, so a half-res frame drew none; they are their
own layer now and draw at full resolution over the scaled glow. The glow beneath
is unchanged.

The C ABI shims were verified separately, through a C-only program against the
built `.dylib`: 16 assertions covering the defaults, both directions of the
enable mapping, the "an explicit scale survives a redundant enable" case, and
the knob round-trips.

### Follow-ups that landed after the seven parts

Cleanups the merge made possible or exposed, in the order they were done:

- **`opaqueOnly` moved to `DebugConfig`.** It is a debug control, so it sits
  with the others; `NeonRenderer` reads that one field back because it selects
  which of its passes run.
- **`WireframeRenderer` absorbed into `DebugRenderer`.** The 1px box is a third
  overlay behind `DebugConfig::showWireframe`; `WireframeConfig` is gone and
  `EL_RENDERER_WIREFRAME` became a deprecated alias for `EL_RENDERER_DEBUG`
  (since removed, and the surviving flags renumbered dense from bit 0 - see
  `el-types.h`). The box
  now draws OVER the glow, since the layer that owns it has to follow the neon.
- **All three `bool` latches removed from `NeonRenderer`.** The two overflow
  flags became a transition test in `OnConfigChanged` against counts already
  stored; `mEmissionFloatUnavailable` became a candidate-format walk that reads
  the buffer's own format. Doing this surfaced a real bug - the segment overflow
  warning could never fire; recorded under V7 in `review-findings.md`.
- **The emission buffer is allocated once, in `Initialize`.** Its dimensions are
  compile-time constants, so it never belonged on the per-frame path.
  `renderEmissionPass` is now `void`, and `Initialize` fails if no candidate
  format allocates - where the renderer previously degraded silently to
  fill-only for the rest of the run.
- **Naming and ordering swept.** `GetClampedResolutionScale` /
  `GetClampedNumSamples` (verb-first PascalCase, matching every other free
  helper in the tree) replaced two vaguely-named statics and moved out of the
  class; definition order in both renderers' `.cpp` now matches their headers,
  which `CLAUDE.md` had long claimed but only the pass list actually honoured.

Two deviations from the plan as written, both noted in place below:

- `DebugRenderer` also honours `neon.opaqueOnly`, so fill-only debug mode stays
  uncluttered exactly as it was. The plan did not call for that cross-read.
- `DebugConfig` sits next to `NeonConfig` in `config.h` rather than next to
  `WireframeConfig` - it reads better beside what it annotates.

## Context

`NeonRenderer` and `NeonOptimizedRenderer` are near-forks: same UBO packing, same LUT
bakes, same emission pre-pass, same uniform wall. Measured, the divergence is tiny:

- **Shaders**: comment-stripped, `neon.frag` (314 lines) vs `neon-optimized.frag` (317)
  differ in **3 uniform declarations and 5 code lines** - `uResolutionScale` scaling
  three px constants, and `uNumSamples` as the gather loop bound. At
  `uResolutionScale = 1.0` / `uNumSamples = NEON_MAX_LOOP_SAMPLES` the two are
  arithmetically identical (`FILAMENT_MIN_HALF_WIDTH` is 0.5, so the added
  `max(minHalf, 1e-3)` guards never engage).
- **C++**: the real differences are the scaled FBO + blit passes, the `optimizedNeon.*`
  reads, and the two debug overlays that only the full-res renderer has.

The cost of the fork is spelled out in `CLAUDE.md`: "A change to neon appearance
generally has to land in **both** copies to stay consistent." The two paths are also
mutually exclusive in practice - `demo/src/debug-ui.cpp:311` warns when both are
enabled, and `demo-capi` has the same `BothNeonPathsWarning`.

Outcome: **one** `NeonRenderer` whose `resolutionScale` selects the path (1.0 = today's
full-res, direct-to-target; < 1.0 = scaled FBO + bilinear blit), and a separate
`DebugRenderer` owning the LUT-strip and colour-stop-marker overlays. Full-res output
must be pixel-identical to today.

Decisions taken (from the clarifying questions): single class with a scale knob,
debug pieces as a **new registered renderer** named `DebugRenderer`, and the
optimized-neon C ABI kept as deprecated shims so no downstream host breaks.

---

## Part 1 - Config

`lib/include/core/config.h`

**`NeonConfig` gains** the three perf knobs, with defaults that reproduce today's
full-res behaviour exactly:

```cpp
float resolutionScale = 1.0f;                  // 1.0 = full-res direct path
int   numSamples      = NEON_MAX_LOOP_SAMPLES; // 128
int   gradientLutSize = 256;                   // was the GRADIENT_LUT_SIZE constant
```

**`NeonConfig` loses** `showGradientLUT` and `showColorStops` (they move to
`DebugConfig`). `opaqueOnly` **stays** in `NeonConfig` - it is a debug *mode of the
main pass* (it changes which neon passes run), not a separate draw, so moving it
would force `NeonRenderer` to read a config belonging to another renderer. Flagging
this in case you want it moved anyway.

**New `DebugConfig`**, placed next to `WireframeConfig`:

```cpp
typedef struct DebugConfig
{
    bool enable = true;            // master mute; both flags below default off
    bool showGradientLUT = false;
    bool showColorStops = false;
    // operator== / operator!= over all three
} DebugConfig;
```

**`Config`**: add `DebugConfig debug;`, delete `OptimizedNeonConfig optimizedNeon;` and
the `OptimizedNeonConfig` struct. Update `Config::operator==` on both counts, and both
`NeonConfig::operator==` edits - per `CLAUDE.md`, a field missing from `operator==`
silently breaks the renderers' dirty-flag gating.

## Part 2 - Shaders

- `lib/shaders/neon.frag`: add `uniform float uResolutionScale;` and
  `uniform int uNumSamples;`, then apply the five scaled lines currently unique to
  `neon-optimized.frag` (`minHalf`, `sigma`, `lineGate`, `headF`/`tailF`, `glowGate`,
  and the `for (i < uNumSamples)` bound). Fold in the optimized file's comments where
  they explain the scaling; keep the base file's longer derivation comments.
- **Delete** `lib/shaders/neon-optimized.frag`. Keep `neon-blit.frag` (the scaled path
  still needs it), `black-rect.frag`, `neon-emission.frag`.
- `lib/CMakeLists.txt`: drop `neon-optimized.frag` from **both** the
  `CMAKE_CONFIGURE_DEPENDS` list (line ~40) and the `file(READ ...)` list (line ~56).
- `lib/shaders/shaders.h.in`: drop the `NEON_OPTIMIZED_FRAG_SRC` block.

## Part 3 - Unified `NeonRenderer`

`lib/include/renderer/neon-renderer.h`, `lib/src/renderer/neon-renderer.cpp`.
**Delete** `neon-optimized-renderer.{h,cpp}`.

The merged class is essentially today's `NeonOptimizedRenderer` with `scale` sourced
from `config.neon.resolutionScale` and the FBO passes made conditional. Take the
optimized copy's versions of `setupGeometry`, `rebuildLoopSamples`, `bakeLUTs` and
`OnConfigChanged` gating verbatim - at `scale == 1.0`, `n == 128`,
`gradientLutSize == 256` they reduce to the base copy's behaviour line for line
(including the `(size + softness + 1.0f) * scale` cutoff cap, which is already written
in full-res px and scaled once).

**Members**: keep the base copy's shader/VAO set minus the two debug shaders and their
VAOs (`mLUTDebugShader`, `mStopMarkerShader`, `mLUTStripVertexArray`,
`mStopMarkerVertexArray`, `mLUTStripHalfSize` all move to `DebugRenderer`). Add
`mBlitShader` and `mScaledBuffer` (renamed from `mHalfResBuffer` - it is no longer
necessarily half). Keep one NDC quad VAO (`mFullVertexArray`) serving the emission,
fill and blit draws.

**One pass schedule for both paths.** Today the two forks order fill and gather
differently; the fill-first order is correct for both, because the scaled path's
gather goes to an independent FBO:

```
Render(vpW, vpH, time, config):
  if (!config.neon.enable) return;
  scale  = clamp(config.neon.resolutionScale, eps, 1.0)
  scaled = (scale < 1.0)
  targetFbo = Framebuffer::GetBoundId()      // before any pass retargets
  premultiplied-over blend
  Pass 2a  renderOpaqueFill()                 // full-res, on targetFbo, if opaqueMode != NONE
  if (config.neon.opaqueOnly) { restore blend; return; }   // preserved for BOTH paths
  packLightBlocks(config)
  blend off
  Pass 0   ok = renderEmissionPass(...)       // uNumSamples = clamped numSamples
  premultiplied-over blend
  Pass 1   if (ok) ok = renderNeonPass(mvp, scale, scaled ? &mScaledBuffer : nullptr)
  Pass 2b  if (scaled && ok) { BindId(targetFbo); glViewport(full); renderBlitPass(); }
  restore GL_SRC_ALPHA blend
```

`renderNeonPass` takes the scale and an optional target: when scaled it resizes +
binds + clears the FBO (with the existing `glGetFloatv(GL_COLOR_CLEAR_VALUE)`
save/restore and the "Resize failed -> bail, do not `Bind()` id 0" guard) and returns
false on allocation failure; when not scaled it draws straight onto the bound target.
Every pixel uniform is multiplied by `scale` unconditionally - a no-op at 1.0 - and
`uResolutionScale` is uploaded alongside.

Transform: derive `proj`/`center`/`mvp` once from `(vpW * scale, vpH * scale)` and
`center * scale`. At `scale == 1.0` that is exactly today's base transform, so the
fill (always full-res, `centerFull`) and the glow stay registered.

`OnConfigChanged`: keep the optimized copy's extra dirty deps, retargeted at the
merged fields - `neon.resolutionScale` and `neon.numSamples` dirty the sample walk and
the quad; `neon.gradientLutSize` reaches `bakeLUTs` through `GradientRingLUT`'s own
guard.

Log strings and `WarnOnOverflow` renderer names collapse back to `"NeonRenderer"`;
GL object debug labels drop the `NeonOpt.` prefix.

## Part 4 - `DebugRenderer` (new)

`lib/include/renderer/debug-renderer.h`, `lib/src/renderer/debug-renderer.cpp`.
Subclasses `BaseRenderer` like any other layer.

Moved in wholesale from `neon-renderer.cpp`: `renderGradientLUTStrip`,
`renderColorStopMarkers`, the strip-quad sizing block out of `setupGeometry`
(`stripHalfW = halfW * 0.6f`, `stripHalfH = min(halfH / 6, 20)`), the unit-quad setup
out of `Initialize`, and the two shader builds (`NEON_LUT_DEBUG_FRAG_SRC`,
`NEON_STOP_MARKER_FRAG_SRC`, both over `NEON_VERT_SRC`).

It owns **its own** `GradientRingLUT`, baked in `OnConfigChanged` from
`config.neon.colorStops / blendSpace / gradientLutSize / colorTransitionDuration` and
ticked in `Update` - the same inputs the neon renderer bakes from, so the previewed
ring and its cross-fade stay in step. Cost is one extra 256x1 RGBA8 texture; it is
what keeps `NeonRenderer` free of every debug member.

`Render` is the block lifted from the tail of `NeonRenderer::Render`: derive
`proj`/`center`/`halfW`/`halfH` at full res, strip unblended, markers straight-alpha,
restore `GL_SRC_ALPHA` on the way out. Early-out on `!config.debug.enable`, and on
neither show-flag being set.

Registered **after** `NeonRenderer` so the overlays land on top of the glow.
(Since moved further: the layer is registered **last**, above the droplets
and the flare as well, and holds the last `el_renderer_flags_e` bit.)

## Part 5 - C ABI (`lib/capi/`)

- `el-types.h`: add `EL_RENDERER_DEBUG = 1 << 6`. `EL_RENDERER_ALL` is already
  `0x7FFFFFFF` so it picks the new bit up. Leave `EL_RENDERER_NEON_OPTIMIZED` in place
  with a deprecation note - **do not renumber**, and add/adjust the `capi-internal.h`
  `static_assert` wall if any mirrored enum shifts.
- `el-effect.cpp` / `el_effect_init_with_renderers`: register `DebugRenderer` for
  `EL_RENDERER_DEBUG`; make `EL_RENDERER_NEON_OPTIMIZED` register the unified
  `NeonRenderer` **only if `EL_RENDERER_NEON` did not already** (guard both bits
  through one `if`, so a host passing `EL_RENDERER_ALL` gets one instance, not two).
- The 8 `el_effect_*_optimized_*` functions keep their exact signatures and map onto
  the merged fields:
  - `set_optimized_resolution_scale` / `num_samples` / `gradient_lut_size` -> the
    matching `config.neon.*` field (and the getters read them back).
  - `set_optimized_renderer_enabled(e, 1)` -> `neon.enable = true`, and
    `neon.resolutionScale = 0.5f` if it is currently 1.0 (restores the old default).
  - `set_optimized_renderer_enabled(e, 0)` -> `neon.resolutionScale = 1.0f`, leaving
    `neon.enable` untouched.
  - `get_optimized_renderer_enabled` -> `neon.enable && neon.resolutionScale < 1.0f`.
  Document this mapping in `el-effect.h` above the group.
- `el_effect_set/get_show_gradient_lut` and `..._show_color_stops`
  (`el-effect.cpp:96-120`): repoint from `config.neon.*` to `config.debug.*`.
  Signatures unchanged.
- Optional, only if you want the new flags reachable from C: `el_effect_set/get_neon_
  resolution_scale` etc. as properly-named aliases. Not required by anything in-tree.

## Part 6 - Demos

`demo/src/main.cpp`
- Drop the `NeonOptimizedRenderer` construction + `AddRenderer` (lines 110-111, 116);
  add `DebugRenderer`, registered immediately after `neonRenderer`.
- Hotkey at `main.cpp:369` toggling `optimizedNeon.enable` becomes a
  `resolutionScale` toggle between `1.0f` and `0.5f`.

`demo/src/debug-ui.{h,cpp}`
- Delete `buildOptimizedNeonSection` and the `BothNeonPathsWarning` helper
  (`debug-ui.cpp:311`) - both paths can no longer be on at once.
- The Neon section gains a "Performance" group: `Res Scale` (0.125-1.0),
  `Samples` (8-`NEON_MAX_LOOP_SAMPLES`), `LUT Size` (32-256), all on `cfg.neon.*`.
- New `buildDebugSection` holding the two checkboxes now on `cfg.debug.*` (moved from
  `debug-ui.cpp:599-600`), plus the `debug.enable` master toggle. `opaqueOnly` stays in
  the Neon section, so `OpaqueOnlyCheckbox` keeps one call site instead of two.
- `demo/src/ui-controls.h:138` reads `n.opaqueOnly` - unchanged.

`demo-capi/src/debug-ui.cpp` - the hand-maintained fork, same treatment through the C
ABI: `buildOptimizedNeonSection` folds into the neon section's perf group (the three
`el_effect_*_optimized_*` sliders still work as-is), `BothNeonPathsWarning` goes, and
the two show-flag checkboxes move to a Debug section. Its include path deliberately
cannot see `lib/include/`, so nothing else there needs touching.

## Part 7 - Docs

Must be updated (they state the fork as fact):
- `CLAUDE.md` - the renderer list ("Six renderers"), the `NeonOptimizedRenderer` bullet,
  and the "A change to neon has to land in both copies" warning, which this change
  retires.
- `docs/implementation.md`, `docs/emission-prepass.md` (pass tables + the "no such
  inversion" note, now that both paths share one schedule),
  `docs/architecture-design.md`, `docs/effect-reference.md` (the `optimizedNeon.*`
  parameter rows move under neon; new `debug.*` rows).
- `docs/neon-renderer-reference.html` - the uniform tables gain `uResolutionScale` /
  `uNumSamples` on the main shader; the overview/explained tiers get a lighter pass
  wherever they describe two neon renderers.

Leave alone as historical records, with a one-line "superseded by the unification"
note at the top: `docs/branch-vs-main-comparison.md`,
`docs/emission-prepass-comparison.md`. Sweep `docs/naming-review.md` and
`docs/review-findings.md` for entries the merge resolves.

---

## Verification

No test target exists, so verification is build + visual A/B.

**1. Baseline capture, before touching anything.** Build current HEAD, run the demo,
set a config that exercises the lot (rounded corners, several colour stops, 2+ arcs, a
segment boost, `opaqueMode` on), and save a capture PNG from the UI (it lands in
`res/` via `CaptureUtil::TimestampedPath`). Keep it.

```bash
cmake -S . -B build -G Ninja && cmake --build build && ./build/demo/edge-lighting-demo
```

**2. Pixel identity at `resolutionScale = 1.0`.** After the change, rebuild, dial in the
same config, capture again, and compare:

```bash
cmp baseline.png after.png
```

These must be byte-identical, not merely similar - the unified shader at scale 1.0 and
128 samples is arithmetically the same program as today's `neon.frag`. Any difference
is a real regression in the merge, most likely a uniform that lost or gained a `* scale`.

**3. Scaled path.** Set `resolutionScale = 0.5`, `numSamples = 64` and compare against a
capture from today's `NeonOptimizedRenderer` at the same settings - same test, same
expectation of identity.

**4. Both paths' guards.** Toggle `opaqueOnly` at scale 1.0 and 0.5 (silhouette only,
no glow, no overlays); drag `resolutionScale` across 1.0 in both directions and confirm
no FBO churn or flicker at the boundary.

**5. Debug overlays.** Tick "Show Gradient LUT" and "Show Color Stops": strip and
markers must render exactly as before, over the glow, with the marker discs still
landing on their stop positions. Animate the stops and confirm the strip's cross-fade
still tracks the glow's (this is the check that the `DebugRenderer`'s own
`GradientRingLUT` stays in step). Untick `debug.enable` and confirm both vanish.

**6. C ABI.** `./build/demo-capi/edge-lighting-capi-demo` must build and behave the
same - in particular the old "Optimized Neon" enable checkbox should now move the
resolution scale, and the two show-flag checkboxes must drive the new `DebugRenderer`.

**7. Frame capture path.** Trigger the offscreen capture with the scaled path active -
this is the case where `Framebuffer::GetBoundId()` matters. The PNG must contain the
effect, not an empty frame.
