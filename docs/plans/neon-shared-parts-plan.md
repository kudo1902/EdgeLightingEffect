# Plan: the two parts as the neon's shared product

Status: PROPOSED, 2026-10-18. Nothing here is built yet. Written as a brief
another agent can execute step by step without the conversation that produced
it: every step names its files, its symbols, what to change, what can go
wrong, how to check it, and its commit.

**The owner's decision behind it (2026-10-18):** the readers of the neon's
light can run our GLSL. So a reader samples the two textures the screen is
drawn from - the glow (`NeonRenderer.Glow`) and the ring atlas
(`NeonRenderer.Ring`) - through `neonLight`, and needs no flat texture made
for it.

**What the plan does:**

1. Makes the **two parts the product**:
   - findable without a pointer to the neon;
   - versioned;
   - read with one declaration and one upload;
   - able to build a flat texture in a reader's own pass.
2. **Removes the flat shared buffer** (`NeonRenderer.Composite`,
   `NeonConfig::compositeScale`) from the neon. What it computed stays as a
   GLSL function any reader can call.

Background, in this order:
- [`neon-shared-buffer-explained.md`](../guides/neon-shared-buffer-explained.md): how
  it works today, in plain words;
- [`neon-shared-buffer.md`](../guides/neon-shared-buffer.md) section 4: every rule the
  renderer keeps today;
- [`neon-shared-layer-perf-review.md`](../history/neon-shared-layer-perf-review.md): the
  costs.

![Planned: phase 1 makes the glow and the ring atlas, both PUBLIC; the neon publishes a description of its light (NeonLight) found through the effect or the context; phase 2 draws the screen from the two textures; readers - a host shader, a library layer, and a reader that wants a flat texture - each read them in passes of their own.](../images/neon-shared-buffer/parts-core.svg)

---

## 0. Before you start

### 0.1 Ground rules

- **Branch.** Work on `design_system_sharing_resources_between_renderers`, or
  a branch from it. Nothing in this plan is on `main`: `main` has no
  `compositeScale`, no `AddHostRead` and no PUBLIC keys, so removing the flat
  buffer breaks no host.
- **One commit per step**, made only after that step's checks pass. Do not
  push. End each commit message with the attribution line your harness gives.
- **Read [`CLAUDE.md`](../../CLAUDE.md) and `AGENTS.md` first.** The rules this
  plan touches most:
  - no em-dash anywhere; shader sources ASCII only;
  - brace every single-statement body, `case` bodies included;
  - `typedef struct X { ... } X;` for every struct;
  - members `mFoo`; private methods `camelCase`; constants `ALL_CAPS`;
  - **one program object per pass target and blend state** (the AB rule: one
    program drawing an offscreen buffer unblended and the screen blended
    measured 93-624x slower on the AMD driver);
  - a new shader or `.glsl` chunk is registered in `lib/CMakeLists.txt` (both
    the `CMAKE_CONFIGURE_DEPENDS` and the `file(READ ...)` lists) and in
    `lib/shaders/shaders.h.in`; never write an `@NAME@` placeholder inside a
    comment of `shaders.h.in`;
  - the neon's header groups methods and members by role, and the `.cpp`
    defines methods in the header's order.
- **The neon's invariants this plan must keep** are listed in
  [`neon-shared-buffer.md`](../guides/neon-shared-buffer.md) section 4.6. Read it
  before step 3.

### 0.2 Build and the gates

```bash
cmake -S . -B build -G Ninja -DEDGE_LIGHTING_BUILD_TOOLS=ON
cmake --build build
```

| gate | command | passes when |
| ---- | ------- | ----------- |
| scenes | `./build/tools/neon-scale-check/neon-scale-check check` | prints `PASS` |
| tiling | `./build/tools/neon-scale-check/neon-scale-check partition` | `0 px overlapping, 0 px of seam gap`, the ring copied from the atlas in some configs, `PASS` |
| parts | `./build/tools/neon-scale-check/neon-scale-check parts` | `PASS (max 1, ...)` |
| flat buffer | `./build/tools/neon-scale-check/neon-scale-check shared` | `PASS (max 2, ...)`; worst max 1 today (removed in step 3) |
| pixels | `./build/tools/layer-check/layer-check report --out FILE` then `layer-check diff BEFORE AFTER` | `0 differ` (or only the frames the step says) |
| compiles | `./build/tools/layer-check/layer-check startup` | the compile and link counts the step says |

