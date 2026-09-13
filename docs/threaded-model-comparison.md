# Threaded model: the evidence

What the five changes in [`threaded-model-design.md`](threaded-model-design.md)
actually did, measured rather than argued. Read that document for why the seam
is shaped the way it is; this one is only the results.

Three questions, in the order they matter:

1. Did the pixels change? They must not.
2. Did the per-frame cost change? It must not.
3. Is it actually thread-safe? That is the whole point.

## 1. The pixels did not change

One harness source compiled twice - once against a worktree at the pre-change
commit, once against the branch - with **no `#ifdef` anywhere**. Nothing in this
work alters a public signature or a config semantic, so both sides run literally
the same program and any pixel difference is attributable to the seam.

Every scene renders through `OffscreenCapture` at 512x384 and is dumped as raw
RGBA8, compared byte-wise so the comparison never passes through a PNG decoder.
As in [`neon-unification-comparison.md`](neon-unification-comparison.md),
`hueRotationRate` and `colorTransitionDuration` are pinned to zero except where
a scene is about them.

**15 of 16 scenes byte-identical**, and the sixteenth differs by exactly one
LSB.

`double_update` - the scene that calls `Update` twice per `Render` - stopped
matching when the raw-time accumulator was widened from `float` to `double`
(review-findings I33, which fixed a cross-fade that froze permanently after 58
hours of uptime). Accumulating two deltas per frame makes the blend land a hair
differently:

| | |
| --- | --- |
| pixels differing | 221 of 196,608 (0.112%) |
| max channel delta | **1**/255 |
| mean over differing pixels | 1.00 - every difference is exactly 1 |
| whole-frame luma | 45.1028 before, 45.1028 after |

That is last-bit quantisation, not a behaviour change, and it is the correct
side of the trade. The other fifteen scenes, including the three other fade
scenes, remain byte-identical.

Nine are controls - `fullres`, `scaled_50`, `scaled_25`, `opaque_fill`, `arcs`,
`segments`, `droplets`, `flare`, `overlays`. The other seven exist because the
seam changed something specific about them:

| scene | what it pins down |
| ----- | ----------------- |
| `fade_mid` | a cross-fade sampled mid-flight, so the render side's `rawAccumulatedTime` differencing has to land on the same blend the old per-frame delta did |
| `fade_settled` | the same fade run to completion. It differs from `fade_mid` (luma 32.67 vs 45.10), which is what proves the fade scenes are sampling mid-flight instead of comparing two finished rings |
| `fade_double_render` | two `Render` calls per `Update` - the demo's capture path. Identical to `fade_mid`, so the extra draw does not advance the fade |
| `fade_clock_paused` | the clock paused for the whole fade. Identical to `fade_mid`, so the fade is driven by accumulated time and not by the clock |
| `double_update` | two `Update` calls per `Render`. Identical to `fade_mid`, so a dropped snapshot loses no fade time |
| `anim_pulse` | an `IntensityPulse` attached, so `refreshActiveConfig` takes its animated branch and the generation counter moves every frame |
| `set_then_render` | `SetConfig` with no `Update` before `Render` - which works only because `SetConfig` publishes |

Two of those scenes initially proved nothing and were fixed rather than counted.
`anim_pulse` at 30 frames sampled the oscillator exactly on its peak, so it
hashed identically to `fullres`; it runs 37 frames now. And the fade scenes had
no control establishing they were mid-flight, which `fade_settled` now provides.
A scene that compares equal for the wrong reason is worse than no scene.

## 2. The per-frame cost did not change

Steady state, 120 frames, one `IntensityPulse` attached, at the segment and arc
counts, with a global `operator new` counter:

| | before | after |
| --- | --- | --- |
| allocations in `Update` | 0 | **0** |
| allocations in `Render` | 124 | 124 |

The publish adds **zero** allocations, which was a requirement rather than a
nice result: a slot's `Config` keeps the vector capacity it was last written
with, so `slot.config = mActiveConfig` copy-assigns into warm buffers. This is
the property `mScratchConfig` was introduced for, and the reason nothing may
ever clear or move-from a slot. (The 124 in `Render` is roughly one per frame,
pre-existing, and untouched by this work.)

Memory, measured:

| | bytes |
| --- | --- |
| `sizeof(Config)` | 384 |
| `sizeof(ConfigSnapshot)` | 400 |
| `sizeof(ConfigSnapshotBuffer)` | 1216 |
| `sizeof(EdgeLightingEffect)` | 2440 |
| default config's colour-stop heap | 80 (4 stops) |

So the seam costs about 1.2 KB resident per effect plus whatever the three
slots' vectors hold. That heap is now bounded rather than open-ended:
`MAX_COLOR_STOPS_CAP` is 256 and both pool caps are 8, so a fully saturated
config holds 25 stop lists and the pathological case is a few hundred KB across
the three slots. A realistic config is a few hundred bytes.

## 3. It is thread-safe, and here is what that rests on

### 3.1 The demo, which is the honest test

`demo-capi --threaded` runs `el_effect_update` on its own thread while ImGui,
all 76 setter call sites, the `el_effect_read_*` calls behind the animated
sliders, and `el_effect_render` stay on the GL thread. That is all three host
roles at once, in a real UI-shaped program, through the flat ABI alone.

