# Threaded model: data thread + render thread

Plan for splitting the effect into a data side and a render side so a C# host
can mutate configuration, animations and the clock on one thread while the GL
context is driven from another.

Status: design only. Nothing in this document is implemented yet.

Re-checked against `8c28e37` (the active-config C ABI merge). `lib/src/core/`,
`lib/include/core/edge-lighting.h` and every renderer are byte-unchanged since
this was first written, so sections 2 to 4 stand as they were. The C ABI is not:
the `el_effect_read_*` family, the in-place re-init path and the enforced config
caps all landed, and sections 5 onward are rewritten for them. Section 9's
"reading animated values" item is closed by work that has now shipped.

## 1. What this is for, and what it is not for

The decision that shapes everything else: **the library spawns no threads.**
The host owns both. Native gains a thread-safe seam, not a scheduler.

| | |
| --- | --- |
| Goal | A C# host may call setters, the `el_effect_read_*` family, animation control and the clock from any thread while `el_effect_render` runs on the GL thread, with no races and no half-applied frames. |
| Data side owns | staging config, base config, active config, `Clock`, `AnimationManager`, config composition. |
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
- Any change to the effect's lifecycle semantics. In particular the in-place
  re-initialisation path added in `62253a1` stays exactly as it is; see section
  5.6, which replaces an earlier draft of this document that would have broken
  it.

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
    float clockTime = 0.0f;        ///< Clock::GetTime() - every clock control moves it
    float rawAccumulatedTime = 0.0f; ///< sum of the dt fed to Update - no clock control moves it
    uint64_t configGeneration = 0; ///< bumped ONLY when the composite actually changed
} ConfigSnapshot;
```

Three fields that each answer a question the render side cannot answer itself:

- **`clockTime`** is what renderers receive as their `time` argument. It pauses
  with the clock, which is today's behaviour and what the shaders' `uTime`
  expects.
- **`rawAccumulatedTime`** is the running sum of the deltas handed to `Update`,
  and **no clock control reaches it** - not pause, not stop, not reset, not a
  scrub, all of which move `clockTime`. It exists because
  `GradientRingLUT::Tick` is documented to take the *raw* frame delta, not
  clock time: a colour change must keep fading whatever the animation is doing.
  It is stored as an **absolute accumulator, not a delta**, so that a render
  thread which misses snapshots still advances the fade by the right amount:
  `dt = snap.rawAccumulatedTime - lastRawAccumulatedTime` is correct whether
  zero, one or five snapshots were dropped in between. (`Bake` and `Tick` gained an upper size clamp in the active-config
  merge and nothing else; their fade semantics are unchanged, so this reasoning
  still holds.)
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
- **Neither side ever blocks.** `el_effect_render` takes no lock (see section
  5.2), so a stalled UI thread can never hold up a frame. That is the property
  this whole plan exists to buy.
- **The render thread always has something valid to draw.** Nothing new means
  it keeps the slot it holds and redraws it. Render faster than data is a
  supported, silent case.
- **Data faster than render drops intermediate snapshots, and that is
  correct.** `OnConfigChanged` is idempotent with respect to the final state,
  so a skipped intermediate animated config is precisely a skipped frame.
- **Zero allocations after warm-up.** Each slot's `Config` keeps the vector
  capacity it was last written with, so `slot.config = mActiveConfig` is a
  copy-assign into warm buffers - the same property `mScratchConfig` was
  introduced for and documented at length in `edge-lighting.h`, and the same
  one `AnimationManager::mTickScratch` was later built on. The slots must
  therefore never be default-reconstructed, cleared or moved from.

Cost: three resident `Config` copies where there were two, plus one extra
copy-assign per tick (the publish). Sub-microsecond, zero allocations.

The worst case is now bounded rather than open-ended, which it was not when this
document was first drafted. `MAX_COLOR_STOPS_CAP` (256) caps every stop list,
`MAX_SEGMENT_BOOSTS_CAP` and `MAX_ARCS_CAP` are both 8, and the C ABI count
setters now reject past them (review finding I16). So a `Config` holds at most
1 + 8 + 8 + 8 = 25 stop lists of 256 stops: on the order of 128 KB fully
saturated, and a few hundred bytes for any realistic config. Three extra
resident copies is therefore a bounded few hundred KB in the pathological case
and nothing in practice. Quantify `sizeof(Config)` and the measured heap in the
comparison doc.

- **All three slots are seeded with the default config at construction**, so a
  `Render` that arrives before any `Update` draws what it draws today rather
  than nothing.

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
const float fadeDt = std::max(0.0f, s.rawAccumulatedTime - mLastRawAccumulatedTime);
mLastRawAccumulatedTime = s.rawAccumulatedTime;
for (auto &r : mRenderers) { r->Update(fadeDt, s.clockTime, s.config); }
for (auto &r : mRenderers) { r->Render(w, h, s.clockTime, s.config); }
```