Compare `layer-check` reports only within one GPU and one build type
(`Release`, the default).

### 0.3 Step 0 - the baseline, and a reader option for `time`

**Goal:** what every later step is compared against, and a way to time a
reader without a scratch patch.

1. In `tools/neon-scale-check`, give `time` a repeatable `--read KEY` option.
   - `main.cpp`, `Time()`: parse `--read KEY` into a `std::vector<std::string>`.
   - `harness.h` / `harness.cpp`: give `TimeRender` and `TimeGPU` a
     `const std::vector<std::string> &hostReads` parameter (defaulted
     empty). Each calls `effect.AddHostRead(key)` for each key, after
     `CreateEffect`, before the first `SetConfig`.
   - Document it in `tools/neon-scale-check/README.md` and in `CLAUDE.md`'s
     `neon-scale-check time` sentence.
2. Save the baselines in a scratch directory (`$S` below), not in the repo:

   ```bash
   ./build/tools/layer-check/layer-check report --out $S/lc-base.txt
   ./build/tools/neon-scale-check/neon-scale-check shared > $S/shared-base.txt
   ./build/tools/neon-scale-check/neon-scale-check parts  > $S/parts-base.txt
   ./build/tools/layer-check/layer-check startup 2>/dev/null | grep -v "^\[Thread" > $S/startup-base.txt
   for m in still hue intensity arc-wipe segment-travel; do
     ./build/tools/neon-scale-check/neon-scale-check time $S/t-base-none-$m.json --scales 1,0.5 --mode $m --size 1920x1080
     ./build/tools/neon-scale-check/neon-scale-check time $S/t-base-ring-$m.json --scales 1,0.5 --mode $m --size 1920x1080 --read NeonRenderer.Ring
   done
   ```

**Checks:** builds; `time` with and without `--read` runs. The tool change
moves no pixel, so `layer-check` is identical by construction.

**Commit:** `neon-scale-check time: --read KEY declares a host read`.

---

## 1. Why

What it costs today on an Apple M2 Pro at 1080p, on a frame where something
moves:

| reader | what the neon does for it | cost |
| ------ | ------------------------- | ---- |
| none | nothing | 0 |
| of the two parts | the ring is shaded into its atlas instead of onto the screen, which copies it: one more render pass | +0.016-0.033 ms |
| of the flat shared buffer | the same, plus pass 1s: clear, fill, assemble, store | +0.04-0.19 ms |

A reader of the parts makes the neon draw nothing twice. On still frames it
costs nothing: the atlas exists anyway. `NeonLight`, the description of the
light, is CPU data, not a draw.

The flat buffer adds, on top of the parts:
- a config field, with its floor and clamp;
- a PUBLIC record with its contract;
- six functions, three types and two programs in `NeonRenderer`;
- two decision rules coupled to the ring atlas;
- a region with its own rounding and edge rule;
- a release timer, a demo slider, a gate, and eleven `layer-check` scenes.

Readers that run our GLSL need none of it.

---

## 2. Today, in the code

| piece | where |
| ----- | ----- |
| the glow | `mScaledBuffer`, registered twice: `NeonRenderer.Scaled` (DEBUG) and `GLOW_KEY` = `NeonRenderer.Glow` (PUBLIC) - `NeonRenderer::NeonRenderer()` in `lib/src/renderer/neon-renderer.cpp` |
| the ring atlas | `RingAtlas mRingAtlas`, `RING_KEY` = `NeonRenderer.Ring`; pass 1t `renderRingAtlasPass`; the copy `renderRingCopyPass` |
| the description | `NeonRenderer::SharedLight` (`lib/include/renderer/neon-renderer.h`), filled in `RenderOffscreen` as a local `SharedLight light`, stored in `mSharedLight`, returned by `GetSharedLight()`; uploaded by the static `UploadSharedLight(shader, light, glowUnit, ringUnit)` |
| the edge mask | `NeonRenderer::EdgeMask`, `GetEdgeMask(config, viewportHeight)`, `UploadEdgeMask(shader, mask)` |
| the reader's GLSL | `lib/shaders/neon-light.glsl` (`neonLight`, `neonLightClampPx`, `neonLightSquareTouchesRing`), `lib/shaders/neon-ring.glsl` (`neonRingTexel`); exported whole as `ShaderSource::NEON_LIGHT_GLSL` (`shaders.h.in`) |
| demand | `IsBufferWanted(mBufferRecords.ringAtlas)` in `RenderOffscreen`; a host declares with `EdgeLightingEffect::AddHostRead` |
| the flat buffer | pass 1s `renderCompositePass`; `neon-assemble.frag`; inventory in section 7 |
| gates | `tools/neon-scale-check/src/parts.cpp` (`Parts()`, `Shared()`), `tools/layer-check/src/main.cpp` (the `shared/` scenes) |

