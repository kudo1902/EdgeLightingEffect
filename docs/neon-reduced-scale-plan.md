# Neon below scale 1.0: cleanup and render-time plan

The owner's target (2026-10-07): raise the frame rate with the neon at a
`resolutionScale` below 1.0. This document answers two questions for that
target - what code can go, and what is left to take off a frame - with
measurements of the tree as it stands, not of the history behind it.

Measured on 2026-10-07 at `b0ac8cf`, on an **Apple M2 Pro** (arm64, macOS GL
4.1 on Metal), a Release build, the neon layer alone, 1920 x 1080. The target
device is not this one: read ratios and shares, not milliseconds, and see
section 5, step 0. The probes are not checked in; section 2 says how to rebuild
them.

**Result.** Below 1.0, what a frame costs depends on what moved in it far more
than on the scale. A still frame (0.018 ms of GPU at 0.5) or one where only the
hue rotates (0.042 ms) is already cheap - the fields of I50 and I52 did that.
A frame whose neon CONFIG changes - an intensity pulse, an arc wipe, a
travelling segment, any `SetConfig` that touches `config.neon` - costs 4-10x
more (0.18-0.44 ms at 0.5), because every cache the renderer has is
invalidated by it. That is where the remaining time is, and the reduced scale
helps there only with the one pass it scales: below ~0.35 the passes that do
not scale - the gather, the coverage bake, the edge ring and the blit - are
most of the frame. Separately, the production band (`GlowSide::OUTSIDE` with an
outside cutoff) is slower at 0.5 than at 1.0 in every frame type, and takes
neither field.

## 1. Where it stands

- **`neon-scale-check check` passes at `b0ac8cf`.** The 1.0 column reads 1
  everywhere. Against 1.0, every scene is within 2/255 down to 0.35 and within
  3/255 at 0.25 (`small_rect`); only 0.125 on `small_rect` (10 / p99 4) is
  visibly off.
- **The reduced scale buys nothing on still or hue frames.** At 0.5 vs 1.0,
  geometric mean over the twelve check scenes: still 0.055 vs 0.092 ms, hue
  0.154 vs 0.187 - and flat from 0.75 down (table 3.1). It pays on frames whose
  config changes: intensity 0.268 vs 0.492, arc wipe 0.402 vs 0.736, segment
  travel 0.492 vs 0.824.
- **Scales below 0.5 still pay, on those frames only.** 0.25 against 0.5:
  intensity 1.39x, arc wipe 1.37x, segment travel 1.31x; 0.35: 1.20-1.25x.
  Quality cost: one level (section 1, first bullet). No code involved.
- **The CPU is not the problem on this machine.** With one command-buffer
  submit per frame, as a host that swaps has, `Render` takes 0.015-0.018 ms of
  CPU on a still frame and 0.04-0.07 ms on a moving one, most of it the
  driver encoding each pass. The library's own C++ - including the uniform
  name lookups of `docs/neon-perf-plan.md` item 13 - is a few percent of that.
  A slower target CPU can change this; measure there before acting on it.
- **`neon-scale-check time` overstates the CPU of moving frames.** It never
  swaps, so the driver submits mid-frame whenever a pass renders into a texture
  that an earlier, unsubmitted frame read: once per hue frame, about five times
  per segment-travel frame. The FRAME totals it reports are still right (those
  frames are GPU-bound either way: 0.26 vs 0.26 ms per intensity frame with and
  without a `glFlush` per frame), but its CPU share is not a host's.

## 2. Method

