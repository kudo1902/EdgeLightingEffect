# A two-thread model for the C ABI

**Status: implemented.** The model in this document ships in `lib/capi/`.
Section 10 is still out of scope and still is not implemented.

One thing the design got wrong survived into the code and was caught by
ThreadSanitizer rather than by review: **detach is asynchronous**, and the
first cut treated it as synchronous. Section 7.4 is the finding and the fix -
it is the most useful paragraph here for anyone extending this.

The goal is a host that drives the library from two threads: a **data thread**
that authors config, and a **render thread** that owns the GL context. That is
the shape a P/Invoke or JNI host naturally has - a UI thread and a
`GLSurfaceView` / swapchain thread - and today the ABI forbids it outright
([`edge-lighting-capi.h:79`](../lib/capi/edge-lighting-capi.h#L79)).

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
`el_effect_shutdown`. This is the large majority of the ABI surface.

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
| `el_effect_clock_play` / `_pause` | writes `impl` | queued command |
| `el_effect_clock_is_playing` | reads `impl` | mirrored atomic |
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
would be a data race on a non-atomic float. The shadows are seeded at attach -
the last moment the animation is provably not yet on the render thread's list -
and advance only when a queued setter is accepted, so a full queue cannot leave
the data thread reporting a value that never landed.

The mirror carries a fourth field beyond the three above, `attached`, which is
an acknowledgement rather than a readout. Section 7.4.

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

**Verified**, by a C-only two-thread program built against the `.dylib` (the
same kind of guard `demo-capi/` is, one thread further). It is not in-tree -
there is no test target to hang it on - so to reproduce, write it against
`lib/capi` and build it the way `demo-capi` links, which is the one detail
worth writing down:

```
clang -std=c11 -I lib/capi -I external/include test.c -o test \
  -L build/lib -ledge-lighting-c -L external/lib/x86_64 -lglfw.3 \
  -Wl,-rpath,"$PWD/build/lib" -Wl,-rpath,"$PWD/external/lib/x86_64" \
  -framework Cocoa -framework OpenGL -framework IOKit
```

Do **not** also compile `external/src/glad.c` into it. The dylib exports its
own glad symbols (glad.c is C, so the `hidden` visibility preset on the C++
targets does not reach it), and a second copy in the executable gives the host
and the library independent function-pointer tables - the host loads its own,
the library's stay null, and the first `glClear` inside the library segfaults.
`demo-capi` avoids this by accident of link order; a hand-built program has to
avoid it on purpose.

What it covers:

- One scene rendered for 30 frames in each mode, hashed per frame index and
  compared frame for frame. All 30 pairs byte-identical, and stable across
  runs. Split mode changes *when* a config reaches `SetConfig`, never *what*
  the renderers do with it, and the hashes say so.
- 41 contract assertions: mode immutability past init, `el_effect_capture`
  refusal, published config arriving, unpublished edits NOT arriving, the clock
  command plus mirror round trip, attach/count/contains, the
  `el_animation_update` / `_apply` refusals while attached, the deferred
  completion callback firing exactly once and on the DATA thread, the shadowed
  getters, queue overflow failing rather than blocking, recovery after a drain,
  and the shutdown protocol.
- The whole thing under **ThreadSanitizer**, against a TSan-instrumented build
  of the library: zero races. That is what found section 7.4, and it is the
  check to re-run after any change here - the contract is not the sort of thing
  reading can confirm.

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