---

## 3. The target API

After step 2, a reader writes:

```cpp
#include "core/neon-light.h"
#include "renderer/neon-renderer.h"     // the keys, UploadLight

// Once: this host reads the neon's light on every frame.
effect.AddHostRead(NeonRenderer::GLOW_KEY);
effect.AddHostRead(NeonRenderer::RING_KEY);

// Each frame, after effect.Render(w, h):
const NeonLight *light = effect.GetNeonLight();
if (light && light->valid)
{
    if (light->version != myCachedVersion)
    {
        // redo what depends on the light
    }
    myShader.Use();
    NeonRenderer::UploadLight(myShader, *light, /*glowUnit*/ 0, /*ringUnit*/ 1);
    // ... draw
}
```

A library layer declares the same with
`NeonRenderer::AddLightReads(out, this, ReadPhase::RENDER)` in its
`CollectReads`, and finds the light with `GetResourceContext()->GetNeonLight()`.

```glsl
// After the version line: ShaderSource::NEON_LIGHT_GLSL, then
vec4 c = neonLight(gl_FragCoord.xy, 1.0);                   // per pixel, full resolution
vec4 t = neonLightOverSquare(floor(gl_FragCoord.xy) * (1.0 / s), s); // one texel of a flat texture at scale s
```

---

## 4. Steps

### Step 1 - the averaging as a public function (no pixel moves)

**Goal:** `neonLightOverSquare` in `neon-light.glsl`, so any reader can build
a flat texture; pass 1s calls it.

**Files:** `lib/shaders/neon-light.glsl`, `lib/shaders/neon-assemble.frag`.

**Change:**

1. Add to `neon-light.glsl`, after `neonLight`:

   ```glsl
   // The screen's light averaged over the square of caller px [lo, lo + 1 / scale]
   // (y up): every full-resolution pixel the square touches, weighted by how
   // much of it the square covers - a box filter of the screen, for one texel
   // of a flat texture at `scale` texels per caller px, scale in (0, 1]. At 1
   // the square must be pixel-aligned (any grid anchored at a pixel corner).
   vec4 neonLightOverSquare(vec2 lo, float scale)
   ```

   Its body is `neon-assemble.frag`'s `main()` today, moved, with `lo` and
   `scale` as parameters in place of `uAssembleOrigin + floor(gl_FragCoord.xy) * footprint`
   and `uAssembleScale`:
   - the `scale >= 1.0` exit: `return neonLight(lo + 0.5, 1.0);`
   - `footprint = 1.0 / scale; hi = lo + vec2(footprint);`
   - the glow-only shortcut, with the clamped corners `clo` / `chi` and the
     grown test, as today;
   - the loop, returning `sum * (scale * scale)`.

   **Keep every expression exactly as it is today.** Write `1.0 / scale` and
   multiply by `scale * scale`; do not invert a footprint. The output must be
   bit for bit what it is: `1 / (1 / s)` is not always `s` in float.
2. `neon-assemble.frag`'s `main()` becomes:

   ```glsl
   void main() {
       vec2 lo = uAssembleOrigin + floor(gl_FragCoord.xy) * (1.0 / uAssembleScale);
       fragColor = neonLightOverSquare(lo, uAssembleScale);
   }
   ```

   At scale 1 this `lo` equals today's `uAssembleOrigin + floor(gl_FragCoord.xy)`
   exactly: `1.0 / 1.0` is exactly 1.
3. Move the comments with the code. The helper's comment carries the
   shortcuts' reasons and the "hairline read 2 without the margin" warning.
   `neon-assemble.frag` keeps one line pointing at it.

**Pitfalls:**
- `neonLightOverSquare` reads only `uLight*` uniforms and chunks
  `neon-light.glsl` already depends on, so `shaders.h.in` needs no change.
  Check that `NEON_LIGHT_GLSL` still compiles standalone: `parts` builds its
  probe from it.
- An early `return` inside a function is fine; keep the `scale >= 1.0` test
  first so the scale-1 path stays loop-free.

