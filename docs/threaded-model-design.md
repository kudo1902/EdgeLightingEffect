# Threaded model: data thread + render thread

Plan for splitting the effect into a data side and a render side so a C# host
can mutate configuration, animations and the clock on one thread while the GL
context is driven from another.

Status: design only. Nothing in this document is implemented yet.

## 1. What this is for, and what it is not for

The decision that shapes everything else: **the library spawns no threads.**
The host owns both. Native gains a thread-safe seam, not a scheduler.

| | |
| --- | --- |
| Goal | A C# host may call setters, animation control and the clock from any thread while `el_effect_render` runs on the GL thread, with no races and no half-applied frames. |
| Data side owns | staging config, base config, `Clock`, `AnimationManager`, config composition. |
| Render side owns | every renderer, every LUT, every GL object, every bake. |
| Threads created by the library | none. |
| Host framework | irrelevant. The contract is a per-function thread table, nothing more. |

Explicitly **not** goals:

- **Moving CPU bakes off the GL thread.** `docs/neon-perf-review.md` section 4
  measures the whole per-frame config path - compose plus animate, at the
  segment and arc caps - at **0.0002 ms/frame with 0 allocations**. There is no
  frame time to reclaim here. The LUT bakes, the loop-sample walk and the
  geometry rebuild all end in a GL upload, so moving them would mean splitting
  every `BaseLUT` and geometry builder into a bake half and an upload half to
  win back a fraction of a microsecond. Not worth it. They stay on the render
  thread inside `OnConfigChanged`.
- A library-owned render loop or context handoff.
- Any change to renderer internals. No renderer is touched by this plan.

## 2. The coupling that is in the way

One fact blocks everything:

```
el_effect_update -> EdgeLightingEffect::Update
                      -> refreshActiveConfig() -> renderer->OnConfigChanged()  // GL uploads
                      -> renderer->Update()                                    // GL upload (fade)
```

`Update` is a GL call today. `GradientRingLUT::Tick` re-uploads a texture;
`OnConfigChanged` re-bakes LUTs, repacks UBOs and rebuilds VBOs. That is why
`edge-lighting-capi.h` currently says every effect call must run on the GL
thread.

So the entire job is one sentence: **make `Update` GL-free by deferring
renderer notification to `Render`.** Everything below is the machinery for
doing that safely.

## 3. The seam: a published config snapshot

### 3.1 The payload

New header `lib/include/core/config-snapshot.h`:

```cpp
typedef struct ConfigSnapshot
{
    Config config;                 ///< active = base + animation overlays
    float clockTime = 0.0f;        ///< Clock::GetTime() - freezes when paused
    float wallTime = 0.0f;         ///< monotonic sum of raw dt - never freezes
    uint64_t configGeneration = 0; ///< bumped ONLY when the composite actually changed
} ConfigSnapshot;
```

Three fields that each answer a question the render side cannot answer itself:

- **`clockTime`** is what renderers receive as their `time` argument. It pauses
  with the clock, which is today's behaviour and what the shaders' `uTime`
  expects.
- **`wallTime`** exists because `GradientRingLUT::Tick` is documented to take
  the *raw* frame delta, not clock time - a colour change must keep fading
  while the animation clock is paused. It is stored as an **absolute
  accumulator, not a delta**, so that a render thread which misses snapshots
  still advances the fade by the right amount: `dt = snap.wallTime -
  lastWallTime` is correct whether zero, one or five snapshots were dropped in
  between.
- **`configGeneration`** is how the render side knows whether to call
  `OnConfigChanged`. `refreshActiveConfig` already does the deep `Config`
  compare on the data side; the render side must not repeat it every frame, and
  it *cannot* compare against the previous snapshot because it hands that slot
  back on the next acquire. A counter that only moves on a real change carries
  exactly the information `refreshActiveConfig` already computed.

### 3.2 The handoff

`Config` owns vectors, so it is not trivially copyable and a seqlock is out.
A three-slot buffer with one atomic cell:

```cpp
class ConfigSnapshotBuffer
{
public:
    ConfigSnapshot &BeginWrite();    ///< data thread: the slot it privately owns
    void Publish();                  ///< data thread: release, take a slot back
    const ConfigSnapshot &Acquire(); ///< render thread: newest if any, else the held one

private:
    ConfigSnapshot mSlots[3];
    std::atomic<uint32_t> mReady;  ///< slot index in the low bits | DIRTY
    int mWrite = 0;                ///< data thread only
    int mRead = 1;                 ///< render thread only
};
```