**Timing.** `neon-scale-check time --mode <m> --size 1920x1080` for every mode,
one round, for the tables in 3.1 (the tool's minimum-of-five per figure).

**Attribution probe.** A throwaway program linked against the static library,
in the shape of `tools/common/pass-recorder.cpp`: it swaps
`glad_glDrawArrays`, names each draw's pass from its program's uniforms
(`PassRecorder`'s classification, with the ring field's composite filed as
P2R), and times frames with one pass's draws skipped - the difference is that
pass's cost, its share of the CPU included. One frame per configuration is also
drawn with a `GL_SAMPLES_PASSED` query round each draw, for the fragment
counts. Three rounds, rotated, median. Six configurations on a 1920 x 1080
frame, rect centred, `colorTransitionDuration` 0:

| name | rect | what |
| ---- | ---- | ---- |
| `demo` | 960 x 540 | the defaults |
| `soft` | 960 x 540 | `soft_wash`'s neon (lineWidth 10, glowRadius 30, bloom 1.2, intensity 1.5) |
| `arc` | 960 x 540 | one arc of length 0.5 |
| `seg` | 960 x 540 | one `SegmentBoost`, length 0.1, boost 1 |
| `band` | 1840 x 1000 | the production band: `GlowSide::OUTSIDE`, outside cutoff `{true, 20, 4}` |
| `band_both` | 1840 x 1000 | the same rect with the defaults |

**GPU time.** A second probe wraps each frame in a `GL_TIME_ELAPSED` query and
calls `glFlush` after it, so each frame is one submit as on a host, and times
`SetConfig`, `Update` and `Render` separately on the CPU.

**CPU profile.** `sample` on that probe running hue and segment-travel frames.
It is how the mid-frame submits of section 1 were found (`GLDContextRec::
flushContext` under `renderEmissionPass`, `renderGlowCoverPass`,
`renderGatherPass` and `renderNeonPass`'s clears). Unbinding every texture
unit before each framebuffer bind did not remove them; a `glFlush` per frame
did.

## 3. Where the time goes below 1.0

### 3.1 By frame type

`neon-scale-check time`, geometric mean over the twelve check scenes, ms per
frame (wall clock, the tool's figure):

| mode | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ---- | --: | ---: | --: | ---: | ---: | ----: |
| `still` | 0.092 | 0.054 | 0.055 | 0.056 | 0.055 | 0.057 |
| `hue` | 0.187 | 0.171 | 0.154 | 0.152 | 0.147 | 0.147 |
| `intensity` | 0.492 | 0.403 | 0.268 | 0.214 | 0.193 | 0.184 |
| `arc-wipe` | 0.736 | 0.596 | 0.402 | 0.322 | 0.293 | 0.281 |
| `segment-travel` | 0.824 | 0.681 | 0.492 | 0.409 | 0.377 | 0.362 |

GPU time alone, `demo`, one submit per frame:

| frame | 1.0 | 0.5 |
| ----- | --: | --: |
| still | 0.028 | 0.018 |
| hue rotating | 0.062 | 0.042 |
| intensity pulse | 0.455 | 0.180 |
| arc wipe | 0.699 | 0.302 |
| segment travel | 0.838 | 0.440 |

`segments` is the outlier of the hue column: 0.391 ms at 0.5 against ~0.135
for every other scene, since a config with segments takes neither field.

### 3.2 By pass, on frames whose config changes

Each pass's cost (frame with its draws skipped, subtracted from the frame),
ms, and the fragments it shades:

| config, frame | scale | total | P0B coverage bake | P1A gather | P1B shading | P2B blit | P2C ring |
| ------------- | ----: | ----: | ----------------: | ---------: | ----------: | -------: | -------: |
| `demo`, intensity | 0.5 | 0.260 | - | 0.051 | **0.122** | 0.026 | 0.024 |
| | 0.25 | 0.177 | - | 0.051 | 0.040 | 0.026 | 0.029 |
| `soft`, intensity | 0.5 | 0.407 | - | 0.054 | **0.227** | 0.028 | 0.061 |
| | 0.25 | 0.252 | - | 0.051 | 0.068 | 0.027 | 0.068 |
| `arc`, arc wipe | 0.5 | 0.380 | 0.044 | 0.058 | **0.180** | 0.027 | 0.039 |
| | 0.25 | 0.260 | 0.044 | 0.057 | 0.063 | 0.025 | 0.042 |
| `seg`, travelling | 0.5 | 0.473 | 0.092 | 0.088 | **0.180** | 0.018 | 0.031 |
| | 0.25 | 0.360 | 0.098 | 0.096 | 0.068 | 0.026 | 0.048 |
| fragments, `demo` at 0.5 | | | 131,072 (arc) / 94,432 (seg) | 11,776 | 430,920 | 1,654,452 | 80,028 |

- **At 0.5 the reduced-scale shading is the largest pass, 38-56%.** It is the
  only one the scale shrinks - 2.6-3.3x from 0.5 to 0.25.
- **At 0.25 the passes that do not scale are 58-74% of these frames.** The
  gather runs at its own scale (`GetGatherScale`), so its ~0.05 ms (0.09-0.10
  with a segment, which doubles its fetches and attachments) is the same at
  every reduced scale. The coverage bake is scale-free by construction. The
  ring and the blit are full-resolution, and the ring WIDENS as the scale drops
  (`RING_GUARD_TEXELS / scale`).
- **The gather re-runs on frames that cannot have moved it.** It reads the
  shape, the sample positions and the emission table, none of which an
  intensity, bloom, glow-radius or cutoff change touches. It re-runs anyway,
  because `OnConfigChanged` clears `mOffscreenCurrent` on any neon change, and
  because its region (`mGatherOuter`, sized from the glow quad) moves with
  every one of those changes (item 4 step 2 of `docs/neon-perf-plan.md` was
  parked on exactly this).
- **An arc wipe re-bakes the whole coverage table every frame.** All 131,072
  texels, one draw. `GetGlowCoverDirtyPieces` dirties the union of the arc's old
  and new supports, and an arc of length 0.3-0.9 touches every piece - though
  only its end moved.

### 3.3 Still and hue frames

At 0.5 a still frame is the blit and the ring's composite on the caller's
framebuffer and nothing else (`demo` 0.053 ms: blit 0.039, ring field 0.003). A
hue frame adds three small offscreen passes - the emission table (256
fragments), the gather and the field composite - for 0.042 ms of GPU in all. On
the M2 neither has much left to take; what they do have is a fixed
full-resolution cost, the blit's 1.64 M fragments for the default rect, which a
fill-rate-bound GPU will feel far more than this one does.

### 3.4 The production band

| frame | 1.0 | 0.5 | 0.25 |
| ----- | --: | --: | ---: |
| still | **0.054** | 0.061 | 0.076 |
| hue | **0.102** | 0.134 | 0.136 |
| intensity | **0.089** | 0.110 | 0.110 |

1.0 wins every column on the M2. At 0.5 the band's still frame is mostly its
ring (0.047 of 0.061 ms), re-shaded directly every frame: the ring's field
(I52) refuses a one-sided glow and a cutoff, and the band has both. The
shading's field refuses it too, below `FIELD_MIN_FILL` (the band's quad is a
thin frame in a full-screen box). Below 0.5 the ring only widens. This is
`docs/neon-perf-plan.md` item 11 again, confirmed on `b0ac8cf`; on a
fill-rate-bound GPU the comparison could flip (1.0 shades 137 k full-resolution
fragments with the whole glow; 0.5 blits 99 k and shades 53 k of ring), so it
is a target measurement, not a conclusion.

## 4. Cleanup: what can go

### 4.1 Safe - no pixel moves, no decision needed

| # | what | where | size |
| - | ---- | ----- | ---- |
| C1 | **One lazy-bake state machine for the two fields.** `useField` / `bakeField` and `useRingField` / `bakeRingField` are the same schedule written twice - current, settled, unavailable, the viewport and gradient-upload invalidation, the first-frame bake - and `OnConfigChanged` clears both by hand. One small struct (`Decide(viewport, gradientUploads, programsBuilt) -> {use, bake}`, `Invalidate()`, `OnBaked(bool)`) holding five members each. | `Render` lines 1949-2029; header 770-808; `OnConfigChanged` 2266-2269 | ~60 lines, and the two cannot drift |
| C2 | **One hue-invariance test.** `IsFieldEligible` and `IsRingFieldEligible` both spell out "no segments, and a ring time cannot move" before their own conditions. | `neon-renderer.cpp` 1235-1258 | small |
| C3 | **One reduced-target setup.** `renderNeonPass` and `renderFieldCompositePass` each lift the scissor, `Resize` the reduced buffer, bind it and clear it. | 3780-3793, 3884-3896 | ~15 lines |
| C4 | **One render-target capture per frame.** `Render`, `renderEmissionPass` and `renderGlowCoverPass` each `RenderTargetState::Capture` (two `glGetIntegerv`); the passes can take `Render`'s. Also item 13's second bullet. | 2032, 3488, 3589 | small, CPU |
| C5 | **Stale doc comments in `neon-renderer.h`.** The pass-order note says that at 1.0 "pass 1 ... gathers itself ... and neither 1a, 2b nor 2c runs" (1a runs at every scale since `cdcc19c`); `resizeGatherBuffer` is "called every scaled frame" and "released whenever the scaled path is idle" (every frame; only when the layer is off); `mGatherVertexArray` is "Scaled path" (both); the class comment points at "the gather in neon.frag" (`neon-gather.frag`) and counts "two programs at 1.0, four below" (the fields add up to three). | header 22-34, 104-109, 252-260, 262-278, 655 | comments |
| C6 | **`PassKind::P1` in the tools.** "No longer built", but still classified. | `tools/common/pass-recorder.{h,cpp}` | 2 lines |

### 4.2 Needs the owner's call

- **C7. Comment diet for the C++ side.** Step 6 of
  `docs/neon-shader-cleanup-plan.md` was scoped to `neon.frag`. The same method
  - keep the invariant, the CPU mirror and the warning at each line; move the
  measured history verbatim to a notes document - applies to:

  | file | lines | comment lines |
  | ---- | ----: | ------------: |
  | `lib/src/renderer/neon-renderer.cpp` | 4,453 | 2,106 (47%) |
  | `lib/include/renderer/neon-renderer.h` | 920 | 677 (74%) |
  | `lib/include/renderer/neon-tuning.h` | 655 | 585 (89%) |

  This is the largest cleanup by line count. `neon.frag` went 1,746 -> 1,154
  that way with no code change.

- **C8. The 1.0-only machinery - not recommended.** If 1.0 were not shipped,
  the field at 1.0 could go: pass 1f / 1c at 1.0 (`Render`'s
  `else if (bakeField && glowReady)` branch, `renderFieldCompositePass`'s
  direct branch, `GetFieldRegion`'s 1.0 branch), `MasksAfterGrade`, and the
  scale terms in `IsFieldEligible`. But 1.0 is `resolutionScale`'s default,
  it would lose 3.3x on hue frames and 6.3x on still ones (I46), and section 3.4
  says the production band itself is fastest at 1.0. Keep it.

- **C9. Pass 1c below 1.0 and, with it, the reduced buffer for field
  configs** - only if P4 (section 5) lands and holds its error budget.

### 4.3 Not cleanup, whatever they look like

The RGBA8 and half-float fallbacks (`GATHER_FORMATS`, the coverage table's
format walk, `LazyBake::unavailable`) - a GLES 3.0 target without
`EXT_color_buffer_half_float` takes them. The two `neon.frag` program objects
for one source (`mNeonShadeShader`, `mNeonRingShader`: 93-624x on the AMD
driver merged). `invariant gl_Position` in `neon.vert`, the shared blit / ring
vertex floats and the one full-res matrix (`partition`). The per-path lazy
compile. Each is in `CLAUDE.md` with its reason.

## 5. The plan, ranked

### Step 0. Measure on the target, before any code

Three facts reorder everything below, and none can be read off this machine:

1. **Which frames production draws.** Still, hue rotating, or a config
   animation every frame - and which animations, and whether a segment is lit
   most of the time. Section 3.1 is a 2-10x spread by frame type.
2. **Whether the target renders to half float** (`EXT_color_buffer_half_float`
   or `EXT_color_buffer_float` on GLES 3.0). Both fields are R16F with no 8-bit
   fallback: without it neither I50 nor I52 engages, every hue and still frame
   below 1.0 re-shades pass 1b and the ring, and the ranking changes.
3. **Per-pass GPU time there.** The repo only builds on macOS, so the
   attribution probe of section 2 has to be ported, or the passes timed with
   `GL_TIME_ELAPSED` in a debug build of the host. Pay attention to the blit:
   1.6 M full-resolution fragments for a 960 x 540 rect at 1080p is 0.026 ms
   here and a fill-rate-bound GPU's largest single cost.

A tooling change belongs here too. **T1: `neon-scale-check time --flush`**, a
`glFlush` after each frame, so the tool's CPU share is a host's (section 1).

### Step 1. Choose the scale per config - no code

- **Full-size rects: 0.25-0.35, not 0.5.** 1.20-1.39x on every frame whose
  config changes, nothing either way on still and hue frames, one level of
  quality (`check` 2/255 at 0.35, 3/255 at 0.25). Run `check` on the
  production configs at the chosen scale.
- **Thin bands: 1.0** until P3 lands or the target says otherwise (3.4).

### Step 2. Exact items

**P1. Skip the gather when its inputs did not move (keyed gather).** Give pass
1a its own key - the emission table's inputs (`EmissionInputs` plus the bake
time), the shape, `numSamples`, `resolutionScale` and the gather region - and
skip it, like `mOffscreenCurrent` but narrower, when the key holds. The region
is what parked it: make it stop following the glow quad's margin by letting it
only grow until the shape changes (or snapping its box outward to a few gather
texels), so a pulse settles on one region after its first cycle. Its texels are
on the viewport-anchored grid, so a larger region holds the same values.
Expected: the gather's ~0.05 ms off every intensity, bloom, glow-radius,
glow-side and cutoff animation frame, at every reduced scale - ~1.35x on an
intensity frame's GPU time at 0.5, ~1.4x of a 0.25 frame. Byte-identical.
Nothing for arc and segment animations, which move the emission table.

**P2. Re-bake only round an arc's moving end.** When an arc's start and
intensity are unchanged and only its length moved, the coverage changed only
between the old and new ends (plus `HEAD_FEATHER_PX`); dirty the pieces that
interval reaches rather than both whole supports. Exact by the bake's own
invariant (each piece's texels integrate the lights over that piece alone),
and `docs/neon-perf-plan.md` item 5 already lists it. Expected: of the 0.044
ms whole-table bake per arc-wipe frame, one to three pieces of eight. The same
for a segment whose position alone moved is P5.

**P3. The edge ring's field for a one-sided glow and cutoffs.** The ring
applies the cut and the cutoffs after its tone map, which is why I52 refuses
them; but both are analytic functions of the pixel's position, and
`neon-blit.frag` already evaluates exactly them at full resolution. Evaluate
the same terms in the ring composite (`NEON_FIELD_RING`) after the tone map,
and `IsRingFieldEligible` drops its glow-side and cutoff conditions. Expected
for the production band at 0.5: the ring's 0.047 ms of a 0.061 ms still frame
down to its composite (~0.003-0.013 ms on the other configs), which would put
0.5 ahead of 1.0 on the band's still frames for the first time. Within 1/255 of the ring shaded
directly, expected exact where the mask is 0 or 1; verify with `check`
(`glow_inside`, `card_outside`, `bounded_band`) and `partition`.

### Step 3. Prototypes - not exact, decide on their numbers

**P4. Fuse the field composite into the blit.** On a field frame below 1.0,
pass 1c only writes `tonemap(gather * field)` into the reduced buffer for the
blit to read. The blit can read the field and the gather itself: one offscreen
render pass and one reduced-buffer write fewer on every hue frame, and the
reduced buffer not allocated at all for a field config (C9). Not exact:
bilinear before the tone map instead of after, which outside the ring (the
smooth part of the glow) should stay under a level - measure it. Small on the
M2 (a hue frame is 0.042 ms of GPU); on a tiler every render pass skipped
saves a tile store, so decide on the target's numbers.

**P5. The coverage bake under a travelling segment.** 0.092 ms per frame, the
largest pass after the shading at 0.25. Item 5's two follow-ups: cut the bell
where the emission pass already treats it as gone, nearer than `5 / sqrt(2)`
of its sigma (not exact), and the segment counterpart of P2.

**P6. A cheaper gather.** The loop is `numSamples` (128) fetches per texel, two
with a segment. `docs/neon-perf-review.md` section 8 has the measured shape of
`numSamples`, and section 6 the two-level gather. A quality trade either way.

### Step 4. Owner decisions

- **D1. Intensity as a multiplier after the field.** The field cannot survive
  an intensity change today because intensity is not just a factor: it moves
  the bloom's `reach` (`1 + bloomStrength * intensity`), so the pedestals and
  the quad's fade with it. Size `reach` from something an intensity animation
  does not move - the config's peak intensity, say - and the field becomes
  `Fa * intensity`: an intensity pulse turns into a composite frame, ~0.18 ->
  ~0.04 ms of GPU at 0.5 by the hue frame's cost. A look change wherever
  intensity is not that reference value; needs a visual sign-off.
- **D2. Pin thin bands to 1.0** (`docs/neon-perf-plan.md` item 11) - or P3,
  then re-measure 3.4.
- **D3. A field for segments** (cleanup plan step 5, declined for its memory;
  **built 2026-10-08 as I58** - RG16F only on segment configs, 2.09x on their
  hue frames at 0.5). Was: revisit only if production keeps a segment lit: the `segments` scene pays
  2.0x (0.25) to 4.3x (0.75) the default's hue frame.

### Measured and set aside

- **Unbinding textures before each framebuffer bind**, to stop the mid-frame
  submits: no change. A host's per-frame swap removes them anyway.
- **Resolving uniform locations once** (item 13, first bullet): a few percent
  of `Render`'s CPU on the M2. Revisit only on a target CPU profile.
- **The emission table on hue frames**: 256 fragments; its cost in
  `time`'s figures is the mid-frame submit that lands there, not the pass.

## 6. Suggested order

1. Step 0 and T1. Step 1 on the production configs.
2. C1-C6 - behaviour-neutral, and they shrink `Render` before P1, P3 and P4
   change it. C7 if the owner wants it, last, as for `neon.frag`.
3. P1 and P2 - exact, and independent of each other.
4. P3 - the production band.
5. P4 and P5 as prototypes, kept on the target's numbers.
6. D1-D3 as the owner decides.

## 7. Verification, every step

- `neon-scale-check check` and `partition` (two seeds) after every step;
  `check`'s 1.0 column reads 1 at `b0ac8cf`.
- Exact items (C1-C6, P1, P2): a long-lived effect walked through scales,
  configs and viewport changes, every frame byte-identical to `b0ac8cf`
  (the walk of `docs/neon-perf-plan.md` section 2); for P2, the coverage
  table read back texel for texel against a fresh effect's full bake.
- `neon-scale-check time` in `still` AND every animated mode, interleaved
  against a frozen `b0ac8cf` binary, median of three - a change that saves
  work on one frame type must be shown to cost the others nothing.
- `neon-guide-figures`, and diff `docs/images/neon-onboarding/`, after
  anything that moves a pixel.

## 8. Status

| item | state |
| ---- | ----- |
| C1 | **done**: `NeonRenderer::LazyBake` (`Decide`, `Invalidate`, `OnBaked`), one instance per field (`mField`, `mRingField`); ten members and two hand-written schedules gone |
| C2 | **done**: `IsShadingFactorable`, asked by `IsFieldEligible` and `IsRingFieldEligible` |
| C3 | **done**: `bindScaledTarget`, called by `renderNeonPass` and `renderFieldCompositePass` |
| C4 | **set aside**: `renderEmissionPass` and `renderGlowCoverPass` capture their own target on purpose - a pass restores what IT finds, which keeps it correct wherever it is called from (the note in `Render`) - and the two `glGetIntegerv` each did not show in the CPU profile |
| C5 | **done**: the class comment (the gather is `neon-gather.frag`'s), `setupRingGeometry` (builds the gather quad and both region boxes at every scale), `resizeGatherBuffer`, the pass-order note, `renderBlitPass`, `mGatherVertexArray`, and the same "the gather IS the composite" wording in `renderNeonPass` |
| C6 | **done**: `PassKind::P1` and its classification branch |
| C7, C9 | open - the owner's call |
| C8 | **done** (I57), the owner's call made: the direct path is gone, `resolutionScale` defaults to 0.5, and 1.0 takes the one path with a full-size buffer (within 1 level of the old 1.0 output); every frame below 1.0 byte-identical to the build before |
| P3 | **done** (I53): the ring field's composite recomputes the cut and cutoffs; the production band's still frame 0.051 -> 0.029 ms at 0.5 and 0.054 -> 0.035 at 0.25 (faster than 1.0's ~0.055), masked check scenes 1.2-2.7x still, within 1 level |
| P2 | **done** (I54): an arc that only moved re-bakes the bands its ends swept; texel-exact over 6,706 random edits; arc-wipe frames 1.10x at 0.5, 1.15x at 0.25 (M2), up to 1.74x with segments |
| P1 | **done** (I55): the gather skipped while its inputs hold and the quad fits the last one drawn; intensity-pulse frames 1.31x at 0.5, 1.43x at 0.25 (M2); within 1 level, not byte-identical as section 5 expected - a held region reaches the same texels through another projection |
| D1 | **measured, the owner's call**: `IntensityPulse` swings 0.4-1.0 by default, not +/-10%. With the bloom's `reach` held at the pulse's peak (1.0) - neon.frag's shader term alone, the CPU's quad margin untouched - the image moves by at most 2 levels on the default look (1 from intensity 0.85 up), 1 on `soft_wash` and `hairline`, and 6 on `overdrive` (bloom 1.5) at intensity 0.4 (3 at 0.7, 2 at 0.85), at 0.5 and 0.25 alike. Expected gain if built: an intensity frame at 0.5 becomes the field's composite and the ring field's instead of pass 1b and the ring, ~3x |
| D1 | **done** (I56), accepted by the owner: intensity-only changes keep both fields, scaled while the intensity moves, exact once it holds; intensity-pulse frames 2.31x at 0.5 and 1.60x at 0.25 (GPU 0.193 -> 0.049 ms at 0.5) |

Verified against `b0ac8cf` on the Apple M2 Pro: a 261-frame walk over three
long-lived effects (first frames at 1.0, at 0.5 under a rotating hue and at
0.25 still; settle, intensity, viewport, cross-fade, translucent-stop,
one-sided, cutoff, segment, arc, thin-band, moving-rect, disable / re-enable
phases) byte-identical frame for frame, and the pass each draw belonged to
identical too, so the fields bake and composite on the same frames;
`check` reads the same table; `partition` passes on seeds 1 and 7; the 65
guide figures byte-identical. Not timed: the change is CPU bookkeeping on the
same calls.

The committed guide figures (`docs/images/neon-onboarding/`, last regenerated
at `9f5db99`) no longer match what `b0ac8cf` renders on this machine - 60 of
65 differ, before and after this change alike. They predate I50-I52; regenerate
them separately.

**Owner, 2026-10-07: scale 1.0 is never used in production.** What that
changes here:

- Nothing below 1.0 gets faster by deleting the 1.0 path. Its programs and
  buffers are built per path on first use (`ensurePathPrograms`), so a host
  that stays below 1.0 never compiles, allocates or runs any of it; and the
  `uResolutionScale < 1.0` branches in `neon.frag` are not 1.0-only - the edge
  ring shades with scale-1.0 uniforms below 1.0 too. C8 becomes a cleanup
  option (the direct branches in `Render`, `renderNeonPass` /
  `renderFieldCompositePass`'s `scaled` flag, `MasksAfterGrade`, the 1.0
  branches of `GetFieldRegion`, `setupRingGeometry` and `ensurePathPrograms`),
  with two costs to weigh: 1.0 is `resolutionScale`'s default for every other
  host, and `check` measures every reduced scale against the 1.0 render.
- D2 is withdrawn: a thin band can no longer fall back to 1.0, so P3 is the
  way to make the production band cheaper below 1.0, and moves first.
- The order for the speed work becomes P3, P1, P2, then D1 if intensity pulses
  are common, then P4 and P5 on the target's numbers.

## 9. What is left (after I53-I57)

Re-measured 2026-10-07 at `509c5ea` on the Apple M2 Pro, Release build,
1920 x 1080. GPU time per frame by `GL_TIME_ELAPSED` with a `glFlush` per frame,
the neon alone, the `demo` rect (960 x 540), median of three rounds:

| frame | 0.5 | 0.25 |
| ----- | --: | ---: |
| still | 0.033 | 0.030 |
| hue rotating | 0.062 | 0.054 |
| intensity pulse (+/-10%) | 0.047 | 0.045 |
| arc wipe | 0.290 | 0.154 |
| segment travel | 0.555 | 0.327 |

Still, hue and intensity frames are now within 2x of each other. What is left
is arc and segment animation, segments in general, the blit's fixed cost - and,
outside the neon, the lens flare. Ranked by what they could take off a frame:

1. **The lens flare's `resolutionScale` (default 1.0).** On the production
   band, the flare alone: 0.70 ms of GPU at 1.0, 0.18-0.20 at 0.5, 0.02-0.04 at
   0.25 - 15-30x the neon's whole frame at 0.5. If production draws the flare,
   this is the largest single lever for frame rate, and it needs no code: set
   the scale and check the look (`docs/neon-unification-plan.md` records that the
   flare's scales were verified against 1.0 when its fork was merged).
   Then the hex sprite gating left open in `docs/lens-flare-perf-review.md`
   section 7 (~1.7x on that layer).
2. **Step 0 and T1 are still open.** Nothing here was measured on the target:
   which frames production draws, whether it renders half float (both fields
   are R16F only - without it every hue and still frame re-shades pass 1b and
   the ring), and per-pass GPU time there. T1 (`neon-scale-check time --flush`)
   would end the harness noise that made several A/B runs in I55-I57 need a
   separate GPU-timer probe.
3. **Arc-wipe frames: pass 1b is half the frame.** 0.19 ms of 0.36 (wall) at
   0.5 - shaded directly, since an arc change moves the coverage `Fa` reads, so
   no field holds. Levers: a lower scale (pass 1b 0.077 at 0.25), or re-shading
   only the region the dirty coverage pieces reach (I54 already knows which) -
   NOT exact, because the gathered colour is a long-range mean that moves
   everywhere a little; measure it before building it.
4. **Segments.** A config with a segment takes neither field, so its hue frames
   shade pass 1b (0.19 ms at 0.5) and the ring every frame, and a travelling
   segment adds the coverage bake (0.10) and a two-attachment gather (0.10):
   - **4a. Gather only the segment attachment when only segments moved.**
     Exact: the emission table's row 0 (base colour, arcs) does not read the
     segments and attachment 0 of the gather reads only row 0, so on a
     segment-travel frame with the hue still, attachment 0 is redrawn
     unchanged. About half the gather's 0.10 ms.
   - **4b. P5**, the coverage bake's bell cut nearer than 5 sigma (not exact).
   - **4c. D3**, a field for segments - declined for its memory; revisit if
     production keeps a segment lit.
5. **The blit's fixed full-resolution cost.** 0.039 of a 0.057 ms still frame
   (wall) at 0.5: 1.64 M fragments for the `demo` rect, every frame, whatever
   moved. A fill-rate-bound target will feel it most. Measure first how many of
   those fragments write under half a level (a tighter quad would be exact
   there); P4, folding the field composite into the blit, removes one
   offscreen pass on hue frames, and C9 the reduced buffer for field configs.
6. **Cleanup and docs.** C7 (comment diet for `neon-renderer.cpp`, `.h` and
   `neon-tuning.h`). `docs/neon-onboarding-guide.md` predates I44-I49 beyond
   the passes refreshed for I57, and `docs/neon-perf-plan.md` section 9 does not
   list I47, I49, I51 or I52.

C8 in section 4.2 was "not recommended" when 1.0 was the default; the owner
has since made it (I57, section 8).

**Status, 2026-10-08** (AMD Radeon Pro 5300M; detail in
[`neon-animation-perf-analysis.md`](neon-animation-perf-analysis.md), the
day's changes in [`progress-log.md`](progress-log.md)):

- Item 4 (segments) and arc animation: two exact changes built - the
  filament's pointwise inputs skipped off the line (`filamentLit`; P1b no
  longer grows with the light count) and the coverage bake re-integrating only
  the light type that changed (arcs over still segments 1.42x). 4b (P5, the
  bell cut nearer than 5 sigma) was prototyped with an 8-node rule (8 segments
  moving 1.38x, <= 2 levels on ~3% of pixels) and not taken; 4c (D3) is
  re-proposed with numbers - segment configs pay ~1.5 ms more on every
  rotating-hue frame.
- Item 5 (the blit), measured: 96% of its 1.64M fragments are visible, and
  under 2 levels only ~11% on the default frame, so a tighter quad buys little
  exactly. P4 is ruled out on this GPU: the composite would run on 4x the
  fragments.
- Item 6: the comment work on `neon-renderer.h` / `.cpp` is done as Doxygen
  for every type, function, member and helper rather than a cut (C7 itself,
  the diet, is still the owner's call).