**Checks:**
- `shared` output is identical to `$S/shared-base.txt`
  (`diff <(neon-scale-check shared) $S/shared-base.txt` prints nothing).
- `layer-check diff $S/lc-base.txt NEW` shows `0 differ`.
- `parts`, `check` and `partition` pass.

**Commit:** `neonLightOverSquare: the flat texture's averaging as a reader function`.

### Step 2 - the description as a product (no pixel moves)

**Goal:** a reader finds the light through the effect or the context, knows
when it changed, and declares it in one call.

**Files:**
- new: `lib/include/core/neon-light.h`;
- `lib/include/core/resource-context.h`, `lib/src/core/resource-context.cpp`;
- `lib/include/core/edge-lighting.h`, `lib/src/core/edge-lighting.cpp`;
- `lib/include/renderer/base-renderer.h`, `lib/src/renderer/base-renderer.cpp`;
- `lib/include/renderer/neon-renderer.h`, `lib/src/renderer/neon-renderer.cpp`;
- `lib/shaders/neon-light.glsl` (comments only);
- `tools/neon-scale-check/src/parts.cpp`.

**Change:**

1. **The types move to core.** In `lib/include/core/neon-light.h`, which
   includes only `<glm/glm.hpp>` and `<cstdint>`:
   - `typedef struct NeonEdgeMask { ... } NeonEdgeMask;` - today's
     `NeonRenderer::EdgeMask`, the same fields in the same order;
   - `typedef struct NeonLight { ... } NeonLight;` - today's
     `NeonRenderer::SharedLight`, plus:
     - `uint64_t version = 0;` - the larger of the two records' versions
       (below);
     - `bool readMissing = false;` - true when the light is invalid only
       because nothing declared a read of the Ring on a frame something
       moved.

   Core may name neon types: `core/config.h` already holds `NeonConfig`. In
   `NeonRenderer`, `typedef NeonEdgeMask EdgeMask;` keeps every existing use
   of `EdgeMask` compiling; `SharedLight` is renamed, not aliased.
2. **Renames in `NeonRenderer`:**

   | from | to |
   | ---- | -- |
   | `GetSharedLight()` | `GetLight()` |
   | `UploadSharedLight` | `UploadLight` (static, same parameters) |
   | `mSharedLight` | `mLight` |

   Update every use: `neon-renderer.cpp`, `parts.cpp`, `neon-light.glsl`'s
   comments.
3. **The context holds the light.** In `ResourceContext`:

   ```cpp
   /// The neon's light (core/neon-light.h), published by the one renderer that
   /// describes it in this context; nullptr when none has.
   const NeonLight *GetNeonLight() const { return mNeonLight; }
   /// Called by the producer, each frame it fills @p light. Refused - logged
   /// once - while another producer holds the slot.
   bool PublishNeonLight(const BaseRenderer *producer, const NeonLight *light);
   /// Empties every slot @p producer holds (called by
   /// BaseRenderer::unregisterOwned).
   void WithdrawProducts(const BaseRenderer *producer);
   ```

   The members are `const BaseRenderer *mNeonLightProducer = nullptr;`,
   `const NeonLight *mNeonLight = nullptr;` and a "refusal logged" flag.
4. **No dangling pointer.** `BaseRenderer::unregisterOwned()` ends with
   `if (mResourceContext) { mResourceContext->WithdrawProducts(this); }`. It
   already runs on destruction and when a renderer moves to another context
   (`SetResourceContext`).
5. **The neon publishes.** At the end of `RenderOffscreen`, after
   `mLight = light;`, call
   `if (GetResourceContext()) { GetResourceContext()->PublishNeonLight(this, &mLight); }`.
   `BaseRenderer` has no context accessor today, only `GetBufferRegistry()`.
   Add a protected `ResourceContext *GetResourceContext() const` (it returns
   `mResourceContext.get()`); a reading layer uses it too.
6. **The version.** Add to `BaseRenderer` a protected
   `uint64_t GetBufferVersion(BufferHandle handle) const`: the record's
   `version` through its slot, or 0 with no context. In `RenderOffscreen`,
   when `light.valid`, set
   `light.version = std::max(GetBufferVersion(mBufferRecords.glow), GetBufferVersion(mBufferRecords.ringAtlas));`
   AFTER `publishBuffers`, which bumps the versions. Versions come from one
   registry-wide counter, so the maximum moves exactly when either part's
   bytes changed.