- `Publish`: `mWrite = mReady.exchange(mWrite | DIRTY, acq_rel) & INDEX_MASK;`
- `Acquire`: if `mReady.load(acquire) & DIRTY`, then
  `mRead = mReady.exchange(mRead, acq_rel) & INDEX_MASK;` - either way return
  `mSlots[mRead]`.

Properties worth stating, because each one is a design constraint and not an
accident:

- **Exactly one thread touches a slot at a time.** `mWrite` and `mRead` are
  thread-private; the atomic cell is the only shared word. The release store on
  publish and the acquire load on take are what make the vector contents
  visible.
- **Neither side ever blocks.** The render thread takes no lock at all (see
  section 5.2), so a stalled UI thread can never hold up a frame. That is the
  property this whole plan exists to buy.
- **The render thread always has something valid to draw.** Nothing new means
  it keeps the slot it holds and redraws it. Render faster than data is a
  supported, silent case.
- **Data faster than render drops intermediate snapshots, and that is
  correct.** `OnConfigChanged` is idempotent with respect to the final state,
  so a skipped intermediate animated config is precisely a skipped frame.
- **Zero allocations after warm-up.** Each slot's `Config` keeps the vector
  capacity it was last written with, so `slot.config = mActiveConfig` is a
  copy-assign into warm buffers - the same property `mScratchConfig` was
  introduced for and documented at length in `edge-lighting.h`. The slots must
  therefore never be default-reconstructed, cleared or moved from.
- **All three slots are seeded with the default config at construction**, so a
  `Render` that arrives before any `Update` draws what it draws today rather
  than nothing.

Cost: three resident `Config` copies where there were two, plus one extra
copy-assign per tick (the publish). Sub-microsecond, zero allocations. Quantify
`sizeof(Config)` and the cap-case heap footprint in the comparison doc.

## 4. `EdgeLightingEffect`: one class, two sides

No class split. The same type keeps both halves, with the members and methods
grouped and documented by which thread owns them. This keeps `demo/` compiling
untouched and keeps the change reviewable.

