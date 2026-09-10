# A two-thread model for the C ABI

**Status: implemented.** The model in this document ships in `lib/capi/`.
Section 10 is still out of scope and still is not implemented.

Three things the design got wrong survived into the code, and none was found by
reading it. **Detach is asynchronous** (section 7.4), which the first cut
treated as synchronous. **A command must never be gated on a mirror** (section
7.7), which shipped in the clock and silently dropped a `play()` issued behind
an undrained `pause()`. And **attach cannot read the live animation** (section
7.8), which raced `Animation::Update` and rolled a shadow back on any re-attach
issued before the detach was acknowledged. ThreadSanitizer found the first and
third; `tests/capi-threading-contract.c` found the second and the visible half
of the third.

All three are one mistake wearing different clothes: **a render-thread value
treated as though the data thread owned it.** 7.4 and 7.8 are the same
predicate missing from two different call sites. Those three sections are the
most useful paragraphs here for anyone extending this.

The goal is a host that drives the library from two threads: a **data thread**
that authors config, and a **render thread** that owns the GL context. That is
the shape a P/Invoke or JNI host naturally has - a UI thread and a
`GLSurfaceView` / swapchain thread - and today the ABI forbids it outright
([`edge-lighting-capi.h:79`](../lib/capi/edge-lighting-capi.h#L79)).

A drawn version of section 2's model, both threading paths side by side, is
[`capi-threading-workflow.html`](capi-threading-workflow.html).

## 0. The finding that reframes the question

The obvious seam is the one the ABI already names: `update` on one thread,
`render` on the other. **That seam does not exist.**

[`el_effect_update`](../lib/capi/el-effect.cpp#L1825) issues GL calls. It does
`SetConfig(staging)` then `impl->Update(dt)`, and both reach into GL:

- `SetConfig` -> [`refreshActiveConfig`](../lib/src/core/edge-lighting.cpp#L111)
  -> `OnConfigChanged` on all four renderers.
  [`NeonRenderer::OnConfigChanged`](../lib/src/renderer/neon-renderer.cpp#L581)
  re-uploads the geometry VBO, three LUT textures and the loop-samples UBO;
  [`DropletsRenderer`](../lib/src/renderer/droplets-renderer.cpp#L158) rebuilds
  its band-ring VBO;
  [`LensFlareRenderer`](../lib/src/renderer/lens-flare-renderer.cpp#L352)
  re-bakes `GhostBlock`;
  [`DebugRenderer`](../lib/src/renderer/debug-renderer.cpp#L182) rebuilds its
  strip quad and bakes its own ring.
- `impl->Update` -> [`NeonRenderer::Update`](../lib/src/renderer/neon-renderer.cpp#L388)
  -> `mGradientLUT.Tick` -> `glTexImage2D`, on every frame of a colour
  cross-fade ([`gradient-ring-lut.h`](../lib/include/renderer/gradient-ring-lut.h#L126)).

So `update` is a GL phase that happens to also do some arithmetic. Put it on a
context-less thread and the uploads are undefined behaviour, silently on some
drivers.

The seam that does exist runs through the middle of `OnConfigChanged`: **bake
(CPU bytes) on one side, upload plus draw (GL) on the other.** Splitting there
is a real project and it is deliberately **not** what this document proposes -
see [section 10](#10-out-of-scope-and-why).

## 1. What is actually on each side

Pure CPU work in the current update phase:

| work | where | cost |
| ---- | ----- | ---- |
| `AnimationManager::Update` + `Apply` | `EdgeLightingEffect::Update` | modulator arithmetic |
| `Config::operator==` + composite | `refreshActiveConfig` | ~1 us/frame, 0 allocations |
| `SegmentUtils::FillEffectiveSegments` | `NeonRenderer::OnConfigChanged` | gated on the segment pools |
| gradient / segment / arc LUT byte generation | `bakeLUTs` | the only non-trivial one - HSV/HSL conversion over `gradientLutSize` texels |
| `BakeGhostTable` | `LensFlareRenderer` | 160 bytes |
| `packLightBlocks` | `NeonRenderer::Render` | per-frame UBO staging |

Set against a frame where, by
[`docs/neon-perf-review.md`](neon-perf-review.md), the neon layer alone is 81%
of GPU time and its gather loop is 95% of that. **There is no throughput win
in moving this work.** Anyone proposing threading on performance grounds should
be asked for the profile first.

The wins are elsewhere, and they are real:

1. **Host decoupling.** The data thread stops blocking on a vsync'd render
   loop. A slider drag at 1000 Hz costs 1000 staging writes and one publish per
   frame, not 1000 round-trips through the GL thread.
2. **Jank isolation.** A `gradientLutSize` 1024 re-bake in `BlendSpace::HSL`
   currently runs inline between frames. Under this model the *arithmetic* half
   still runs on the render thread (see section 10), but the animation
   composite and the config diffing move off it.
3. **Newest-wins latency.** The render thread takes the most recent published
   config, never a queued backlog. No lag build-up when the data thread
   outruns the display.

## 2. The model

**One mailbox, one direction, animations frame-locked.**

- The **data thread** owns the staging `Config` in
  [`el_effect_handle_impl`](../lib/capi/capi-internal.h#L90) and every
  `el_effect_set_*` / `el_effect_get_*`. It calls `el_effect_publish` when it
  wants a snapshot to become visible.
- The **render thread** owns `impl` entirely: the `Clock`, the
  `AnimationManager`, `mBaseConfig` / `mActiveConfig`, and all four renderers.
  `el_effect_update` acquires the latest published snapshot instead of reading
  `effect->config`, then proceeds exactly as it does today.
- **Animations run on the render thread**, unchanged. `Clock` still ticks per
  frame, `AnimationManager::Update`/`Apply` still run inside
  `EdgeLightingEffect::Update`, and the composite is still frame-locked.
  Timing is byte-identical to the single-threaded path.

The alternative - running the animation system on the data thread and
publishing the already-composited *active* config - was rejected. It removes
the need for a command queue, but it ties animation smoothness to the data
thread's tick rate: a 60 Hz data thread against a 120 Hz display judders, and
matching the display means the data thread has to know the display rate, which
defeats the decoupling that motivated the split.

The price of frame-locking is that the ~30 mutating `el_animation_*` calls now
cross a thread boundary. Section 5 is how.

## 3. Ownership

Every exported symbol falls into one of four buckets.

**Data thread only.** All `el_effect_set_*` and `el_effect_get_*` (they read
and write staging and nothing else), plus `el_effect_publish`,
`el_effect_shutdown`. This is the large majority of the ABI surface. The one
exception is the `el_effect_get_active_*` family, which is still data-thread
only but does not read staging - it crosses the boundary and is in the table
below.

**Render thread only.** `el_effect_init` / `el_effect_init_with_renderers`,
`el_effect_update`, `el_effect_render`, `el_effect_destroy`. These create,
touch or destroy GL objects.

**Either thread, no shared state.** Every `el_modulator_*` function, and the
`el_animation_create*` factories. A freshly created modulator or animation is
owned by its creator until it is handed to an effect.

**Crosses the boundary - needs a mechanism.** The twelve effect functions that
reach `impl` beyond init/update/render/destroy, and the whole mutating
animation surface:

| function | direction | resolution |
| -------- | --------- | ---------- |
| `el_effect_capture` | reads `impl->GetConfig()` into staging | `EL_ERROR_INVALID_PARAMETER` in split mode; staging IS the source of truth there, so the call has no meaning (section 7.1) |
| `el_effect_get_active_*` | reads `impl->GetActiveConfig()` | reverse mailbox - the render thread publishes a copy at the tail of `update`, the data thread swaps it out on read (section 4.2) |
| `el_effect_clock_play` / `_pause` | writes `impl` | queued command |
| `el_effect_clock_is_playing` | reads `impl` | data thread shadows what it set, falling back to a mirrored atomic while it has never set anything (section 7.7) |
| `el_effect_attach_animation` / `_detach_animation` / `_detach_all_animations` | writes `impl` | queued command, carrying an `AnimationPtr` |
| `el_effect_get_animation_count` / `_contains_animation` | reads `impl` | data thread tracks its own attach set - it issued every command |
| `el_animation_play` / `_pause` / `_stop` / `_reset` | writes a live `Animation` | queued command |
| `el_animation_set_elapsed` / `_progress` / `_speed` / `_duration` / `_end_action` / `_playback_mode` | writes a live `Animation` | queued command |
| `el_animation_capture_baseline` | writes the animation, reads staging | queued command; executes against the *published* base (section 7.3) |
| `el_animation_get_state` / `_elapsed` / `_progress` | reads a live `Animation` | mirrored atomics (section 5.2) |
| `el_animation_get_speed` / `_duration` / `_end_action` / `_playback_mode` | reads a live `Animation` | data thread shadows what it set |
| `el_animation_update` / `_apply` | manual composition escape hatch | `EL_ERROR_INVALID_PARAMETER` for an *attached* animation; unchanged for a detached one (section 7.3) |
| `el_animation_destroy` | frees the handle | safe - queued commands hold their own `AnimationPtr` (section 5.1) |

The 20 symbols in `el-deprecated.h` touch staging only and inherit the
data-thread rule.

## 4. The config handoff

### 4.1 Data thread -> render thread: the staging config

A mutex and two `Config` slots. **Not a lock-free triple buffer.**

```cpp
struct ConfigMailbox
{
    std::mutex mutex;
    EdgeLighting::Config pending;   // written by the data thread
    EdgeLighting::Config consumed;  // read by the render thread
    bool hasPending = false;
    bool closed = false;
};
```

`el_effect_publish` (data thread):

```cpp
std::lock_guard<std::mutex> lock(mailbox.mutex);
mailbox.pending = effect->config;   // copy-ASSIGN
mailbox.hasPending = true;
```

`el_effect_update` (render thread), before its existing body:

```cpp
{
    std::lock_guard<std::mutex> lock(mailbox.mutex);
    if (mailbox.hasPending)
    {
        std::swap(mailbox.pending, mailbox.consumed);  // O(1)
        mailbox.hasPending = false;
    }
}
effect->impl->SetConfig(mailbox.consumed);   // outside the lock
effect->impl->Update(deltaTime);
```

Three things make this the right call rather than a compromise:

- **The copy-assign allocates nothing.** This is the same property
  `refreshActiveConfig` already relies on and documents at length in
  `mScratchConfig`: assigning into a warm `Config` reuses the vector capacity
  already there, where a copy-construct would allocate one block per vector
  (3 on the default config, 19 at the segment and arc caps). It measured ~1 us.
- **The render thread never holds the lock for a copy.** It holds it for a
  `std::swap` of two `Config` objects, which is six vector pointer swaps. The
  render thread cannot be made to wait on the data thread's work. This mirrors
  the `std::swap(mActiveConfig, mScratchConfig)` at the end of
  `refreshActiveConfig` and for the same reason.
- **`Config` holds `std::vector`s, so a torn read is UB, not a glitch.** A raw
  double-buffer pointer swap or a `memcpy` handoff would be wrong here in a way
  that does not show up in testing. The lock is what makes the copy legal.

If publish contention ever measures, the fix is to double-buffer the *staging*
side so the data thread's copy also happens outside the lock. It will not
measure at one publish per host tick.

`SetConfig` keeps its own `if (config == mBaseConfig) return;` gate
([`edge-lighting.cpp:63`](../lib/src/core/edge-lighting.cpp#L63)), so a publish
of an unchanged config still costs nothing downstream. A host may publish every
tick without thinking about it.

### 4.2 Render thread -> data thread: the active config

The same mailbox run backwards, and it is what backs `el_effect_get_active_*`
(base + every attached animation's overlay - what the renderers actually drew,
as opposed to the staging config every other `el_effect_get_*` reads).

`mActiveConfig` is rebuilt inside the render thread's `Update`, so the data
thread cannot read it directly, and what a direct read would tear is a whole
`Config` - vectors included - not a scalar. That is 7.4 restated in the read
direction: a render-thread value treated as the data thread's own.

**Three slots, not two.** The direction is reversed, so a two-slot swap would
put the copy on the wrong side of the lock:

```cpp
Config activeStage;      // RENDER THREAD, outside the lock. Copy target.
Config activePublished;  // swapped in by the render thread under the lock
Config activeRead;       // DATA THREAD, outside the lock, between swaps
bool hasActivePublished = false;
std::atomic<bool> activeWanted{false};
```

`publishActiveConfig` (render thread), at the tail of `el_effect_update`:

```cpp
if (!activeWanted.load(std::memory_order_relaxed)) { return; }
activeStage = effect->impl->GetActiveConfig();      // copy OUTSIDE the lock
{
    std::lock_guard<std::mutex> lock(mutex);
    std::swap(activeStage, activePublished);        // O(1)
    hasActivePublished = true;
}
```

`activeConfigFor` (data thread), inside every active getter:

```cpp
activeWanted.store(true, std::memory_order_relaxed);
{
    std::lock_guard<std::mutex> lock(mutex);
    if (hasActivePublished)
    {
        std::swap(activePublished, activeRead);
        hasActivePublished = false;
    }
}
return &activeRead;
```

Copy straight into `activePublished` under the lock instead and the render
thread holds the mutex for a full `Config` copy, which is exactly what 4.1's
two-slot swap exists to prevent. The third slot buys that back. Note also that
each swap hands the other side the buffers the outgoing config owned, so after
the first published frame neither the copy nor the swap allocates - the same
capacity-recycling property as `mScratchConfig`.

Three consequences, all of them in the header:

1. **One frame behind.** A read sees the last `update` that finished, never the
   one in flight. There is no version of this that does not, short of blocking
   the data thread on the render thread's frame.
2. **The first read arms publication and reports the config as of
   `el_effect_init`.** Publication costs one `Config` copy-assign per frame, and
   most hosts never ask for animated values, so nothing is published until
   someone does - `activeWanted` is the gate, and a host that never reads pays
   one relaxed load per frame and nothing else. The price is that the arming
   read has no frame to report yet; `el_effect_init` seeds `activeRead` from the
   staging config so it returns something true rather than a default-constructed
   `Config`, and live values arrive from the next update.
3. **A run of reads can straddle a frame.** Each getter independently takes the
   newest published frame, so a publish landing midway through a host's UI pass
   leaves later fields from the newer frame. This is the same bargain
   `AnimMirror` already strikes for `el_animation_get_elapsed` (5.2), and for
   the same readout: nobody perceives one slider lagging its neighbour by a
   frame, and the alternative is holding the render thread's mutex across the
   host's entire UI pass.

Single mode does none of this - the caller is the only thread, so the getters
read `GetActiveConfig()` straight through. The split path is behind
`activeConfigFor`, so both modes present one API and a host writes one code
path.

## 5. The animation control queue

### 5.1 Commands

A fixed-capacity SPSC ring, drained at the top of `el_effect_update` on the
render thread, before `impl->Update`.

```cpp
struct AnimCommand
{
    enum class Op { PLAY, PAUSE, STOP, RESET, CAPTURE_BASELINE,
                    SET_ELAPSED, SET_PROGRESS, SET_SPEED, SET_DURATION,
                    SET_END_ACTION, SET_PLAYBACK_MODE,
                    ATTACH, DETACH, DETACH_ALL,
                    CLOCK_PLAY, CLOCK_PAUSE } op;
    EdgeLighting::AnimationPtr anim;   // shared_ptr, NOT the raw handle
    union { float f; int32_t i; } arg;
};
```

The command holds an `AnimationPtr`, not an `el_animation_handle_t`. That is
what makes `el_animation_destroy` safe to call from the data thread with
commands still in flight: destroying the handle frees
`el_animation_handle_impl`, but the `Animation` itself survives as long as any
queued command references it.

The ring is a `std::array<AnimCommand, N>` constructed once. Steady-state
enqueue costs one `shared_ptr` copy and no allocation. On overflow the enqueue
fails and returns `EL_ERROR_OUT_OF_MEMORY` rather than blocking - a data thread
that outruns the drain by `N` commands in one frame has a bug, and blocking the
UI thread on the GL thread is exactly what this design exists to prevent. `N`
of 256 is a starting guess; it wants a note in `el-types.h` alongside the other
caps.

### 5.2 Reading animation state back

The reverse direction is three fields the host polls for a progress bar. After
`AnimationManager::Update` returns, the render thread writes each attached
animation's `{state, elapsed, progress}` into a small mirror block hanging off
`el_animation_handle_impl`, as three relaxed atomics.

Tearing *between* the three is possible and harmless: a UI readout that shows
last frame's `elapsed` against this frame's `state` for one frame is not a
defect worth a seqlock. This should be said out loud in the header, because it
is the kind of thing someone later "fixes" at the cost of a lock on the render
thread's hot path.

`el_animation_get_speed` / `_duration` / `_end_action` / `_playback_mode` do
not go in the mirror, but they do need a **shadow** on the handle rather than a
read-through to the live object: the render thread WRITES those four when it
drains the corresponding command, so reading them back off the `Animation`
would be a data race on a non-atomic float. The shadows are seeded at attach
and advance only when a queued setter is accepted, so a full queue cannot leave
the data thread reporting a value that never landed.

"Seeded at attach" carried a rider that was wrong: *the last moment the
animation is provably not yet on the render thread's list*. It is not provable
there, and section 7.8 is the case where it is false.

The mirror carries a fourth field beyond the three above, `attached`, which is
an acknowledgement rather than a readout. Section 7.4.

The clock's play state belongs on the shadow side of this line too, for exactly
the reason the four animation values do - the data thread is its only writer.
Getting that wrong is section 7.7.

## 6. Callbacks

`Animation::OnComplete` fires from inside `Animation::Update`
([`animation.h:295`](../lib/include/animation/animation.h#L295)), which under
this model runs on the **render thread**. The lambda installed by
[`el_animation_set_on_complete_callback`](../lib/capi/el-animation.cpp#L602)
therefore calls the host's function pointer on the GL thread.

That is a trap with two teeth:

- A managed callback (C# delegate, JNI upcall) that blocks stalls the frame.
- The obvious thing to do inside a completion callback is set some config - and
  `el_effect_set_*` is data-thread-only, so the obvious thing is a data race.

Proposed resolution: a **deferred dispatch queue**, symmetric with the command
ring but pointing the other way. The render thread pushes
`{callbackFn, userData}` records; the data thread drains them with a new
`el_effect_poll_callbacks(fx)`. Inside a drained callback the host is on the
data thread and `el_effect_set_*` is legal.

Whether direct render-thread dispatch stays available as an opt-in
(`el_effect_set_callback_dispatch`) is an open question - see section 11. The
default must be deferred.

## 7. Hazards

### 7.1 `el_effect_capture`

[`el_effect_capture`](../lib/capi/el-effect.cpp#L1809) copies
`impl->GetConfig()` back into staging. It exists so a host that lost track of
what it set can resynchronise from the effect's base.

In split mode the data thread cannot read `impl`, and the call has no meaning
anyway: staging is upstream of `mBaseConfig`, so the value it would fetch is
one the data thread published itself. Return `EL_ERROR_INVALID_PARAMETER` in
split mode and document the reason. Unchanged in single mode.

Note this is not the same question as reading the ACTIVE config, which is a
real one and has a real answer: `mActiveConfig` is base plus the animation
overlays the render thread composited, so it is genuinely not something the
data thread already knows. That is what `el_effect_get_active_*` and the
reverse mailbox in 4.2 are for. Capture stays refused because it targets
staging; the active family crosses the boundary properly instead.

### 7.2 Destruction

`el_effect_destroy` frees GL objects, so it belongs to the render thread. But
the data thread may still be inside a `publish` or an enqueue.

Shutdown protocol:

1. Data thread calls `el_effect_shutdown(fx)`. Under the mailbox lock this sets
   `closed`, after which `el_effect_publish` and every command enqueue return
   `EL_SUCCESS` and do nothing.
2. Data thread stops touching the handle. This is the host's responsibility and
   cannot be enforced from here.
3. Render thread exits its loop and calls `el_effect_destroy`.

Skipping step 1 and destroying while the data thread publishes is a
use-after-free. It should be the loudest paragraph in the header.

### 7.3 The three animation functions that straddle

`el_animation_reset`, `el_animation_capture_baseline` and `el_animation_apply`
all take *both* handles and touch `effect->config` - staging -
([`el-animation.cpp:368`](../lib/capi/el-animation.cpp#L368),
[`:396`](../lib/capi/el-animation.cpp#L396),
[`:510`](../lib/capi/el-animation.cpp#L510)) while also touching the
`Animation`, which under this model is render-thread state.

- `reset` and `capture_baseline` become queued commands. The render thread runs
  them against `impl->GetConfig()`, which is the last *published* base. This is
  a small semantic shift - previously they read staging including unpublished
  edits - and it is the correct one: the animation should baseline against
  what the renderers are actually showing.
- `apply` and `el_animation_update` are the manual-composition escape hatch
  that predates the effect owning an `AnimationManager`. For an **attached**
  animation in split mode they must fail with `EL_ERROR_INVALID_PARAMETER`:
  the manager is already ticking that object every frame on the other thread.
  For a **detached** one they stay exactly as they are, on the data thread,
  operating on staging.

### 7.4 Detach is asynchronous

**This is the one the design got wrong.** `el_effect_detach_animation` only
queues a DETACH. The first implementation cleared the handle's `stagedOwner`
back-pointer immediately, on the reasoning that the data thread had said its
piece - which meant the very next `el_animation_play` ran *inline* on the data
thread. But at that moment the render thread may not have drained the DETACH
yet, so the manager is still ticking that animation, and every command queued
ahead of the DETACH is still in flight. ThreadSanitizer caught it on the first
run: an inline `Animation::Play` on the data thread against a draining
`Animation::SetElapsed` on the render thread, same object, no synchronisation.

The fix is an acknowledgement. `AnimMirror::attached` is set **true by the data
thread** when it queues an ATTACH - which covers the window before the ATTACH
itself runs - and cleared **only by the render thread**, when it executes the
matching DETACH or DETACH_ALL. Because the queue is FIFO, everything queued
before that DETACH has already run by the time the flag drops. `stagedOwner`
therefore survives the detach and is cleared lazily, by `splitOwner`, on the
first data-thread call after the acknowledgement arrives. `splitOwner` is now
the only legitimate way to read that field, and the routing predicate for every
mutation and every shadowed getter.

Two consequences worth knowing:

- A re-attach queued behind a DETACH would leave the flag false if only the
  data thread set it, so the render thread sets it true on ATTACH as well.
  FIFO ordering then lands on the right answer whichever order the host asked
  in.
- If the host detaches and then shuts down before the render thread drains,
  the acknowledgement never arrives and the animation stays routed at a closed
  queue - its mutations succeed and do nothing. `el_effect_destroy` is the
  release valve: it severs `stagedOwner` on every live handle still aimed at
  it, which is also what stops an animation outliving its effect from queuing
  into freed memory.

### 7.5 Logging

[`Util::Print`](../lib/include/util/log-util.h#L74) writes a chain of `<<`
into `std::cout`. The stream object is safe from corruption, but nothing
prevents two threads interleaving mid-line. Lines already carry
`std::this_thread::get_id()`, so the interleaving is at least attributable.

Not worth a lock on the render thread's path. Worth one line in the doc so it
is not mistaken for a bug later. Note `LOG_D` is already compiled out on
macOS and Windows by
[`capi-internal.h`](../lib/capi/capi-internal.h#L23).

### 7.6 What is not a hazard

`el_effect_get_*` reads staging, which the data thread owns exclusively. Those
are safe by construction and need no mechanism. That is most of the getter
surface, and it is worth noting because the natural assumption is the opposite.

### 7.7 Never gate a command on a mirror

**The second one the design got wrong**, and the same shape as 7.4: a
render-thread readout used as though it were the data thread's own record.

`el_effect_clock_play` / `_pause` shipped with their idempotence guard reading
`clockPlaying`, the mirror section 5.2 describes. That mirror only advances
inside `el_effect_update`, so an opposing pair issued between two frames found
it still reading the pre-drain value on the second call and returned
`EL_SUCCESS` **without queueing anything**. The last command lost:

```c
el_effect_clock_pause(fx);   /* queues PAUSE; mirror still says "playing" */
el_effect_clock_play(fx);    /* sees "playing", concludes no-op, DROPS it  */
/* ... one el_effect_update later: the clock is paused. */
```

The window is one render frame wide, and a data thread deliberately decoupled
from vsync - which is the entire motivation in section 1 - crosses it
constantly. `tests/capi-threading-contract.c` covers both orderings.

The fix is `ThreadedState::shadowClockPlaying`, the same device as the four
`el_animation_handle_impl` shadows and for the same reason. Which puts the
general rule plainly, because the two mechanisms in section 5.2 are not
interchangeable and this is the distinction:

- a value the **render thread computes** (`state`, `elapsed`, `progress`) is a
  **mirror**. Read it, never gate on it - it is by construction a frame old.
- a value the **data thread sets** and nothing in the library changes on its
  own (`speed`, `duration`, `endAction`, `playbackMode`, and the clock's play
  state - nothing inside `EdgeLightingEffect` ever starts or stops the clock)
  is a **shadow**. Gate on it, read it back from it, and advance it only when
  the enqueue is accepted, so a full queue cannot leave the data thread
  reporting a value the render thread never got.

`shadowClockPlaying` is tri-state (`-1` = the host has never touched the clock)
rather than a bool, because the clock's own default is not knowable at
`el_effect_set_threading_mode` time - there is no `impl` yet - and guessing it
would reintroduce the same dropped command one frame earlier. While it is `-1`,
`el_effect_clock_is_playing` falls through to the mirror, which is the one case
the data thread genuinely has no record of.

### 7.8 Attach cannot read the live animation

A corollary of 7.4, and the one the extended contract test found.

`el_effect_attach_animation` seeded the four shadows and the three mirror
fields by reading the live `Animation`, on the reasoning quoted in 5.2: at that
line the animation is not on the render thread's list yet. That holds for a
first attach. It is false for a **re-attach issued before the DETACH ahead of
it has been acknowledged** - `stagedAttached` is already empty by then, so the
duplicate check lets the call through, while the manager is still ticking the
object every frame. The eight reads race `Animation::Update`, and TSan flags it
on the first detach/attach pair.

It is not only a race. The value read back is *stale*, because a setter queued
just before the detach has not been drained yet, so the re-attach rolls the
shadow back to a value the host already overwrote:

```c
el_animation_set_speed(anim, 3.0f);   /* queued; shadow says 3.0 */
el_effect_detach_animation(fx, anim); /* queued, not acknowledged */
el_effect_attach_animation(fx, anim); /* re-seeds from the live object... */
el_animation_get_speed(anim, &s);     /* ...and s is 2.5, the pre-set value */
```

The fix is one predicate: seed only when `splitOwner` says the animation is
genuinely the data thread's. When it is not, there is nothing to seed from -
the animation never left the render thread, so the shadows already hold what
the original attach seeded plus every setter accepted since, and the mirror is
still being republished each frame.

The general form, which is 7.4 restated: **`splitOwner` is the only predicate
that answers "may I touch this animation", and every path that touches one has
to ask it - reads included.** The mutating setters route through
`QUEUE_IF_SPLIT`, which asks. Attach did not.

## 8. ABI additions

Deliberately small. The host loop keeps its current shape.

```c
typedef enum el_threading_mode_e {
    EL_THREADING_SINGLE = 0,  /* today's contract - everything on the GL thread */
    EL_THREADING_SPLIT  = 1   /* data thread + render thread */
} el_threading_mode_e;

EL_API el_result_e el_effect_set_threading_mode(el_effect_handle_t effect,
                                                el_threading_mode_e mode);
EL_API el_result_e el_effect_publish(el_effect_handle_t effect);
EL_API el_result_e el_effect_poll_callbacks(el_effect_handle_t effect);
EL_API el_result_e el_effect_shutdown(el_effect_handle_t effect);
```

`el_effect_set_threading_mode` must be called before `el_effect_init` and is
immutable afterwards. Everything else - `el_effect_update`, `el_effect_render`,
every setter - keeps its current signature.

The `el_effect_get_active_*` family added later is not on this list on purpose:
it is not a split-mode call. It works in both modes with one signature, and the
reverse mailbox of 4.2 lives entirely behind it, so the host loop below gains
nothing and a host reading animated values writes the same code either way.

`el-types.h` also gained `EL_THREADED_QUEUE_CAPACITY` (256), the bound on both
queues. It is public because a host that can receive
`EL_ERROR_OUT_OF_MEMORY` from a setter needs to know what it means: not an
allocation failure but "more than this many commands between two frames", and
the fix is to retry after the next frame rather than to free memory.

Host loop under `EL_THREADING_SPLIT`:

```c
/* data thread */                      /* render thread */
el_effect_set_neon_intensity(fx, i);   el_effect_update(fx, dt);
el_effect_set_geometry(fx, ...);       el_effect_render(fx, w, h);
el_effect_publish(fx);                 swap_buffers();
el_effect_poll_callbacks(fx);
```

## 9. Migration

`EL_THREADING_SINGLE` is the default and is the current code path byte for
byte: `el_effect_update` reads `effect->config` directly, `el_effect_publish`
is a no-op returning `EL_SUCCESS`, the command ring is never allocated, no
mutex is taken. An existing host recompiles and behaves identically, and
`demo-capi/` needs no change.

**Verified**, by a C-only program built against the `.dylib` (the same kind of
guard `demo-capi/` is, one thread further):
[`tests/capi-threading-contract.c`](../tests/capi-threading-contract.c). It is
in-tree but deliberately **not wired into CMake** - there is still no test
target and this does not add one - so it is built by hand, the way `demo-capi`
links, which is the one detail worth writing down:

```
clang -std=c11 -I lib/capi -I external/include tests/capi-threading-contract.c \
  -o build/capi-threading-contract \
  -L build/lib -ledge-lighting-c -L external/lib/arm64 -lglfw.3 \
  -Wl,-rpath,"$PWD/build/lib" -Wl,-rpath,"$PWD/external/lib/arm64" \
  -framework Cocoa -framework OpenGL -framework IOKit
```

It runs in two phases. Phase 1 drives both sides from one thread and calls
`el_effect_update` by hand, which makes every ordering rule in this document
deterministic rather than timing-dependent - that is what lets it assert things
like "`contains` goes false immediately but the escape hatch is still refused
until the ack". Phase 2 spawns a real render thread and hammers it, and asserts
almost nothing: it exists to give ThreadSanitizer something to watch.

Do **not** also compile `external/src/glad.c` into it. The dylib exports its
own glad symbols (glad.c is C, so the `hidden` visibility preset on the C++
targets does not reach it), and a second copy in the executable gives the host
and the library independent function-pointer tables - the host loads its own,
the library's stay null, and the first `glClear` inside the library segfaults.
`demo-capi` avoids this by accident of link order; a hand-built program has to
avoid it on purpose.

What it covers - 81 assertions:

- Mode selection: unknown enumerator refused, immutability past init,
  re-selecting the current mode accepted as a no-op.
- The config mailbox: `el_effect_capture` refused, staging authored before
  `el_effect_init` reaching frame one without a publish, published config
  arriving, unpublished edits NOT arriving, two publishes before a frame being
  newest-wins rather than a backlog.
- The clock, twice over. Once through `el_effect_clock_is_playing`, which
  reports the data thread's own record, and once **end to end** - a paused
  clock feeds the manager a zero delta, so a frozen mirrored `elapsed` is the
  only evidence the flat ABI exposes for what the render thread's clock is
  really doing. Both same-frame orderings, each starting from the state its
  FIRST call changes: begin a pair in the state its second call asks for and
  the dropped command changes nothing observable, so the test passes against
  the bug. That is section 7.7's regression, and getting the precondition
  right is most of it.
- Attach / count / contains answered from the data thread's own record while
  still undrained, attach deduplicated, and the `el_animation_update` /
  `_apply` refusals while attached.
- **Section 7.4's window explicitly**: after a detach, `contains` goes false
  immediately but the escape hatches are still refused until the render thread
  acknowledges, and are allowed the moment it does. Same for `DETACH_ALL`, and
  a re-attach afterwards routes through the queue again.
- Shadowed getters reading back before the drain; mirrored getters lagging one
  frame by design and catching up after it.
- Section 7.8: a re-attach issued before the ack leaving the shadows alone,
  which is that bug's visible half - TSan sees the race, phase 1 sees the
  rolled-back value.
- Both deferred callbacks: not fired from `el_effect_update`, fired exactly
  once from `el_effect_poll_callbacks`, on the DATA thread, and not repeated by
  a second poll.
- Queue overflow accepting exactly `EL_THREADED_QUEUE_CAPACITY` and then
  failing rather than blocking, recovery after a drain, and the whole capacity
  being available again the next frame.
- The shutdown protocol, and an animation outliving its effect staying safe to
  mutate.

- Phase 2 additionally counts every callback that fires on a thread other than
  the data thread, which is section 6's guarantee and the one property phase
  2's non-reproducible timing can still assert exactly.

Then phase 2 under **ThreadSanitizer**, against a TSan-instrumented build of
the library: zero races. That is what found section 7.4, and it is the check to
re-run after any change here - the contract is not the sort of thing reading
can confirm. Note that phase 1's determinism cuts the other way too: it drains
by hand, so it can prove the *ordering* rules and cannot see a race at all.

- **The 30-frame pixel hash**, in both directions. Phase 3 renders one
  deterministic scene - fixed dt, a live animation, three colour stops - into
  an FBO it owns, and hashes each frame. SINGLE and SPLIT come out
  byte-identical on all 30, which is the "split mode changes *when* a config
  reaches `SetConfig`, never *what* the renderers do with it" claim measured
  rather than argued. It also prints each frame's hash as
  `HASH single <i> <hex>`, so running the same binary against two builds of the
  library and diffing those lines is how you check that a change to `lib/capi`
  left the single-threaded path alone.

  Two traps that phase carries its own guards for, having fallen into both.
  `NeonConfig::enable` defaults to **false** and `DebugConfig::showWireframe`
  to **true**, so a scene that only sets geometry renders the debug box and
  nothing else; and the base `colorStops` vector is empty by default, so an
  enabled ring with no stops bakes to black. Either one makes every frame hash
  identical and the cross-mode comparison vacuously true - it would "pass" over
  two blank images. Hence the third assertion, that the hashes are not all one
  value.

One thing the landed file does **not** do: nothing here reaches the exception
path inside the command drain, which requires an allocation failure in
`runCommand`. The normal-path clear is covered.

Also verified through **`demo-capi --threaded`**, which drives the existing
ImGui UI across the split - the ergonomics test rather than the contract test.
It runs clean under ThreadSanitizer too, demo-side sharing included.

Three things that demo had to get right, and that any real host will meet:

- **The main thread is the DATA thread, not the render thread.** On macOS GLFW
  windowing and event polling are main-thread-only, so the render thread is the
  spawned one. That inverts the intuition that the "render loop" is the main
  loop, and it decides where everything else has to live.
- **Main-thread-only GLFW calls cannot move.** `glfwGetFramebufferSize` and
  `glfwWindowShouldClose` stay on the main thread, so the framebuffer size is
  published to the render thread like any other parameter and the loop is
  driven by an atomic rather than by `glfwWindowShouldClose`.
  `glfwMakeContextCurrent` and `glfwSwapBuffers` are the exceptions and may go
  on the render thread, which is what makes the split possible at all.
- **Demo-side shared state needs the same treatment as the config.** The
  background toggles, the backdrop texture id and the rect geometry are read by
  the render thread and written by the main one, so they are published under
  one mutex and snapshotted whole - a partial snapshot would tear a frame the
  same way a partial `Config` would. The library's mailbox solves the config;
  it does not solve the host's own state, and a host that forgets this gets a
  race the library cannot see.

The two windows already had separate sharing contexts, which is what makes the
demo's version tidy: the render thread holds the main window's context and the
main thread holds the debug window's, one context per thread. A host with a
single context has to hand it over rather than split it.

The demo also sets `glfwSwapInterval(1)` on the render thread only, so the
payoff is visible rather than argued: the effect is pinned to the display while
the ImGui window keeps running at its own rate.

## 10. Out of scope, and why

**Moving the LUT bakes off the GL thread.** The real CPU offload means
splitting every `OnConfigChanged` into `BakeCPU(config) -> bytes` and
`UploadGPU() -> GL`, adding two virtuals to `BaseRenderer`, and
double-buffering the resulting packet. It touches `GradientRingLUT`, all four
renderers, and every dirty gate in `NeonRenderer::OnConfigChanged`.

It is out of scope until a profile asks for it, for three reasons. The frame is
GPU-bound by a wide margin. The dirty gating that makes the current code fast
would have to be duplicated across two phases, and duplicated gating is exactly
the failure mode `NeonRenderer`'s comments spend paragraphs warning about.
And `GradientRingLUT::Tick` returns the bool that drives `mEmissionDirty`
([`neon-renderer.cpp:396`](../lib/src/renderer/neon-renderer.cpp#L396)), so
splitting it means splitting the emission table's staleness signal too.

**Shared GL contexts.** The FBOs, the emission pre-pass and both blit paths are
one context's state. A second context buys nothing here and shared contexts are
unreliable on the drivers this library targets.

**A general command queue for config.** Config is state, not events. Newest
wins, and a mailbox says that; a queue would let the render thread fall behind
by a growing backlog.

## 11. Open questions

Settled by the implementation:

- **Callback dispatch** is deferred-only. No opt-in for direct render-thread
  dispatch shipped, because the trap it opens (a callback calling
  `el_effect_set_*` from the GL thread) is silent and the cost it saves is one
  frame of latency on an animation chain. Revisit if a host actually needs
  zero-latency chaining; the mechanism is one branch in the two lambdas.
- **Queue capacity** is `EL_THREADED_QUEUE_CAPACITY`, 256, public in
  `el-types.h`. Still a guess rather than a measurement - the worst realistic
  burst is a host attaching a preset group and playing each, which is a
  handful - but it now has a name and a documented overflow contract.

Still open:

1. **Should `el_effect_publish` have a "nothing changed" fast path?** It could
   compare staging against `pending` before copying and skip the lock. That is
   a second full `Config::operator==` on the data thread to save a ~1 us copy;
   probably not worth it, but it is cheap to measure.
2. **Should thread-ownership violations be diagnosable?** Recording the thread
   id at `el_effect_init` and returning `EL_ERROR_INVALID_PARAMETER` when a
   render-thread-only call arrives from elsewhere would turn every contract
   violation in section 3 into an error instead of a race. Costs one
   `std::thread::id` compare per call. Worth it in debug builds at minimum -
   section 7.4 is evidence that these are easy to get wrong and invisible
   without tooling.
3. **Does the data thread need a way to wait for a drain?** Everything is
   fire-and-forget today, which is right for config but leaves no way to ask
   "has my DETACH landed yet" other than polling a getter. A frame counter
   published by the render thread would answer it cheaply.