7. **readMissing.** `light.readMissing = !light.valid && glowReady && ringAtlasPossible && !ringAtlasNeeded;`
   using `RenderOffscreen`'s own locals.
8. **The effect.** `const NeonLight *EdgeLightingEffect::GetNeonLight() const`
   returns `mResourceContext->GetNeonLight()`. When that light has
   `readMissing`, it logs once per effect (a `mutable bool`):

   ```
   NeonLight read without declaring NeonRenderer.Ring: invalid on moving frames
   ```
9. **One declaration for a layer:**
   `static void NeonRenderer::AddLightReads(ReadList &out, const BaseRenderer *reader, ReadPhase phase)`
   adds `GLOW_KEY` and `RING_KEY`.
10. **The tools.** `parts.cpp` reads `effect.GetNeonLight()` instead of
    `FindNeon(effect)->GetSharedLight()`; drop `FindNeon` if nothing else
    uses it.

**Pitfalls:**
- `NeonEdgeMask` keeps `rectCenter` first, as `EdgeMask` has it.
  `UploadEdgeMask` and the blit read it.
- The context slot holds a POINTER to the neon's member. Publish it after
  the member is filled, and never publish a local.
- Two neons in one effect: the second's publish is refused and logged once,
  as its keys already are.
- Header order: the new methods go where their role is in `neon-renderer.h`,
  and in the same place in the `.cpp`.

**Checks:**
- `layer-check diff $S/lc-base.txt NEW`: `0 differ` (no pixel moves).
- `parts` and `shared` pass, identical to step 1's output.
- A quick probe, kept in `parts`: one scene, still, the light's `version`
  equal on two frames in a row, and larger after a config change.
- Another: with no Ring read declared under a rotating hue,
  `GetNeonLight()->readMissing` is true.

**Commit:** `NeonLight: the neon's light as a product - found, versioned, declared once`.

### Step 3 - the flat buffer leaves the neon

**Goal:** option B of section 5: delete `NeonRenderer.Composite` and
everything that serves it. Readers that want a flat texture call
`neonLightOverSquare` in their own pass.

**Before you start:** the owner must have answered open question 6.1 (the
fill). If it is unanswered, assume **no fill part**: readers draw their own
fill if they want one. Note that assumption in the commit message.

**Change:** delete everything in the inventory (section 7), then:

1. **The ring atlas's need** in `RenderOffscreen` becomes
   `ringAtlasNeeded = ringAtlasPossible && (reuseOffscreen || ringAtlasWanted);`.
   Delete `compositeWanted`, `compositeReused`, `compositeDraw`,
   `compositeReady`, `compositeRegion` and their uses. That includes the
   `|| compositeDraw` in the two `prevTarget` capture / restore conditions,
   and `mComposite.wanted = false;` at the top.
2. **`OnConfigChanged`:** delete the `onlyCompositeScale` test
   (`OnlyCompositeScaleMoved`). The reuse condition becomes plain
   `config.neon != mCurrentConfig.neon || config.geometry != mCurrentConfig.geometry`.
   Delete the shared buffer's release block and the `setupCompositeGeometry`
   call.
3. **`setupRingGeometry`:** delete the `mBlitBounds` assignment and the
   `setupCompositeGeometry(config)` call. **`setupFillGeometry`:** delete both
   `mFillOuter` assignments.
4. **`drawFill`** loses its reason for taking a program and an `ensure`
   member pointer. Optionally fold it back into `renderFillPass`. Keep the
   change minimal if unsure: pixels must not move.
5. **The gate.** Rewrite `Shared()` in `parts.cpp` as the second half of
   `Parts()`, or keep the `shared` command name for it. The tool builds the
   flat texture itself:
   - a program from `PROBE_VERT_SRC` and
     `"#version 330 core\n" + NEON_LIGHT_GLSL + main`, whose `main` is
     `fragColor = neonLightOverSquare(floor(gl_FragCoord.xy) * (1.0 / uScale), uScale);`;
   - drawn unblended over transparent black into an `OffscreenCapture` of
     `floor(FRAME_WIDTH * s) x floor(FRAME_HEIGHT * s)`, after
     `UploadLight(probe, *effect.GetNeonLight(), 0, 1)`;
   - judged against `ScreenOverSquare(screen, q / s, 1 / s)` with the same
     bounds as today (max 2, 99.9% of lit texels within 1), at 1, 0.5 and
     0.4, over every gate scene and motion.

   Drop the fill cases (`1+fill`, `1+all`): the parts carry no fill. Delete
   `effect.AddHostRead(COMPOSITE_KEY)`; declare `GLOW_KEY` and `RING_KEY`.