**Data side** (no GL, host's data thread):

- `SetConfig` / `GetConfig` / `GetActiveConfig`
- `Attach` / `Detach` / `GetAnimationManager`
- `GetClock`
- `Update(float dt)` - tick clock, advance animations, compose the active
  config, **publish a snapshot**. Calls no renderer method. This is the change.

**Render side** (GL context, host's render thread):

- `Initialize`, `AddRenderer`, the destructor
- `Render(int w, int h)` - acquire the newest snapshot, then:

```cpp
const ConfigSnapshot &s = mSnapshots.Acquire();
if (s.configGeneration != mLastConfigGeneration)
{
    for (auto &r : mRenderers) { r->OnConfigChanged(s.config); }
    mLastConfigGeneration = s.configGeneration;
}
const float fadeDt = std::max(0.0f, s.wallTime - mLastWallTime);
mLastWallTime = s.wallTime;
for (auto &r : mRenderers) { r->Update(fadeDt, s.clockTime, s.config); }
for (auto &r : mRenderers) { r->Render(w, h, s.clockTime, s.config); }
```

`AddRenderer`'s immediate hand-over of the current config now reads the render
side's held snapshot rather than `mActiveConfig`.

### 4.1 Equivalence for a single-threaded host

Both demos and the whole offscreen-capture comparison methodology drive
update-then-render on one thread. Under the new model they must behave
identically, and the reasoning is:

- Relative order is preserved: `OnConfigChanged`, then renderer `Update`, then
  `Render`.
- `renderer->Update`'s delta was the value passed to `Update`; it is now the
  `wallTime` difference, which for update-then-render is the same number.
- **`OnConfigChanged` now fires once per frame instead of the documented
  up-to-twice.** `SetConfig` notifies, then `Update` recomposes and notifies
  again; the second call almost always carried the same config. This is the one
  behavioural risk in the plan, because `GradientRingLUT::Bake` has fade-start
  semantics keyed off when it is called. Both calls happen within the same
  frame before any draw, so the visible result should be identical - but this is
  exactly what the byte-identical capture suite in section 7 is for.

## 5. C ABI changes

### 5.1 No new entry points for the split itself

`el_effect_update` stays the data-side call, `el_effect_render` stays the
render-side call, and the seam is entirely internal. No ABI break, no threading
mode flag, no second pair of entry points to document. A single-threaded host
keeps working because it is the same code path.

### 5.2 The data lock

`el_effect_handle_impl` gains a mutex covering the data-side state:

```cpp
struct el_effect_handle_impl
{
    std::recursive_mutex dataMutex;   ///< guards staging + the effect's data side
    EdgeLighting::Config config;      ///< staging
    std::unique_ptr<EdgeLighting::EdgeLightingEffect> impl;
};
```

| call group | lock |
| --- | --- |
| `el_effect_set_*`, `el_effect_get_*`, `el_effect_capture` | take it briefly |
| `el_effect_attach_animation` / `detach*` / `clock_*` | take it briefly |
| `el_effect_update` | hold for the whole tick (compose + publish) |
| `el_effect_render` | **takes no lock at all** |

The render thread touching nothing but its own slot and one atomic is the
point. Hold time on the update path is the measured 0.0002 ms plus one snapshot
copy; setters run at UI rates, where a ~20 ns uncontended lock is free.

**Why recursive.** Animation callbacks (`el_animation_on_completed_callback`,
`el_animation_on_state_changed_callback`) fire from inside `impl->Update`, so
with the lock held. A host callback that calls back into `el_effect_set_*`
would self-deadlock on a plain mutex. The cleaner alternative - collect
callbacks and fire them after unlocking, so the callback observes a settled
state - requires changing `Animation`'s dispatch and is worth doing later;
recursive is the contained choice now.

### 5.3 Animation and modulator handles

`el_animation_*` mutates objects the data thread walks every tick, and those
calls receive no effect handle, so they cannot reach the effect's lock on their
own. Proposal: **an attached animation adopts its effect's lock.**

- `el_animation_handle_impl` carries a `std::shared_ptr<std::recursive_mutex>`,
  null while detached.
- `el_effect_attach_animation` sets it to the effect's mutex;
  `el_effect_detach_animation` clears it.
- Every `el_animation_*` entry point locks it when non-null and does nothing
  special when null (a detached animation is owned by whoever built it).

Consequences to document:
- Attaching one animation handle to two effects becomes unsupported. The
  existing "attach does not transfer ownership" wording already implies a single
  owner; this makes it a rule.
- `el_modulator_sequence_append` is the one mutating modulator entry point
  (everything else is a factory, plus the pure `el_modulator_evaluate`).
  Appending to a sequence that is already driving an attached animation is a
  race. Either give modulators the same adoption through the animation that
  holds them, or document append as construction-time only. **Open item** -
  resolve during implementation.

### 5.4 Batched writes

A UI thread that sets colour stops in a loop (`el_effect_set_color_stop` per
index) can be interrupted by a tick and publish a half-updated gradient. Single
scalar setters are mostly harmless and `el_effect_set_geometry` already takes
all five fields in one call, so the exposure is the indexed families: colour
stops, segments, preserved segments, arcs.

```c
EL_API el_result_e el_effect_begin_batch(el_effect_handle_t effect);
EL_API el_result_e el_effect_end_batch(el_effect_handle_t effect);
```

Implemented so that **no lock is ever held across host code**:

- `begin_batch` locks, copies staging into a shadow `Config`, records the owner
  thread id, sets a depth counter, unlocks.
- Setters called on the owner thread while a batch is open write the shadow
  with no lock at all (it is thread-private). Getters on the owner thread read
  the shadow, so a read-back inside the batch sees what was just written.
- `end_batch` locks, copies shadow into staging, clears the flag, unlocks.

Document: one open batch at a time, from one thread, and other threads must not
write staging while it is open.

### 5.5 Handle lifetime

`el_effect_init_with_renderers` currently constructs a brand new
`EdgeLightingEffect` and assigns it to `impl`. Under two threads that is a
pointer swap racing with every data-side call. Fix:

- `el_effect_create` constructs `impl`, so the pointer is immutable for the
  handle's lifetime.
- `el_effect_init_with_renderers` only registers and initialises renderers, and
  returns an error if called twice (today a second call silently rebuilds
  everything; after the change it would double-register layers).

Ordering rules to document and, where cheap, to enforce:

| call | thread |
| --- | --- |
| `el_effect_create` | any |
| `el_effect_init*` | the GL thread, before the data thread starts |
| `el_effect_destroy` | the GL thread, after the data thread has stopped |

`el_effect_destroy` runs GL deletes through the RAII wrappers, so it is a
render-thread call. The host is responsible for quiescing its own data thread
first; the library cannot do it, having spawned nothing.

### 5.6 Documentation

Rewrite the `@section threading` block in `edge-lighting-capi.h`. It currently
says every effect call must run on the GL thread, which stops being true. It
becomes the table in section 6, plus a `@thread` note on each function group in
`el-effect.h` and `el-animation.h`.

## 6. The host contract

| call group | thread |
| --- | --- |
| `el_effect_create` | any |
| `el_effect_init*` | GL thread, before the data thread starts |
| `el_effect_destroy` | GL thread, after the data thread has stopped |
| `el_effect_set_*` / `get_*` / `capture` / `attach_*` / `clock_*` | any |
| `el_effect_begin_batch` / `end_batch` | any, one open batch from one thread |
| `el_effect_update` | any, one thread at a time |
| `el_effect_render` | the GL-owning thread, exclusively |
| `el_animation_*` / `el_modulator_*` factories | any |

Notes a C# host needs and nothing more:

- Animation callbacks fire on whichever thread called `el_effect_update`.
  Marshal to the UI thread yourself.
- The library creates no threads, so there is no reverse-P/Invoke attach
  concern beyond the usual: keep delegates alive with a `GCHandle` for as long
  as native holds the function pointer.
- Handles stay plain opaque pointers. Nothing about the marshalling changes.

## 7. Validation

The bar in this repo is byte-identical captures plus a written comparison, and
this change should meet it.

1. **Behaviour.** Render the scene set used by
   `docs/neon-unification-comparison.md` through `OffscreenCapture` at 1920x1080
   on both sides of the change, single-threaded, and compare as raw RGBA8. The
   `configGeneration` consolidation from section 4.1 is the one place a
   difference could appear.
2. **Races.** The `-fsanitize=thread` build is already a workflow here (there is
   a configured `build-tsan/`). Add a `--threaded` mode to `demo-capi`: run
   `el_effect_update` on a spawned thread while ImGui and `el_effect_render`
   stay on the GL thread and the UI's setters fire from the GL thread. That
   exercises all three roles at once and keeps `demo-capi` in its designated
   role as the proof that the ABI is self-sufficient for a real host. Run it
   under TSan.
3. **Allocations.** Reuse the global `operator new` counter harness from the
   perf review to show the snapshot publish is 0 allocations per frame once
   warm.
4. **Doc.** `docs/threaded-model-comparison.md` with the capture table, the TSan
   result, and the allocation and `sizeof(Config)` numbers.

## 8. Sequencing

Five changes, each shippable on its own:

1. **Snapshot seam, still single-threaded.** `ConfigSnapshot` +
   `ConfigSnapshotBuffer`, renderer notification moved from `Update` to
   `Render`. No ABI change. All the behavioural risk lives here, and validation
   1 and 3 gate it.
2. **C ABI lock and lifetime.** `dataMutex`, `impl` constructed at create, the
   init-twice guard, the threading docs.
3. **Animation lock adoption.** Section 5.3, including the modulator decision.
4. **Batching.** Section 5.4.
5. **Threaded `demo-capi` under TSan** and the comparison doc.

## 9. Open items

- **Fade cadence.** The colour cross-fade advances by the snapshot wall-time
  delta, so a 30 Hz data thread makes it step at 30 Hz even when rendering at
  120 Hz. Acceptable, or should `el_effect_render` gain a render-supplied delta?
- **Modulator mutation after attach** (`el_modulator_sequence_append`) - adopt
  the lock, or document as construction-time only?
- **Reading animated values from the host.** There is no active-config read in
  the C ABI at all today; `el_effect_get_*` returns staging. A C# slider that
  should follow an animated value has nothing to read. The natural source is
  `GetActiveConfig` on the data side, under the same lock - an
  `el_effect_get_active_*` family, or one struct-shaped read. Worth adding
  alongside this work, or separately?