```
$ demo-capi --threaded --seconds 8      # built with -fsanitize=thread
[mode] THREADED: el_effect_update on its own thread; ImGui, the setters
       and el_effect_render stay on this one.
[mode] data thread ran 837 updates alongside 711 frames.
ThreadSanitizer warnings: 0
```

The counters are there so the result cannot be vacuous: a sanitizer reporting
zero races on a thread that never ran proves nothing. 837 updates against 711
frames also means the data side outran the render side, so the snapshot buffer's
drop path was exercised rather than merely present.

`--threaded` exists in `demo-capi` and deliberately **not** in `demo/`. The
asymmetry is the point rather than drift between the two forks: every
`el_effect_*` call takes the handle's lock, but `EdgeLightingEffect` itself has
none. The C++ class supports one data thread plus one render thread; the C ABI
is what additionally supports a UI thread. `demo/`'s ImGui panel edits config
from the GL thread, so the same split there would race.

### 3.2 The rest of the evidence

| harness | what it covers | result |
| --- | --- | --- |
| snapshot buffer, single-threaded | adversarial publish/acquire - writer 7 ahead, reader 50 ahead - asserting the writer's and reader's slots are never the same object | pass |
| snapshot buffer, two threads | 184k-334k cross-thread acquires, checking every snapshot is internally consistent and generations never go backwards | 0 races, 0 inconsistencies |
| pre-init guards | 12 calls that used to dereference a null `unique_ptr`, plus 5 unchanged contracts re-asserted | all refused, contracts held |
| callback re-entrancy | `el_animation_pause`, `get_elapsed`, `set_intensity` and self-detach called from inside a completion callback, two frames below `el_effect_update` | all `EL_SUCCESS` |
| 4-thread stress | render + data + UI + animation control; 2.1M frames, 1.1M updates, 88k write batches, 1.8M reads, 93k animation calls | 0 errors, 0 races |
| consistency scope | misuse, blocking, and tearing with/without the scope | see below |

### 3.3 The scope demonstrably does something

The tearing test is the one worth stating in full, because a clean result is
only meaningful next to a dirty one. An observer thread reads every colour stop
and counts how often it catches a set that does not agree with itself, while a
writer rewrites all six stops repeatedly:

| | torn observations | samples |
| --- | --- | --- |
| unwrapped | 2866 | 75,994 |
| wrapped in `begin_batch` / `end_batch` | **0** | 180,171 |

The unwrapped row is the control. Without it, the wrapped zero could mean the
scope works or could mean the observer never looked.

That test caught itself being useless once already: the first version used a
fixed iteration count, and the writer's tight `begin`/`end` loop starved the
observer outright - the mutex is not fair, so the wrapped phase collected **zero
samples** and the clean result proved nothing. It is timed with a yield now, and
asserts a sample floor so it fails rather than passing vacuously.

### 3.4 The sanitizer was verified to be looking

Every "0 races" above is worth exactly as much as the evidence that
ThreadSanitizer would have reported one. So the lock was removed and the
4-thread stress re-run:

```
with the lock:     0 warnings
without the lock:  418 warnings
```

The first report was precisely the expected one - `el_effect_set_intensity`
writing staging while `EdgeLightingEffect::SetConfig` read it. The header was
restored and re-verified immediately after.

## 4. Two defects found on the way

Neither was caused by this work; both were invisible until something drove the
library from two threads.

**The logger was not thread-safe.** `Util::Print` did six unsynchronised
`operator<<` on one `std::cout`. The first TSan run with an animation-control
thread reported **82 races, all of them this**. It is a genuine data race on the
stream's own state, not merely interleaved output, and any threaded host with
logging on would hit it. Fixed with a function-local static mutex around the
whole line, which also stops two threads' log lines interleaving mid-token - the
thread id in the prefix is worthless if lines do not stay whole.

**Twelve pre-init calls crashed the host.** The design document named one,
`el_effect_update`. In fact `capture`, `render`, all three `clock_*` and all
five animation calls dereferenced the same null `unique_ptr`. Only the
`el_effect_read_*` family was careful, through `ResolveConfigSource`. All twelve
now return `EL_ERROR_INVALID_HANDLE`. Nothing can depend on the old behaviour,
which was a crash.

## 5. What is not covered

- **`demo/` is untested under threading** and cannot be, as above.
- **Attach and detach write an animation's adopted lock non-atomically.** Doing
  either while another thread calls something else on that same handle is
  outside the contract, so the stress harness deliberately uses one animation
  for control calls and a different one for attach/detach churn. Testing the
  unsupported case would only prove it is unsupported.
- **Modulators have no lock at all.** `el_modulator_sequence_append` is
  construction-time only.
- **One platform, one compiler.** Everything here is macOS arm64 under
  Apple clang. The atomics and the lock discipline are portable by construction,
  but the numbers are not measurements of anything else.
- **None of these harnesses are in the tree.** They were written, run, and left
  in a scratch directory, the same way the active-config work's four assertion
  programs were. That was tolerable for a one-off ABI probe and is not tolerable
  here: a race harness nobody can re-run is a race harness that rots, and two of
  the five caught real defects. `demo-capi --threaded --seconds N` is the one
  piece that did land in the tree, and it is the one that can be re-run.