`AddRenderer`'s immediate hand-over of the current config now reads the render
side's held snapshot rather than `mActiveConfig`.

A side benefit worth naming, because the code it protects is new: `Initialize`
is re-entrant by contract (review finding R1) and `el_effect_init_with_renderers`
now uses that as the GL-context-loss recovery path, rebuilding shaders and GL
objects in place and dropping any renderer whose re-initialisation fails. Under
this split that path touches only render-side state, because `Update` no longer
reaches `mRenderers` at all. Recovering from context loss while a data thread is
mid-tick is therefore safe by construction rather than by timing.

### 4.1 Equivalence for a single-threaded host

Both demos and the whole offscreen-capture comparison methodology drive
update-then-render on one thread. Under the new model they must behave
identically, and the reasoning is:

- Relative order is preserved: `OnConfigChanged`, then renderer `Update`, then
  `Render`.
- `renderer->Update`'s delta was the value passed to `Update`; it is now the
  `rawAccumulatedTime` difference, which for update-then-render is the same number.
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
    std::recursive_mutex dataMutex;   ///< guards staging, impl, and the effect's data side
    EdgeLighting::Config config;      ///< staging
    std::unique_ptr<EdgeLighting::EdgeLightingEffect> impl;
    uint32_t rendererMask = 0;
};
```

| call group | lock |
| --- | --- |
| `el_effect_set_*`, `el_effect_get_*`, `el_effect_capture` | take it briefly |
| **`el_effect_read_*` (the whole family)** | **take it for the whole call** - see 5.3 |
| `el_effect_attach_animation` / `detach*` / `clock_*` | take it briefly |
| `el_effect_update` | hold for the whole tick (compose + publish) |
| `el_effect_init*` | take it (setup and context-loss recovery, not the frame path) |
| `el_effect_render` | **takes no lock at all** |

State the invariant precisely, because `init` is also a render-thread call: the
guarantee is that **`el_effect_render` never blocks**, not that the render
thread never touches the lock. Init taking it is correct and costs nothing; it
runs at startup and on context loss, not per frame.

Hold time on the update path is the measured 0.0002 ms plus one snapshot copy;
setters run at UI rates, where a ~20 ns uncontended lock is free.

**Why recursive.** Animation callbacks (`el_animation_on_completed_callback`,
`el_animation_on_state_changed_callback`) fire from inside `impl->Update`, so
with the lock held. The most natural thing to write in one is "detach me now",
which reaches `el_effect_detach_animation` and re-enters the lock; on a plain
mutex that is a self-deadlock. `AnimationManager::Update` was reworked to make
exactly that callback shape safe - it walks a local copy swapped out of
`mTickScratch`, so attach, detach and re-entry from a callback are all
supported - and a plain mutex here would take that back by turning a supported
pattern into a hang. The cleaner alternative, collecting callbacks and firing
them after unlocking so the callback observes a settled state, requires changing
`Animation`'s dispatch and is worth doing later; recursive is the contained
choice now.

While the lock is being added, fix a null dereference it sits next to:
`el_effect_update` does `effect->impl->SetConfig(...)` with no check, so calling
it before `el_effect_init` crashes. Every `el_effect_read_*` guards this through
`ResolveConfigSource`; update does not.

### 5.3 The `el_effect_read_*` family

This family did not exist when this document was first drafted and it is the
largest new surface the threaded model has to cover. It is also the one place
where the hazard is a use-after-free rather than a stale value.

Each reader calls `ResolveConfigSource`, which hands back a raw
`const EdgeLighting::Config *` pointing at `effect->config` (STAGING),
`impl->GetConfig()` (BASE) or `impl->GetActiveConfig()` (ACTIVE), and then reads
one scalar through `field-access.h`. For BASE and ACTIVE that pointer aims
straight at live data-side state:

- `refreshActiveConfig` **swaps** the active config's vectors with the scratch
  copy, and the next tick overwrites the buffers the swap handed over. A
  concurrent indexed read walks reallocated memory.
- `SetConfig` copy-assigns into `mBaseConfig`, which reallocates its vectors
  whenever the incoming config is larger.

`el-effect.h` already documents this, under `@par Threading` on the read group:
read on the update thread, and hand values to other threads yourself. That
wording is correct today and **this plan supersedes it** - the whole point is
that a C# slider should be able to read ACTIVE from the UI thread. Taking the
data lock for the duration of each reader is what makes it true, and it is
enough for memory safety because of a property the active-config review already
established and wrote down: *no reader retains the resolved `Config *` past its
own call*. Under the lock that constraint tightens to "past its own lock scope",
and it must stay an invariant of the family.

Two things the lock does **not** fix, both of which are about consistency rather
than safety:

- **A read loop is not atomic.** The documented idiom is `el_effect_read_count`
  followed by N indexed reads, and an `el_effect_update` can land in the middle.
  Every reader re-validates its own index, so there is no out-of-range access -
  the host just gets a mix of two frames, and `el_effect_read_preserved_id`
  followed by a preserved field read can report an id that is no longer live.
  Section 5.5 is the answer.
- **`el_effect_read_count` on `EL_CONTAINER_EFFECTIVE_SEGMENTS`** delegates to
  `SegmentUtils::CountEffectiveSegments`, which is pure arithmetic over two
  `.size()` calls and allocates nothing. Cheap to hold the lock across, and no
  special handling needed.

The existing `@par Which frame you get` paragraph needs rewording rather than
replacing: "the last completed `el_effect_update`" stays true, but it is now
the last completed update *on whichever thread runs it*, and the interleaving
warning becomes the reason section 5.5's scope exists.

### 5.4 Animation and modulator handles

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

### 5.5 A consistency scope, for reads and writes

Two problems, one answer:

- A UI thread that sets colour stops in a loop (`el_effect_set_color_stop` per
  index) can be interrupted by a tick and publish a half-updated gradient.
  Single scalar setters are mostly harmless and `el_effect_set_geometry` already
  takes all five fields in one call, so the write exposure is the indexed
  families: colour stops, segments, preserved segments, arcs.
- A read loop straddles two frames (section 5.3).

```c
EL_API el_result_e el_effect_begin_batch(el_effect_handle_t effect);
EL_API el_result_e el_effect_end_batch(el_effect_handle_t effect);
```

`begin_batch` takes the data lock and `end_batch` releases it, with a depth
count so nesting works. The piece that needed care and is not obvious from the
sketch: `end_batch` has to know whether the CALLING THREAD owns the scope before
it unlocks, and at that moment it may own nothing at all - an unmatched close,
or a close from the wrong thread. Reading the depth counter to find out is the
very race the answer is needed to avoid, and unlocking a `recursive_mutex` this
thread does not hold is undefined behaviour rather than an error code. So the
owner is an `std::atomic<std::thread::id>`, written under the lock and read
without it. Inside the scope every setter and every reader finds the
lock already held by its own thread (it is recursive), so writes commit as a
group and BASE/ACTIVE hold still across a whole read loop.

This is a deliberate reversal of an earlier draft, which had `begin_batch` copy
staging into a thread-private shadow so that no lock was ever held across host
code. That design is safer against a host that forgets `end_batch`, but it
cannot give read consistency at all - a shadow of staging says nothing about
BASE or ACTIVE - and the read family is now the bigger half of the problem.
Holding the lock is the only thing that covers both.

What that costs, stated plainly: a host that leaves a batch open stalls the next
`el_effect_update`. It does **not** stall rendering, because `el_effect_render`
takes no lock; the ring simply stops advancing until the batch closes. Document
it as one open scope at a time, from one thread, closed on every path including
error returns - which matters more now that the count setters actually reject
over-cap values (review finding I16) and a mid-batch setter can fail.

### 5.6 Handle lifetime

**No change.** An earlier draft of this document proposed constructing `impl` in
`el_effect_create` and refusing a second `el_effect_init`. Both halves of that
are now wrong, and the reasons are worth recording so nobody proposes them
again:

- **Refusing a second init** would undo `62253a1`. A second call with the same
  mask is the GL-context-loss recovery path: re-initialise in place, keep
  attached animations and the clock. A second call with a *different* mask is
  already refused, for the separate reason that the layer set is fixed by the
  registration order and there is no unregister.
- **Constructing `impl` at create** would make BASE and ACTIVE readable before
  init, which contradicts the semantics `el_config_source_e` documents and that
  the active-config work verified by probe: STAGING is readable on an
  uninitialised handle, BASE and ACTIVE are refused.

The threading problem the draft was trying to solve - a non-atomic `impl`
pointer observed by a concurrent reader - is handled by the data lock instead
(section 5.2), which is where it belonged. `impl` is written once, inside the
lock, and every reader of it takes the lock.

Ordering rules to document:

| call | thread |
| --- | --- |
| `el_effect_create` | any |
| `el_effect_init*` | the GL thread; first call before the data thread starts, re-init safe at any time |
| `el_effect_destroy` | the GL thread, after the data thread has stopped |

`el_effect_destroy` runs GL deletes through the RAII wrappers, so it is a
render-thread call. The host is responsible for quiescing its own data thread
first; the library cannot do it, having spawned nothing, and a lock cannot help
because the object holding it is the one going away.

### 5.7 Documentation

Rewrite the `@section threading` block in `edge-lighting-capi.h`. It currently
says every effect call must run on the GL thread, which stops being true. It
becomes the table in section 6, plus a `@thread` note on each function group in
`el-effect.h` and `el-animation.h`. The `@par Threading` paragraph on the read
group in `el-effect.h` is superseded wholesale (section 5.3), and
`el-animation.h`'s "called on the effect thread" note on the callbacks becomes
"called on whichever thread called `el_effect_update`, with the effect's lock
held".

## 6. The host contract

| call group | thread |
| --- | --- |
| `el_effect_create` | any |
| `el_effect_init*` | GL thread; first call before the data thread starts |
| `el_effect_destroy` | GL thread, after the data thread has stopped |
| `el_effect_set_*` / `get_*` / `capture` / `attach_*` / `clock_*` | any |
| `el_effect_read_*` | any (was: the update thread) |
| `el_effect_begin_batch` / `end_batch` | any, one open scope from one thread |
| `el_effect_update` | any, one thread at a time |
| `el_effect_render` | the GL-owning thread, exclusively |
| `el_animation_*` / `el_modulator_*` factories | any |

Notes a C# host needs and nothing more:

- Animation callbacks fire on whichever thread called `el_effect_update`, with
  the effect's lock held. Re-entering the effect from one is legal; blocking in
  one stalls the data thread. Marshal to the UI thread yourself.
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
2. **Races.** Add a `--threaded` mode to `demo-capi`: run `el_effect_update` on
   a spawned thread while ImGui and `el_effect_render` stay on the GL thread and
   the UI's setters and `el_effect_read_*` calls fire from the GL thread. That
   exercises all three roles at once, drives the read family across a thread
   boundary (which is the new hazard), and keeps `demo-capi` in its designated
   role as the proof that the ABI is self-sufficient for a real host. Run it
   under `-fsanitize=thread`.
3. **Allocations.** Reuse the global `operator new` counter harness from the
   perf review to show the snapshot publish is 0 allocations per frame once
   warm.
4. **Doc.** `docs/threaded-model-comparison.md` with the capture table, the TSan
   result, and the allocation and `sizeof(Config)` numbers.

The harness style is already established: the active-config work was verified by
four standalone assertion programs (C-only against the dylib, and one with a
hidden GLFW context), none checked in because "there is nowhere for them to
live yet". A threading harness has the same problem and a worse consequence -
a race harness that nobody can re-run is a race harness that rots. Worth
deciding, as part of this work, whether these get a `tests/` directory.

## 8. Sequencing

Five changes, each shippable on its own:

1. **Snapshot seam, still single-threaded.** `ConfigSnapshot` +
   `ConfigSnapshotBuffer`, renderer notification moved from `Update` to
   `Render`. No ABI change. All the behavioural risk lives here, and validation
   1 and 3 gate it.
2. **The data lock.** `dataMutex` across staging, `impl`, the setters, the
   getters and the whole `el_effect_read_*` family; the `el_effect_update` null
   check; the threading docs including the superseded read-group paragraph.
   No lifecycle changes.
3. **Animation lock adoption.** Section 5.4, including the modulator decision.
4. **The consistency scope.** Section 5.5.
5. **Threaded `demo-capi` under TSan** and the comparison doc.

Steps 1 to 4 have landed. What each one turned up that this plan did not
predict is recorded where the code is; the two worth repeating here:

- The C ABI crashed on TWELVE pre-init calls, not the one this plan named.
  `el_effect_update` was the only one it listed; `capture`, `render`, every
  clock call and every animation call dereferenced the same null `unique_ptr`.
- The library's own logger was not thread-safe. `Util::Print` did six
  unsynchronised `operator<<` on one `std::cout`, which ThreadSanitizer
  reported as 82 races the first time anything drove the library from two
  threads. It is a pre-existing defect, invisible until then, and a blocker for
  any threaded host with logging on.

## 9. Open items

- **Fade cadence.** The colour cross-fade advances by the snapshot wall-time
  delta, so a 30 Hz data thread makes it step at 30 Hz even when rendering at
  120 Hz. Acceptable, or should `el_effect_render` gain a render-supplied delta?
- **Modulator mutation after attach** (`el_modulator_sequence_append`) - adopt
  the lock, or document as construction-time only?
- **Where the assertion harnesses live.** Section 7.

Closed since the first draft:

- ~~Reading animated values from the host.~~ The `el_effect_read_*` family
  shipped in `8c28e37` with `EL_CONFIG_SOURCE_ACTIVE`, which is exactly this.
  It turns into work for this plan rather than a question for it: section 5.3.