6. **`layer-check`** (`tools/layer-check/src/main.cpp`):
   - Delete the `shared/` scenes, `addShared`, `Motion::SHARED_STEP` and its
     `FrameConfig` case, and the `published-in` block.
   - Add `parts/` scenes with `hostReads = {"NeonRenderer.Glow", "NeonRenderer.Ring"}`:
     - `parts/default@0.5+read` (`STILL`);
     - `parts/default@0.5+read+hue` (`HUE`);
     - `parts/default@0.5+read+intensity` (`INTENSITY`);
     - `parts/edge@0.5+read` (`STILL`; the default scene moved across the
       frame's top-right corner, `geometry.position` as in `parts.cpp`'s
       `Placement::EDGE`);
     - `parts/default@0.5+read+view` (`STILL`,
       `debug.bufferView = "NeonRenderer.Ring"`), so the atlas's own texels
       reach a hash.
   - Update `tools/layer-check/README.md`: scene list and frame count.
7. **`PassRecorder`** (`tools/common/pass-recorder.{h,cpp}`):
   - delete `PassKind::P1S`, its name case, and the `uLightRing` branch;
   - the `uOpaqueMode` branch returns `P2A` again, unconditionally.
8. **The demo:**
   - `demo/src/debug-ui.cpp`: delete the "Shared Scale" slider block, its
     tooltip and the "Drawn at" line;
   - `demo/src/ui-controls.h`: delete the "Shared buffer" status line.

   `demo-capi` never had them.
9. **`edge-lighting.h`:** `AddHostRead`'s comment names `COMPOSITE_KEY` as
   its example; name `RING_KEY` instead.

**Pitfalls:**
- **Pixels.** With no reader, no neon frame may change. Every `neon/`,
  `all@`, `spotlight*` and `debug*` frame in `layer-check` stays identical to
  `$S/lc-base.txt`; only the removed `shared/` frames go and the new `parts/`
  frames appear.
- **The ring atlas's rule.** Do not drop `reuseOffscreen` from
  `ringAtlasNeeded`. Still frames copy the atlas whether or not anything
  reads it.
- **`RING_ATLAS_RELEASE_SECONDS`** is defined as `COMPOSITE_RELEASE_SECONDS`
  today. Give it its own value, 5.0f, when the latter goes.
- **`neon-light.glsl` stays.** Only `neon-assemble.frag` goes. `neonLightClampPx`
  stays too: readers' squares can cross the viewport.
- **`GLOW_KEY`'s contract** (beside the key in `neon-renderer.h`) mentions
  the shared buffer in passing. Reword it, and keep its substance.
- `config.h`'s `operator==` lists `compositeScale`: delete that line with the
  field, or the comparison stops compiling.

**Checks:**
- `check`, `partition` and `parts` (with the new flat-texture half) pass.
- `layer-check diff $S/lc-base.txt NEW`:
  - every scene present in both is identical;
  - the `shared/` scenes are missing and the `parts/` scenes are new - list
    them in the commit message.
- `layer-check startup` on the default config: the same compiles and links
  as `$S/startup-base.txt`, since the flat buffer's programs were never
  built without a reader.
- `neon-scale-check time` with no read and with `--read NeonRenderer.Ring`,
  every mode, against `$S/t-base-*`: no mode's median more than 3% slower.
  Re-run any that is, interleaved, before calling it a regression.

**Commit:** `Remove the flat shared buffer: readers build one from the parts`.

### Step 4 - the docs

**Goal:** the docs describe the parts as the product, and the flat buffer
only as history.

**Files and what changes:**

| file | change |
| ---- | ------ |
| `docs/guides/neon-shared-buffer.md` | Rename to `docs/neon-shared-light.md` (update every link with `grep -rl neon-shared-buffer.md`). Sections: the parts; reading them with `NeonLight` / `GetNeonLight` / `UploadLight` / `AddLightReads`; `neonLight` and `neonLightOverSquare`; a flat texture in a reader's pass (code sample); section 4 (the renderer's decisions) without pass 1s and its rules; the code map; limits. Redraw `decisions.svg` without the shared-buffer boxes (`docs/images/neon-shared-buffer/diagrams.py`, `decisions()`). |
| `docs/guides/neon-shared-buffer-explained.md` | Rename to `docs/neon-shared-light-explained.md`. The middle of the story is the light and its readers; pass 1s becomes "a reader's own pass"; redraw `components.svg` (`components()` in `diagrams.py`) with the shared buffer box replaced by readers. Example 4.1/4.2 tables lose the 1s column. |
| `docs/guides/architecture.md` | 4.2 PUBLIC list (two products: Glow, Ring); 4.3 demand (the ring atlas only); 4.5 rewritten around `NeonLight`, the context slot, `GetNeonLight`; section 6 (the `GetSharedLight` exception paragraph goes - the light is now found through the context); invariants and gates tables (drop `shared` rows); `channels.svg` (`channels()` in `docs/images/architecture/diagrams.py`): remove `Composite`, reader box text |
| `docs/guides/implementation.md` | the `CollectReads` row and the neon paragraph |
| `docs/guides/neon-onboarding-guide.md` | 5.1 tables (drop `mCompositeFillShader`, `mAssembleShader`, `mComposite.lightMesh`, `mComposite.buffer`), 5.5 schedule (drop the composite lines), Part 6 list and the P1s card |
| `docs/reference/config-reference.md` | drop the `compositeScale` row and the `NeonRenderer.Composite` buffer row |
| `docs/reference/effect-reference.md` | drop the `neon.compositeScale` entry; add a short "reading the neon's light" pointer |
| `CLAUDE.md` | the docs list entries for the renamed docs and this plan (mark it BUILT); the shared-buffer paragraph becomes a "the neon's light is a product" paragraph; the `@NEON_LIGHT@` sentence gains `neonLightOverSquare`; the gates sentence (`shared` / `parts`); the layer-check scene count |
| `docs/history/progress-log.md` | one entry per step, newest first: what changed, what was checked, the numbers |
| `docs/history/neon-shared-layer-perf-review.md` | one line at the top: "the flat buffer described here was removed on DATE; see neon-shared-parts-plan.md" - the body stays as the record |
| `tools/neon-scale-check/README.md` | `parts` (and `shared`, if kept) as rebuilt in step 3; `time --read` |

**Checks:**
- every relative link in the changed docs resolves;
- no em-dash anywhere (`grep -rnP "\x{2014}" docs CLAUDE.md` prints nothing);
- the diagrams regenerate (`python3 docs/images/neon-shared-buffer/diagrams.py`,
  `python3 docs/images/architecture/diagrams.py`) and the SVGs are
  committed with the scripts.

**Commit:** `Docs: the neon's light as a product; the flat buffer as history`.

### Step 5 - optional, later: the C ABI

Only if a C host needs the parts. Read `lib/capi/` and `capi-internal.h`
first, and keep to the rules there: new exports only, parameterised, nothing
re-signed, no C++ exception across the boundary.

- `EL_API const char *el_neon_light_glsl(void);` returns
  `ShaderSource::NEON_LIGHT_GLSL`.
- `EL_API el_result_e el_effect_get_neon_light(el_effect_t effect, el_neon_light_t *out);`
  where `el_neon_light_t` mirrors `NeonLight` in plain C (textures as
  `uint32_t`, vectors as `float[2]` / `float[4]`, `version` as `uint64_t`).
  Add a `static_assert` on any mirrored enum.
- `EL_API el_result_e el_neon_light_upload(uint32_t program, const el_neon_light_t *light, int glowUnit, int ringUnit);`
  with raw `glGetUniformLocation` / `glUniform*` by the names
  `neon-light.glsl` declares, because `ShaderProgram` is not in the C
  surface.
- `el_effect_add_host_read` if `AddHostRead` is not exported yet.
- Check `demo-capi` still builds; add a C-side probe if one exists for other
  exports.

### Step 6 - optional: a reader in the demo

A toggle in `demo/src/debug-ui.cpp` that turns on a small host-side layer,
drawn after `effect.Render`, which draws a reflection of the neon under the
rect from `neonLight`. That gives the parts a real, host-shaped user and a
manual check of:
- `valid` on still and moving frames;
- `version` holding on still frames;
- the one-time log when the read is not declared.

---

## 5. The flat buffer: the options

| option | what | cost | verdict |
| ------ | ---- | ---- | ------- |
| **A. Keep** | the status quo: made by the neon while read | everything in section 7, for no consumer | no |
| **B. Remove, keep the function** | delete it from the neon; its averaging stays as `neonLightOverSquare`; a reader that wants a flat texture draws it in its own pass | the reader pays what pass 1s cost (one render pass); the neon pays nothing | **recommended - steps 1 and 3** |
| **C. Move out** | B, plus a small class outside the neon that builds the flat texture from the public API for any host that wants one | B plus a class to keep, with no consumer | later, if a consumer appears: B plus ~100 lines |

---

## 6. Open questions and risks

1. **The fill - owner's decision, before step 3.** The parts carry no fill;
   the flat buffer did. Do readers need it?
   - If yes, add a third, textureless part: `neonFill(p)`, the fill's
     analytic edge from `black-rect.frag`, as a chunk with its own upload
     (`UploadFill`) - no buffer, no pass. Insert it as a step 2b.
   - Unanswered, step 3 assumes no.
2. **The quiet failure.** A reader that forgets the Ring read gets `valid`
   only on still frames. Step 2's one-time log and `readMissing` make it
   loud. A reader that declares in some frames only still gets `valid` false
   on its undeclared moving frames; that is by design, and documented.
3. **Tile-based GPUs.** The one cost left for a reader is the ring atlas's
   render pass on moving frames: ~0.03 ms on the M2, unmeasured on Mali or
   Tizen. Measure it there before promising it.
4. **Two readers wanting the same flat texture** each pay for their own pass
   under B. If that case appears, option C shares one.
5. **History.** The flat buffer's perf review and the progress log stay as
   the record, the review with a pointer line (step 4).

---

## 7. Appendix: what step 3 removes

| symbol | file | note |
| ------ | ---- | ---- |
| `NeonConfig::compositeScale`, its `operator==` line | `lib/include/core/config.h` | and its doc comment |
| `NeonConfig::COMPOSITE_MIN_SCALE` | `lib/include/core/config.h` | |
| `NeonRenderer::COMPOSITE_KEY` and its contract comment | `lib/include/renderer/neon-renderer.h` | |
| `NeonRenderer::GetCompositeScale` | `neon-renderer.h`, `neon-renderer.cpp` | also used by `demo/src/debug-ui.cpp`, `demo/src/ui-controls.h` |
| `OnlyCompositeScaleMoved` | `neon-renderer.cpp` (anonymous namespace) | |
| `COMPOSITE_RELEASE_SECONDS` | `neon-renderer.cpp` | give `RING_ATLAS_RELEASE_SECONDS` its own `5.0f` |
| `CompositeRegion`, `CompositeCache`, `CompositeTarget`, `mComposite` | `neon-renderer.h` | |
| `mAssembleShader`, `mCompositeFillShader`, `PROGRAM_ASSEMBLE`, `PROGRAM_COMPOSITE_FILL` | `neon-renderer.h` | renumber nothing else; the bits are only masks |
| `ensureCompositePrograms`, `setupCompositeGeometry`, `getCompositeRegion`, `renderCompositePass` | `neon-renderer.h`, `neon-renderer.cpp` | |
| `mBlitBounds`, `mFillOuter` | `neon-renderer.h`, `neon-renderer.cpp` | read only by the removed functions |
| `BufferRecords::composite`, `BufferUse::compositeRead` / `compositeWritten`, the composite mark in `publishBuffers` | `neon-renderer.h`, `neon-renderer.cpp` | |
| the composite release in `Update` and in `OnConfigChanged` | `neon-renderer.cpp` | |
| the registration of `mComposite.buffer` in the constructor, and its vertex format in `Initialize` (`mComposite.lightMesh.vertexArray.SetAttribPointer`) | `neon-renderer.cpp` | |
| `neon-assemble.frag`; `NEON_ASSEMBLE_FRAG_SRC` | `lib/shaders/`, `shaders.h.in`, `lib/CMakeLists.txt` (both lists) | |
| `Shared()` as it is (rebuilt, step 3.5) | `tools/neon-scale-check/src/parts.cpp` | and its README section |
| the `shared/` scenes, `Motion::SHARED_STEP`, `published-in` | `tools/layer-check/src/main.cpp` | |
| `PassKind::P1S` | `tools/common/pass-recorder.{h,cpp}` | |
| the Shared Scale slider; the "Shared buffer" status line | `demo/src/debug-ui.cpp`, `demo/src/ui-controls.h` | |
