# Neon: render time and memory plan

What is left to win in the neon layer after the scale-1.0 gather split
(`cdcc19c`), where the frame's time and memory go now, and a ranked plan for
taking it. It answers question 6 of
[`neon-perf-review.md`](neon-perf-review.md) section 10.8 ("what is next for
speed?") with measurements rather than a list of candidates, and adds memory,
which no earlier review covered at scale 1.0.

Everything here was measured on 2026-10-06 against `0cec9f4`, on an **Apple M2
Pro** (arm64, macOS GL 4.1). This is the first M2 run since the split landed.
Section 10 of the perf review is the AMD Radeon Pro 5300M's view of the same
tree; where this document leans on an AMD number it says so. Read ratios, not
milliseconds, and expect the target device to differ again.

**Result.** At scale 1.0 the shading pass is now ~90% of a neon frame, and the
gather is cheap. The two largest costs left are not in the shading at all.
The first is **work repeated on frames where nothing changed**: skipping the
offscreen passes then was prototyped byte-identical and renders still frames
**2.0x-6.0x faster at scale 0.5** and 1.15x-1.66x at 1.0. The second is the
**glow coverage table re-baking in full every animated frame**: ~0.15 ms with
one travelling segment, which more than doubles the production band's frame.
Re-baking only the pieces that changed is exact and measured at up to 1.91x on
those frames. A partly lit ring costs +23-31% at 1.0, most of it the per-piece
glow fix. Memory is small at 1.0 (0.10 MB) until a ring is partly lit (+1.0 MB
coverage table), and below 1.0 the production band allocates a viewport-sized
buffer for a 24 px frame of light while rendering slower than at 1.0.

## 1. Where it stands

- **The split holds on the M2, and gains more than on the AMD.** Still frames
  at scale 1.0, `80d71a7` (the commit before `cdcc19c`) against `0cec9f4`, two
  interleaved rounds: `demo` 3.85 -> 0.61 ms (6.33x), `card` 2.81 -> 0.51 ms
  (5.54x), `band` 0.404 -> 0.096 ms (4.22x). The AMD measured 2.30x-4.04x on
  comparable scenes (perf review 10.5).
- **`neon-scale-check check` passes on the M2, at its limit.** The 1.0 column
  reads 2 on `hairline` and `card_outside` against a bound of 2; every other
  scene reads 1. The split already spends 1/255 (perf review 10.1), and
  GPU-to-GPU variance spends the other. Any further change that moves a pixel
  by 1/255 at scale 1.0 fails `check` here until the comparison page's 1.0
  images are regenerated (perf review 10.8, question 3). Items 8 and 9 below
  cannot be verified before that.
- **The offscreen pass's fixed cost is the AMD's, not the design's.** The AMD
  charges ~0.15 ms for the first offscreen pass of a frame, which is what
  `FULL_RES_SPLIT_MIN_AREA_PX` was calibrated against. On the M2 the same
  charge measures 6-8 us (section 3.2). Apple's GPUs are tile-based, so this is
  also the first data point from a tiler - though not the target's.
- **The timing tool only times still frames.** `neon-scale-check time` calls
  `Render` repeatedly with nothing changing between calls, so it cannot see the
  glow coverage bake, the emission table re-bake, or any cache that skips work
  on unchanged frames. Three of the items below are invisible to it. (Since
  fixed: `--mode`, item 2.)

## 2. Method

**Build.** A Release build of the library in its own build directory. Do not
time a Debug build cache: `CMakeLists.txt` defaults to Release only when no
build type is set, and an existing Debug cache keeps Debug.

**Timing probe.** A throwaway program linked against `libedge-lighting.a`,
the shape described in [`neon-perf-review.md`](neon-perf-review.md) section 7:
a hidden GLFW window, an `OffscreenCapture` at 1920 x 1080, a freshly
initialised effect per scenario, 10 warm-up frames, then the minimum over 5
runs of the mean of 40 frames between `glFinish` calls. Each figure is the
median over interleaved rounds - three for the baseline tables, two per
variant (four runs each) for the prototype comparisons.

Three rect shapes, centred in the frame, everything else at its default with
`colorTransitionDuration` 0 and the hue still unless a column says otherwise:

| shape | rect | notes |
| ----- | ---- | ----- |
| `demo` | 960 x 540 | the demo's rect; its glow reaches its middle, so no interior hole |
| `card` | 640 x 360 | the check scenes' base rect |
| `band` | 1840 x 1000 | the production target of perf review 10.2: `GlowSide::OUTSIDE`, outside cutoff `{true, 20, 4}` |

and these frame modes:

| mode | what changes each frame |
| ---- | ----------------------- |
| still | nothing; `Render` only |
| hue rotating | `hueRotationRate` 0.5, `Update(1/60)` every frame |
| intensity pulse | `intensity` = 1 + 0.1 sin(0.1 f), through `SetConfig` + `Update` |
| arc still / arc wipe | one arc of length 0.5; the wipe sweeps its length over 0.3-0.9 |
| segment still / travelling | one `SegmentBoost` (length 0.1, boost 1); travelling moves it 0.003 of the perimeter a frame |

The animated modes run `SetConfig` and `Update` inside the timed loop, so
their figures include the library's CPU path (1-2 us, section 3.6).

**Memory.** The probe swaps glad's `glad_glTexImage2D` and
`glad_glDeleteTextures` pointers for wrappers that record each texture's size
and format against the bound name, so it counts the live texture bytes of
every pass without a hook in the library. Process memory is
`task_info(TASK_VM_INFO).phys_footprint`.

**Attribution.** The probe swaps `glad_glShaderSource` for a wrapper that
applies string replacements to `neon.frag` before it compiles, so a term can be
stubbed out at run time and timed against the unpatched program in one binary.

**Prototypes.** Built in a scratch worktree at `0cec9f4`, switched by
environment variables so both sides ran from one library. Verified
byte-identical against the unmodified library over a fixed walk: one long-lived
effect through eight configs (scales 1.0, 0.5 and 0.25, an intensity change,
the band, a partly lit ring, hue rotation, a segment, a fractional move), four
frames held at each plus one at a different viewport width - 40 frames, every
byte equal. None of the probes or prototypes is checked in.

## 3. Where the time goes

### 3.1 The frame by mode

ms per frame, neon only, 1920 x 1080:

| scene | still | hue rotating | intensity pulse | arc still | arc wipe | segment still | segment travelling |
| ----- | ----: | -----------: | --------------: | --------: | -------: | ------------: | -----------------: |
| `demo` 1.0 | 0.606 | 0.609 | 0.612 | 0.746 | 0.797 | 0.793 | **0.954** |
| `demo` 0.5 | 0.291 | 0.295 | 0.291 | 0.335 | 0.386 | 0.382 | **0.547** |
| `card` 1.0 | 0.519 | 0.533 | 0.537 | 0.630 | 0.679 | 0.678 | **0.844** |
| `card` 0.5 | 0.263 | 0.265 | 0.264 | 0.308 | 0.355 | 0.355 | **0.513** |
| `band` 1.0 | 0.095 | 0.107 | 0.107 | 0.106 | 0.156 | 0.123 | **0.283** |
| `band` 0.5 | 0.108 | 0.138 | 0.138 | 0.116 | 0.182 | 0.132 | **0.295** |

`neon-scale-check time` on the same build, still frames, for the record
(its own layout, scaled to the frame):

| scene | 1.0 | 0.5 | 0.25 | 0.125 |
| ----- | --: | --: | ---: | ----: |
| `default` | 0.619 | 0.298 | 0.195 | 0.194 |
| `soft_wash` | 1.041 | 0.454 | 0.281 | 0.255 |
| `bounded_band` | 0.231 | 0.126 | 0.125 | 0.117 |
| `arcs` | 0.854 | 0.376 | 0.238 | 0.239 |
| `segments` | 0.812 | 0.393 | 0.263 | 0.270 |

(The full twelve-scene run at 1280 x 720 and 1920 x 1080 reads the same way:
below 0.35 every scene sits on a floor of ~0.11-0.27 ms, the passes' fixed
cost, and 0.125 buys nothing over 0.25.)

### 3.2 The gather and the offscreen pass

At `numSamples` 1 the gather is free, and the hue rate decides the path - at
rate 0 the area gate keeps the loop inline, at any other rate the gather splits
out - so the pair isolates what leaving the caller's framebuffer costs:

| scene, 1.0 | inline, no offscreen pass | split, one offscreen pass | difference |
| ---------- | ------------------------: | ------------------------: | ---------: |
| `demo` | 0.589 | 0.555 | -34 us |
| `card` | 0.457 | 0.437 | -20 us |
| `band` | 0.066 | 0.072 | +6 us |

The split is cheaper on two of three even at one sample - reading the gather
buffer costs less than a one-sample loop - and costs 6 us on the third. The
AMD's ~150 us does not exist here. The 128-sample gather itself (still frame
less the one-sample split) is 23 us on the band, 51 us on `demo` and 82 us on
`card`.

### 3.3 The shading pass

With the loop off the quad, everything else in `neon.frag` is the cost: ~0.55
ms of `demo`'s 0.61. Its attribution has not been re-run on the M2; on the AMD
(perf review 10.2) the four corner pieces are ~40% of it and the straights'
bloom ~25%, and an earlier M2 measurement (before the split) also put the
corner arcs at ~40% of everything outside the gather.

### 3.4 The partly lit premium

A ring that is not lit all the way round reads the glow coverage table: each
of the outline's eight pieces scales its halo and bloom by its own coverage
(`addStraightGlowFix`, `addCornerGlowFix`). A uniformly lit ring skips all of
that. The premium, and what it is made of, by stubbing `neon.frag` at run time:

| scene | uniform ring | as is | table fetch stubbed | whole glow fix stubbed |
| ----- | -----------: | ----: | ------------------: | ---------------------: |
| `demo` 1.0, arc | 0.606 | 0.746 | 0.689 (1.08x) | 0.629 (1.19x) |
| `demo` 1.0, segment | 0.606 | 0.796 | 0.738 (1.08x) | 0.679 (1.17x) |
| `card` 1.0, arc | 0.519 | 0.630 | 0.577 (1.09x) | 0.523 (1.20x) |
| `demo` 0.5, arc | 0.291 | 0.332 | 0.311 (1.07x) | 0.294 (1.13x) |
| `band` 1.0, arc | 0.095 | 0.106 | 0.100 (1.06x) | 0.096 (1.10x) |

"Table fetch stubbed" replaces the `textureLod` in `glowCoverAt` with a
constant; "whole glow fix stubbed" forces `uniformCover` true. On `demo` at
1.0 the premium is 0.140 ms, and the glow fix is 0.117 of it (84%): the fetch
~40%, the arithmetic around it (each piece's development, its forward map onto
the table, the mix) the rest. The arc loop of the filament and the second
gather read are the remaining ~16%.

### 3.5 Animated frames

Two costs appear only when the config changes:

- **The glow coverage table re-bakes in full** whenever an arc, a segment, the
  rect's shape or `glowRadius` moves (`mGlowCoverDirty`, I34). Travelling
  segment less segment still: **0.158-0.166 ms on every scene at both scales**,
  since the table is a fixed 1024 x 128. An arc wipe: 0.047-0.066 ms (arcs are
  closed forms; segments are a 16-point Gauss-Legendre per texel). On the band
  at 1.0 that is 0.283 ms against 0.123 still - 2.3x the frame.
- **The emission table re-bakes** on every config change at all
  (`mEmissionDirty`, set unconditionally in `OnConfigChanged`), and every frame
  while the hue rotates. On the M2 the intensity pulse costs +0.000-0.018 ms on
  `demo` and `card` and +0.012 / +0.030 ms on the band at 1.0 / 0.5, quad
  rebuilds and the config path included. On the
  AMD it is the inline path's only offscreen pass under an animation, so the
  ~0.15 ms charge of section 3.2 applies to it every frame (inferred from perf
  review 10.1, not measured).

### 3.6 CPU

Thread CPU time per frame, `demo`:

| mode | config path (`SetConfig` + `Update`) | `Render` submission |
| ---- | -----------------------------------: | ------------------: |
| still, 1.0 | 0.1 us | 20 us |
| hue rotating, 1.0 | 0.4 us | 44 us |
| intensity pulse, 1.0 | 1.3 us | 42 us |
| `glowRadius` pulse, 1.0 | 1.5 us | 45 us |
| segment travelling, 1.0 | 0.7 us | 56 us |
| still, 0.5 | 0.1 us | 48 us |
| intensity pulse, 0.5 | 2.1 us | 58 us |

Nothing here matters on this CPU. On an embedded one the submission column is
the one to measure (item 13).

## 4. Where the memory goes

Live texture bytes the neon holds after three frames, 1920 x 1080 (the
1920 x 1080 capture target excluded), with what makes them up:

| config | MB | textures |
| ------ | -: | -------- |
| `demo` 1.0, uniform ring | 0.10 | gather 128 x 96 RGBA16F; LUTs (256 x 1, two 128 x 8); emission table 128 x 2 RGBA16F |
| `demo` 1.0, arc 0.5 | 1.10 | + glow coverage table 1024 x 128 RGBA16F |
| `demo` 1.0, arc 0.5 + segment | 1.20 | + second gather attachment 128 x 96 RGBA16F |
| `demo` 0.5 | 1.79 | reduced buffer 816 x 540 RGBA8 |
| `demo` 0.25 | 0.53 | reduced buffer 416 x 270 RGBA8 |
| `band` 1.0 | 0.09 | gather 128 x 80 RGBA16F |
| `band` 0.5 | 2.07 | reduced buffer **960 x 540** - the whole frame - for a lit frame 24 px wide |
| 160 x 96 rect, 1.0 | 0.01 | LUTs and emission table; inline, no gather buffer |
| 160 x 96 rect, 1.0, segment | 1.01 | + glow coverage table, ~100x everything else |

Two things stand out. The coverage table is 1.0 MB whatever the rect, and it
is most of what a partly lit ring holds at 1.0. And the band at 0.5 holds a
viewport-sized buffer: the blit's box spans the frame, so the region cap in
`GetBufferRegion` lands on the full reduced viewport, while what is lit is a
thin frame round its edge - ~7% of those texels, and 7.9 MB at 4K.

Process memory rose by 11.7 MB across the first frames of the first effect, the
frames that compile the path's programs. Most of that is the GL compiler's own
first-use cost; later effects in the same process added 0-3.7 MB, the most
when a partly lit ring compiled the coverage bake and allocated its table. It
cannot be attributed to the library's objects with this method.

## 5. The plan

Numbered as in the review that produced it. Items 4-7 change no pixel; 8 and 9
move some by 1/255 and wait on item 1.

### 5.1 Measurement first

**1. Regenerate the comparison page's 1.0 images.** *Done on the Apple M2 Pro,
after A (I46), so the images are the output that ships: `check`'s 1.0 column
reads 0 on all twelve scenes there. The page is now timed with the hue
rotating (`generate --mode hue`): a still frame reuses its passes at every
scale, and with the field 1.0 is the cheapest scale on every scene it serves.* The four steps in
[`tools/neon-scale-check/README.md`](../tools/neon-scale-check/README.md).
Until then `check` has no headroom on the M2 (section 1), so nothing that moves
a pixel at 1.0 can be verified.

**2. Teach `neon-scale-check time` animated modes.** *Done - section 9.* Hue rotating, an
intensity pulse, an arc wipe and a travelling segment, as in section 2 - a
`--mode` option, `SetConfig` + `Update` inside the timed loop. Items 4, 5 and
6 are invisible to the tool as it stands, and every one of them should land
with a before / after from it.

**3. Run it on the target device.** Perf review 10.8, question 1. The two GPUs
measured so far disagree by 25x on the one constant the split's gate rests on.

### 5.2 Render time, exact

**4. Skip the offscreen passes on frames where nothing moved.** *Step 1
done - section 9, I40.*

The gather (pass 1a) is a pure function of the emission table, the loop
samples and its region; pass 1b (the reduced-resolution shading) of those plus
the config and, only through the hue rotation, the time. Both buffers persist
between frames and nothing else writes them, so on a frame with no config
change, no cross-fade tick, the same viewport and - at a non-zero hue rate -
the same time, both passes would write what is already there.

Prototyped as a flag in `NeonRenderer`: cleared in `OnConfigChanged`, set at
the end of a successful offscreen phase, and honoured only when
`isEmissionTableStale` is false and the viewport matches the one recorded with
it. A frame that reuses skips pass 1a, pass 1b and the `RenderTargetState`
capture and restore around them; the regions and their UV maps are still
computed, and everything on the caller's framebuffer still draws. Byte-identical
over the section 2 walk. Still frames, ms (before -> after):

| scene | uniform ring | arc still | segment still |
| ----- | -----------: | --------: | ------------: |
| `demo` 1.0 | 0.612 -> 0.614 (1.00x) | 0.772 -> 0.697 (1.11x) | 0.798 -> 0.694 (1.15x) |
| `card` 1.0 | 0.508 -> 0.439 (1.16x) | 0.646 -> 0.584 (1.11x) | 0.679 -> 0.561 (1.21x) |
| `band` 1.0 | 0.095 -> 0.062 (1.53x) | 0.106 -> 0.072 (1.47x) | 0.123 -> 0.074 (1.66x) |
| `demo` 0.5 | 0.302 -> 0.070 (**4.30x**) | 0.335 -> 0.076 (4.41x) | 0.380 -> 0.078 (4.86x) |
| `card` 0.5 | 0.267 -> 0.057 (4.68x) | 0.319 -> 0.067 (4.74x) | 0.400 -> 0.067 (**5.98x**) |
| `band` 0.5 | 0.110 -> 0.054 (2.05x) | 0.115 -> 0.062 (1.87x) | 0.132 -> 0.063 (2.10x) |

Animated frames, which cannot reuse, measured 0.96x-1.08x - the session's drift
on an unchanged code path. A still frame below 1.0 then costs the blit and the
edge ring and nothing else, which is ~0.06-0.08 ms here: the scaled path stops
being "not a guaranteed saving" (I25) on any still frame.

`demo` at 1.0 does not move, although its gather is 51 us (section 3.2). The
likeliest reading is that the M2 already overlaps that gather with the previous
frame's shading, so throughput hides it; it is not established. Do not
extrapolate that row to another GPU. (The landed version reads 1.12x on the
same 960 x 540 rect - section 9 - so the row is suspect either way.)

Steps:

1. **The wide version, as prototyped.** One member flag; `OnConfigChanged`,
   `Update`'s cross-fade tick (through `mEmissionDirty`) and a viewport change
   invalidate it. Keep the two-phase schedule as it is: a reusing frame simply
   has an empty offscreen phase.
2. **A keyed gather.** Give pass 1a its own key - an emission-bake counter, a
   loop-samples counter, the gather region's origin and texel count, the gather
   scale - so it survives config changes that move none of them: an intensity
   or bloom pulse, a colour animation. Needs item 6, or the emission counter
   moves on every change. Pass 1b keeps the wide flag; it reads too much of the
   config to key narrowly.

Do **not** retire `FULL_RES_SPLIT_MIN_AREA_PX` on step 1 alone. (Since set to
0 on the owner's decision, on the M2's measurement - section 10.6.) It would make
still frames free on the AMD, but an animated frame with the cache invalid
would split and pay the 0.15 ms there - the 3.6x-slower case the gate exists
for. After step 2 only animations of the gather's own inputs (the shape, the
arcs and segments through the emission table) pay it; re-measure on the AMD
before deciding.

Verification: the section 2 walk; `check` and `partition --seed 1`; the I34
walk (a long-lived effect against a fresh one through every kind of change,
byte-identical at each step).

**5. Re-bake only the pieces of the glow coverage table that changed.**
*Done - section 9, I42: exact, 1.29x on the band rather than the 1.91x below.*

Each piece's texels integrate coverage over that piece's own extent and
nothing else: `pieceCover` in `neon-glow-cover.frag` clips every arc's
trapezoid and every segment's bell to `[0, len]` of the piece, and normalises
by the kernel's mass over the same span. A segment's bell is already truncated
there at `reach = min(5 * 0.7071 / invSigma, 0.5)` of the perimeter. So a
change whose support misses a piece leaves that piece's texels exactly as they
were, and re-baking only the touched pieces is exact by construction.

Upper bound from the prototype, which scissored the bake to one band of four
(one straight and one corner - about what a segment in the middle of a straight
dirties, since a straight takes most of its band's columns):

| scene | travelling segment | arc wipe |
| ----- | -----------------: | -------: |
| `demo` 1.0 | 0.956 -> 0.813 (1.18x) | 1.01x |
| `card` 1.0 | 0.837 -> 0.697 (1.20x) | 1.04x |
| `band` 1.0 | 0.283 -> 0.149 (**1.91x**) | 1.08x |
| `demo` 0.5 | 0.543 -> 0.402 (1.35x) | 1.07x |
| `card` 0.5 | 0.548 -> 0.407 (1.35x) | 1.07x |
| `band` 0.5 | 0.294 -> 0.171 (1.72x) | 1.04x |

A segment crossing a corner dirties two pieces and costs more than this; an arc
wipe gains little because its bake was cheap already.

How:

- `mGlowCoverDirty` becomes an 8-bit piece mask, still accumulated and still
  cleared only by the bake (I34's reasons hold per piece).
- `OnConfigChanged` computes the mask from the old and new light blocks in
  perimeter units: for each arc or segment that changed, the union of its old
  and new support - an arc's span widened by both full feathers, a segment's
  `position +/- reach` - against each piece's perimeter interval. The intervals
  must follow the bake's own `straightStart` / `cornerStart`, winding included:
  one more CPU mirror, of the same kind as `GetGlowCoverSplit`.
- **Everything dirty** when the shape, the winding or `glowRadius` moves (as
  now); when the brightest contributing arc's intensity changes, since
  `arcsOnPiece` clamps every piece to it (`most`); and when an arc's length
  crosses either of the bake's thresholds (`1e-6` and `1 - 1e-6`), which drop
  it out of `most` or light every piece.
- `renderGlowCoverPass` scissors each dirty piece's rectangle: band `b` is rows
  `[b * GLOW_COVER_ROWS, (b + 1) * GLOW_COVER_ROWS)`, its straight the columns
  left of `2 * GLOW_COVER_OVERHANG + inner`, its corner the rest (corner block
  `b`, as the bake's `signs` and `glowCoverCornerBlock` agree). The scissor is
  the library's own here: take it inside the existing `NoScissorScope` and
  disable it after the draw.

Verification is crisper than a frame diff: after a random walk of arc and
segment changes, read the table back (`CaptureUtil::ReadTexture2D`) and compare
it with a full bake of the same config. They should agree texel for texel.

**6. Re-bake the emission table only when its inputs move.** I2, revisited
with a measurement behind it. *Done, keyed on upload counts rather than on
config fields - section 9, I39.*

`neon-emission.frag` reads `uTime`, `uHueRotationRate`, `uNumSamples`, the
three LUTs and the two light blocks, and nothing else. Today `OnConfigChanged`
sets `mEmissionDirty` on any change, so an intensity, bloom, glow or geometry
animation re-bakes it every frame. Gate it on `hueRotationRate`, `numSamples`,
`segmentsDirty || arcsDirty`, and a re-upload by any of the three LUTs - which
means `GradientRingLUT::Bake` and `SpanAtlasLUT::Bake` reporting whether they
re-uploaded (both return void today). `Update`'s cross-fade tick keeps setting
it as now.

The gain on the M2 is small (section 3.5). On the AMD it removes the frame's
only offscreen pass from every inline-path config under an animation that does
not touch those inputs, and it is the precondition for item 4's step 2. I2
declined this for a real reason - a missed input is a stale ring - so take
I34's discipline with it: a comment at the top of `neon-emission.frag` naming
the gate, and the I34 walk extended to every field the shader does NOT read
(byte-identical to a fresh effect at each step) and every one it does (the
table re-baked).

**7. A complete texture on the segment-gather sampler.** *Done - section 9,
I41.* Without segments
`bindGatherBuffer` binds attachment 1, which does not exist, so unit 4 gets
texture 0. Apple's driver logs "unit 1 GLD_TEXTURE_INDEX_2D is unloadable ...
using zero texture" on every split or scaled run with no segment (seen in
`check` and in every probe run of that kind, absent with a segment; the
driver's unit numbering does not match the GL unit, so confirm the source when
fixing). Bind attachment 0 as the stand-in, as I33 did for unit 5. Not a
measured cost; the same hygiene as I33.

### 5.3 Render time, 1/255 (after item 1)

**8. Trim the partly lit premium.** Section 3.4: up to 20% of a 1.0 frame on a
partly lit ring, ~40% of it the table fetch.

- **Constant pieces.** A piece wholly inside one arc's plateau, or outside
  every arc's and segment's support, has a constant table value (that arc's
  intensity under the `most` clamp, or 0). The CPU can tell, from the same
  intervals item 5 computes, and pass a per-piece mask and constant so those
  pieces skip the fetch. Per-index data goes in a `layout(std140)` block
  through `UniformBuffer` - never a bare uniform array (see `CLAUDE.md`). Not
  exact: the table holds `c / (1 + c)` at half precision, the constant does
  not, so a pixel can move by the rounding.
- **A two-channel table without segments** (`.r` halo and `.g` bloom carry the
  arcs, `.b` / `.a` the segments): half the bytes per fetch, and item 10's
  memory. The read in `glowCoverAt` must then take only `.rg` - a missing
  alpha reads 1, which the `c / (1 - c)` decode turns into 1024.

Expected 3-8% on partly lit rings; unmeasured.

**9. A far-field corner.** The corners are the largest piece of the shading
(section 3.3). Far from its circle relative to `r`, a corner arc's halo and
bloom approach a point source of length `pi r / 2`, which needs no
development and no `atan`. Switch to that past a radius set on the CPU, from
the same `GlowBoundTerms` mirror `GetCornerArcBound` uses, with the budget
`uCornerSkip` has: a quarter of half an 8-bit level per arc. Most of the corner
evaluations in `demo` lie between that radius and `uCornerSkip` (320 px at the
defaults), so the gain could be a large share of the corners' 40%. Unbuilt and
unmeasured. Change the halo or bloom terms and this bound, like the two that
already exist, has to follow.

*Measured and set aside (2026-10-07, AMD Radeon Pro 5300M, after I48): the
premise does not hold.* A CPU mirror of the corner piece against two far
fields - a midpoint rule on the developed segment, and a point of length
`pi r / 2` at the arc's real midpoint (no development, no `atan`) - put the
radius where either stays within `uCornerSkip`'s own budget (a quarter of half
a level per arc) at 314 px against a 315 px skip at the defaults: **0%** of
the default rect's corner evaluations would take it, and the same within a
pixel of the skip for a hairline glow and a 200 px corner radius. The pedestal
leaves the bloom near that budget everywhere it is non-zero, and the far
field's error at 150 px is 12.5 budgets. Only a frame-sized glow (`soft_wash`)
opens a band, mostly off screen. Loosened until it matters - the point
source past 138 px, 16x that budget, 71% of evaluations - it is 1.07x on an
intensity pulse and 1.04x on an arc wipe at 1.0, and moves 3.4% of the
frame's channels by a level (past 85 px: 1.08x / 1.05x, up to 3 levels on
5.4%), with a radius solved numerically for one config, since no closed-form
bound on the point's error is tight enough to place it. Not worth either.

### 5.4 Memory

**10. A smaller glow coverage table.** 1.0 MB, and most of what a partly lit
ring holds at 1.0. *First and third bullets done - section 9, I43.*

- **Two channels without segments** (item 8): 0.5 MB. Reallocate when segments
  appear or disappear, as the gather buffer's attachment count already does.
- **Columns sized to the perimeter**, in power-of-two buckets so a resizing
  rect does not reallocate every frame. A 160 x 96 rect does not need the 1024
  columns a thin line on a 4K rect does (V21: 768 read 6 off there). This
  touches the layout - `GLOW_COVER_WIDTH` and `GLOW_COVER_SHARED` are
  compile-time constants in `neon-pieces.glsl` - so it is the expensive half.
- **Release after the ring has been uniform for a while.** I33 keeps the table
  when the ring turns uniform because an arc animation reaching length 1 does
  that once per loop; a delay of a few seconds keeps that and still frees a
  ring that has settled.
- **RGBA8** only if a run against V21's brute-force reference stays within 2
  levels; it is the existing fallback format and has never been measured for
  quality.

**11. Keep thin bands at 1.0.** The band is slower at 0.5 than at 1.0 (0.108
against 0.095 ms here, 0.41 against 0.23 on the AMD), and allocates 2.07 MB
(7.9 MB at 4K) where 1.0 allocates 0.09. I25 documents that a reduced scale is
not a guaranteed saving; this would act on it. Pin the effective scale to 1.0
when the glow's area at 1.0 is below a threshold, keep the configured value,
and log the transition - exactly the shape of the spotlight's `HasClippedLamp`
/ `GetClampedSpotScale`. A behaviour change to a public knob, so the owner's
call (section 6). The alternative that keeps the scale - a reduced buffer laid
out as four strips - saves the memory but not the time, and is much more work.

**12. The idle path's programs.** A host that visits both paths holds up to
three compiles of `neon.frag` (plain, the shading at reduced scale, the ring)
plus the gather and the blit. Releasing a path's programs after a few seconds
unused frees their driver memory at the price of a recompile on return. The
duplicate shading / ring compile stays: it is the AMD's 93-624x pathology
(`ensurePathPrograms`). Low priority: the memory is the driver's and was not
measurable here (section 4).

### 5.5 CPU

**13. Cheaper submission, if the target's CPU says so.** Two things in
`Render`'s 20-58 us (section 3.6):

- `ShaderProgram::SetUniform(const char *, ...)` looks the name up in an
  `unordered_map<std::string, GLint>`, which builds a `std::string` temporary
  and hashes it, then looks the value up in a second map - about 30 times per
  pass. Resolving each program's locations once after link into a struct of
  `GLint`s removes both.
- `RenderTargetState::Capture` (two `glGetIntegerv`) runs up to three times a
  frame - in `Render`, `renderEmissionPass` and `renderGlowCoverPass`. Capturing
  once in `Render` and handing it down is I5's concern about state queries on
  a tiler, at a third of its scope.

Measure on the target's CPU first. On the M2 neither is visible.

## 6. Decisions for the owner

1. **Does production rotate the hue, and are animations usually attached?**
   This orders the plan. With the hue rotating every frame item 4 buys nothing
   and items 5 and 6 carry it; with still frames common, item 4 is the largest
   single win (4x at 0.5). Perf review 10.8 asked the same.
2. **Is 1/255 at scale 1.0 acceptable?** Items 8 and 9 move pixels by that
   much, after item 1 restores the headroom.
3. **Pin thin bands to 1.0?** Item 11. It changes what a configured
   `resolutionScale` does, as the spotlight's clipped lamp already does for its
   own.

## 7. Suggested order

1. Items 1 and 2 - the headroom and the tool - before anything else.
2. Items 4 (step 1) and 6 together: both live in `Render` and `OnConfigChanged`
   and share one verification walk. Then item 4's step 2 on top of 6.
3. Item 5, with its texel-for-texel check.
4. Items 7 and 10's first and third bullets: small and exact.
5. Item 3 on the target, then the decisions in section 6, then items 11, 8 and
   9 as they fall.

## 8. What not to trust

- **One GPU.** Every figure is the M2's. The AMD figures quoted are from perf
  review section 10 and the AMD consequences of items 4 and 6 are inferred from
  them, not measured. The M2 is a tiler but not the target.
- **The partial-bake figures are a proxy.** They bake one band of four whatever
  moved, which is close to a segment in the middle of a straight and an
  overestimate of the gain when it crosses a corner.
- **`demo` at 1.0 under item 4** read 1.00x in the prototype for a reason this
  document guesses at rather than establishes (section 5.2), and the landed
  version reads 1.12x on the same rect (section 9). Trust the landed figure.
- **Drift.** Back-to-back rounds of an unchanged code path moved by up to 4%
  in this session, so single-digit ratios here (items 6, 8, the attribution's
  1.06x-1.09x) are indicative; the 1.5x-6x ones are not in doubt.
- **Not explored, deliberately:** a windowed gather (perf review section 6:
  the Lorentzian's tails make its error independent of the window), CPU
  hoisting of uniform-only shader math (measured 4-7% slower on Apple: its
  compiler already does it), merging the shading and ring programs (the AMD's
  93-624x), and caching the halo and bloom field itself across hue rotation,
  which is time-invariant but would hold full-resolution half-float channels -
  ~8 MB per pair at 1080p, and the shading combines six - against the memory
  goal.

## 9. Status

| item | state |
| ---- | ----- |
| 1. Regenerate the comparison page's 1.0 images | **done on the Apple M2 Pro** (2026-10-06, the tree with I46): `check`'s 1.0 column reads 0 there; the page is timed with the hue rotating |
| 2. `time --mode` | **done**: `still`, `hue`, `intensity`, `arc-wipe`, `segment-travel` |
| 3. Target-device run | **done on the Apple M2 Pro** (section 10); the real target still unmeasured |
| The split's gate | **set to the M2's values**, 1.0 and 0 (section 10.6, I44) |
| 4. Skip the offscreen passes when nothing moved | **step 1 done** (I40); step 2, the keyed gather, open |
| 5. Per-piece glow coverage bake | **done** (I42) |
| 6. Emission table keyed on its inputs | **done** (I39) |
| 7. Complete texture on `uGatherSeg` | **done** (I41) |
| 8 | open |
| 9 | **measured and set aside** (2026-10-07): within budget it applies nowhere at the defaults; loosened, 1.04-1.07x for a level on 3.4% of the frame |
| 10. Smaller glow coverage table | **two-channel without segments and idle release done** (I43); sizing by perimeter and RGBA8 open |
| 11-13 | open |

What 4, 6 and 7 measured together on the Apple M2 Pro, `neon-scale-check time`
at 1920 x 1080, before -> after, geometric mean over the twelve scenes (three
interleaved rounds; five for `hue` and `segment-travel`):

| mode | 1.0 | 0.5 | 0.25 |
| ---- | --: | --: | ---: |
| `still` | **1.15x** (1.03-1.47x) | **3.84x** (2.20-6.63x) | **2.54x** (1.93-4.41x) |
| `hue` | 1.00x | 1.00x | 1.00x |
| `intensity` | 1.01x (`bounded_band` 1.12x) | 1.03x (`bounded_band` 1.15x) | 1.03x (`bounded_band` 1.18x) |
| `arc-wipe` | 1.00x | 1.00x | 1.00x |
| `segment-travel` | 1.00x | 1.00x | 1.00x |

`default` still at 0.5 went from 0.295 to 0.073 ms. Two things in that table
were predictable and one was not:

- **Moving frames are untouched**, as they should be: a frame whose config or
  hue moved can reuse nothing. That is also where most of what is left lives -
  `arc-wipe` and `segment-travel` are the glow coverage bake (item 5).
- **`intensity` only moves the inline path.** Item 6 removes the emission
  re-bake from a frame whose config changed without touching the table's
  inputs, and on the split path that frame still runs the gather, so it
  leaves the caller's target either way. The cutoff band keeps its loop
  inline, so the re-bake was its only offscreen pass - and on the M2 that pass
  cost 0.03 ms there, not the 6-8 us section 3.2 measured for the split.
- **The gain at 1.0 is larger than item 4's prototype measured.** At 1920 x
  1080 the check suite's `default` scene is the same 960 x 540 rect as
  section 3's `demo` (corner radius 60 rather than 40), and it reads 1.12x
  (0.617 -> 0.552 ms) where the prototype read 1.00x. Why the prototype missed
  it is not established; section 8 was right not to extrapolate that row.

Verification is recorded with the findings (I39-I41): 212 frames of a
long-lived effect byte-identical to the library before, `check` unchanged,
`partition` passing on two seeds.

### Item 5

Exact - 788 coverage tables read back after random arc and segment edits,
every one equal, texel for texel, to a fresh effect's full bake - and,
`neon-scale-check time` against `f7465fc`, five interleaved rounds:

| mode | 1.0 | 0.5 | 0.25 |
| ---- | --: | --: | ---: |
| `segment-travel` | 1.07x (1.02-1.12x) | 1.12x (1.06-1.21x) | 1.15x (1.10-1.26x) |
| `arc-wipe` | 1.01x | 1.01x | 1.01x |
| `still` | 1.00x | 1.00x | 1.00x |

On the 1840 x 1000 band, a travelling segment's frame went from 0.283 to 0.220
ms (1.29x) at 1.0 and 1.28x at 0.5 - against the 1.91x section 5.2 measured by
baking one band of four. The gap is the bake's own segment reach: it cuts a
bell at `5 / sqrt(2)` of its invSigma, so a segment of length 0.1 reaches
+/-0.18 of the ring and meets three to five pieces, not one. Two follow-ups,
neither exact: cut the bell nearer, where the emission pass already drops it
(under 0.005 of its boost), which needs `check`'s headroom back first (item 1);
and, exact but small, mark only the moving end of an arc whose length alone
changed.

### Item 10, first and third bullets

Exact (I43), and a memory change only: 0.99x-1.00x in every timing mode.
Live textures on the M2 for a partly lit ring with no segments, 1.08 -> 0.58
MB; after a ring has sat uniformly lit for more than 5 s, 1.08 -> 0.08 MB.

## 10. Item 3: measured on the Apple M2 Pro

Item 3 run on the machine at hand, on the tree after I39-I43 (`f7465fc` plus
items 5 and 10): the profile by frame type, the split's gate re-calibrated, the
whole frame layer by layer, startup, and memory. Everything here is this GPU's;
the real target - a tiler, by the docs - is still unmeasured, and this is the
data the section 6 decisions were waiting on for this machine.

### 10.1 The neon by frame type

`neon-scale-check time` in every mode, median of two rounds, ms. A selection;
the full grid is twelve scenes x six scales x five modes x three sizes.

| scene, size, scale | still | hue | intensity | arc wipe | segment travel |
| ------------------ | ----: | --: | --------: | -------: | -------------: |
| `default` 720p 1.0 | 0.330 | 0.398 | 0.394 | 0.538 | 0.643 |
| `default` 1080p 1.0 | 0.554 | 0.620 | 0.619 | 0.808 | 0.919 |
| `default` 1080p 0.5 | 0.073 | 0.301 | 0.291 | 0.391 | 0.498 |
| `default` 4K 1.0 | 1.192 | 1.304 | 1.291 | 1.587 | 1.797 |
| `default` 4K 0.5 | 0.175 | 0.655 | 0.649 | 0.793 | 0.952 |
| `soft_wash` 1080p 1.0 | 0.992 | 1.064 | 1.059 | 1.675 | 1.792 |
| `soft_wash` 4K 1.0 | 3.899 | 4.064 | 4.068 | 6.401 | 6.576 |
| `bounded_band` 1080p 1.0 | **0.232** | **0.106** | 0.250 | 0.344 | 0.518 |
| `bounded_band` 4K 1.0 | 0.100 | 0.138 | 0.134 | 0.218 | 0.295 |

- **Below 1.0 the still column is the cache (I40), and the rest is not.** At
  0.5 a moving frame costs 3-5x a still one: everything the cache skips comes
  back. A host that animates every frame should read the moving columns.
- **`bounded_band` at 1080p is cheaper with the hue rotating than still**,
  0.106 against 0.232 ms: a rotating hue forces the split, while the area gate
  keeps this band inline. Section 10.2 is that row, for every size.
- **Arcs and segments moving are the dearest frames** - 1.3-1.7x a still one
  at 1.0 - and what is left of them is the glow coverage bake (the bell's
  reach, section 9) and the partly lit shading (items 8 and 9).

### 10.2 The split's gate, re-calibrated

Two libraries built from this tree, one that never splits the gather at 1.0
(`FULL_RES_SPLIT_GATHER_MAX_SCALE` 0) and one that always does (1.0, and
`FULL_RES_SPLIT_MIN_AREA_PX` 0), timed interleaved with the shipped gate, two
rounds, 1920 x 1080. "Pulse" is an intensity pulse at hue rate 0 - a config
change every frame, so nothing is reused. Inline / split, above 1 the split
wins:

| rect (gather scale) | the gate picks | still | pulse | hue |
| ------------------- | -------------- | ----: | ----: | --: |
| 64 x 36 glow (1.00) | inline | 4.61x | 1.11x | 1.09x |
| 96 x 54 glow (0.82) | inline | 4.67x | 1.37x | 1.41x |
| 128 x 72 glow (0.62) | inline | 4.62x | 1.92x | 1.92x |
| 192 x 108 glow (0.41) | split | 4.75x | 2.67x | 2.64x |
| 960 x 540 glow (0.08) | split | 6.02x | 5.44x | 5.34x |
| 160 x 87 band (0.50) | inline (hue: split) | 3.21x | 1.31x | 1.19x |
| 640 x 348 band (0.12) | inline (hue: split) | 4.55x | 2.26x | 1.93x |
| 960 x 522 band (0.08) | inline (hue: split) | 4.75x | 2.86x | 2.34x |
| 1840 x 1000 band (0.06) | split | 5.50x | 3.49x | 3.37x |

The split wins at every size in every mode on this GPU, and the gate leaves
1.1-4.75x on the table wherever it picks inline: on the small glows its gather
scale cap does, on the bands below ~1100 px wide its area threshold does. Two
things changed since the AMD set those constants (perf review 10.1): this
GPU's first offscreen pass costs ~6 us rather than 0.15 ms, and since I40 a
still frame never leaves the caller's target, so the "still" column - the one
the area threshold was calibrated against - no longer pays for the split at
all on any GPU.

What it would take, which is the owner's call (section 6):

- **`FULL_RES_SPLIT_MIN_AREA_PX` to 0** on this GPU: no memory cost, and on
  the AMD its only remaining reason is the animated, rate-0 frame of a quad
  under ~140k px, which pays the 0.15 ms there. Re-measure the AMD's "pulse"
  column before changing it for both.
- **`FULL_RES_SPLIT_GATHER_MAX_SCALE` to 1.0** for the small glows: 1.1-1.9x on
  moving frames and 4.6x still, but above 0.5 the gather buffer grows toward
  the viewport at RGBA16F - up to 16.6 MB at 1080p for a tiny rect whose glow
  fills the frame, against ~4 MB at the 0.5 cap (perf review 10.8, question 5).
  A memory trade, not a free one.

### 10.3 The whole frame

Each layer alone and all four together, default configs, three spotlight lamps,
min of five runs of 40 frames, "animated" with the clock advancing (the hue
rotating, the droplets falling, the flare turning):

| scene | neon | droplets | lens flare | spotlight x3 | all four |
| ----- | ---: | -------: | ---------: | -----------: | -------: |
| 1080p, 960 x 540 rect, still | 0.545 | 0.053 | 0.768 | 0.072 | 1.417 |
| 1080p, 960 x 540 rect, animated | 0.616 | 0.053 | 0.765 | 0.072 | 1.501 |
| 1080p, production band, still | 0.060 | 0.076 | 0.767 | 0.072 | 0.968 |
| 1080p, production band, animated | 0.108 | 0.075 | 0.768 | 0.071 | 1.020 |
| 4K, 1920 x 1080 rect, animated | 1.257 | 0.081 | 3.020 | 0.071 | 4.480 |
| 4K, production band, animated | 0.179 | 0.134 | 3.023 | 0.071 | 3.423 |

**The neon is no longer the expensive layer here; the lens flare is.** On the
production band it is 75% of the frame at 1080p and 88% at 4K, and it costs the
same whatever the rect, because it shades the whole viewport. Perf review 10.8
question 6 asked exactly this, and on this GPU the answer is: the next speed
work belongs to the flare - its hex sprite gating (`lens-flare-perf-review.md`
section 7, about 1.7x on that layer) and its own `resolutionScale` - not to
items 8, 9 or 12 here.

### 10.4 Startup

Construct + `Initialize` + `AddRenderer` + the first config, then the first
frame (which compiles the path's programs), then the second; three processes
each, ms:

| path | init | first frame | second frame |
| ---- | ---: | ----------: | -----------: |
| 1.0, loop inline (a small rect) | 2.8-3.0 | 10.9-11.7 | 1.0-1.4 |
| 1.0, gather split | 2.8-3.0 | 11.3-11.7 | 1.4-2.3 |
| 1.0, split, partly lit (adds the coverage bake) | 2.9 | 17.2-18.8 | 1.3-3.1 |
| 0.5 (gather, shade, ring, blit) | 2.9-3.0 | 19.5-22.3 | 0.8 |

The first frame is the compile, and it is the number a host feels: ~11 ms at
1.0, ~18 ms with a partly lit ring, ~20 ms below 1.0. Repeated processes, so
the driver's shader cache may have been warm.

### 10.5 Memory

Live textures, 1920 x 1080, after item 10 (section 4 has the full table from
before it): a uniform ring at 1.0 holds 0.08 MB; an arcs-only partly lit ring
0.58 MB; with a segment 1.15 MB; a ring settled uniform for 5 s gives the
table back; 0.5 holds 1.8-2.1 MB of reduced buffer, the production band
included (section 4, item 11).

### 10.6 The gate set to the M2's values

The owner's decision on 10.2: `FULL_RES_SPLIT_GATHER_MAX_SCALE` 0.5 -> 1.0 and
`FULL_RES_SPLIT_MIN_AREA_PX` 140000 -> 0, so every rect splits the gather at
1.0. The gate itself stays, with the AMD's values recorded beside it in
`neon-tuning.h`, so a GPU that needs it can be tuned back, and a max scale of 0
still builds the inline loop as a reference.

**Output.** Only configs that used to keep the loop inline move, by the split's
own 1/255: of ten probed at 1080p - small glows 64 x 36 to 128 x 72, cutoff
bands 160 to 960 px wide, with and without a segment, a tiny rect with a
frame-filling glow - each moved 1,126-14,793 pixels by exactly 1 level.
`neon-scale-check check` passes, `bounded_band` still reading 1 on the 1.0
column; `partition` passes.

**Time.** `neon-scale-check time` before -> after, three interleaved rounds.
Of the twelve check scenes only `bounded_band` changed path:

| `bounded_band` at 1.0 | still | intensity | arc wipe | segment travel |
| --------------------- | ----: | --------: | -------: | -------------: |
| 1280 x 720 | 0.172 -> 0.049 (3.50x) | 2.03x | 1.69x | 1.81x |
| 1920 x 1080 | 0.242 -> 0.066 (3.69x) | 2.40x | 1.92x | 2.07x |

Every other scene, at every scale and size, in every mode, 0.93x-1.06x - the
session's noise on a path that did not change.

**Memory**, the price, measured with the same probe (live textures, 1080p):

| config | before | after |
| ------ | -----: | ----: |
| cutoff bands 160-960 px | 0.01 MB | 0.06-0.08 MB |
| glow 128 x 72 / 96 x 54 / 64 x 36 | 0.01 MB | 1.65 / 2.68 / 3.62 MB |
| glow 128 x 72 + segment | 1.01 MB | 4.29 MB |
| 40 x 24 rect, `glowRadius` 20 | 0.01 MB | **16.08 MB** |
| the same + segment | 1.01 MB | **33.15 MB** |

The bands - the production case - cost nothing. A rect small enough to gather
near full resolution (perimeter under ~450 px) now holds a gather buffer over
its whole glow quad at up to full resolution, RGBA16F, two attachments with
segments: the last two rows are the worst case, a thumbnail-sized rect whose
glow covers the frame. If that host exists, the cheapest cap is to clamp the
split's gather scale rather than refuse the split - not built, since gathering
a small rect coarser than its kernel costs it quality (V17).

## 11. The neon's next targets (after I39-I44)

Section 10's end-to-end run left one frame type unimproved: the library's
default, a hue rotating every frame, where every pass recomputes. This section
attributes what that frame costs now and ranks what could take it down. Apple
M2 Pro, 1920 x 1080, current tree; each term stubbed in the shader source at
compile time (a `glShaderSource` hook, no library change), three interleaved
rounds, median. Terms are not additive - removing one lets the compiler drop
what fed only it.

### 11.1 Where a default frame goes

| scene | total | after the gather read | corners | straights' bloom | `perimeterPosition` | straights' halo | gather pass | filament | tone map |
| ----- | ----: | --------------------: | ------: | ---------------: | ------------------: | --------------: | ----------: | -------: | -------: |
| 960 x 540, 1.0, hue | 0.622 | 0.474 (76%) | 16% | 14% | 9% | 7% | 8% | 0% | 0% |
| 960 x 540, 1.0, still | 0.572 | 0.509 (89%) | 23% | 19% | 13% | 9% | - | 1% | 2% |
| arc 0.5, 1.0, hue | 0.805 | 0.646 (80%) | 23% | 20% | 10% | 9% | 11% | 4% | 3% |
| screen-sized rect, 1.0, hue | 0.636 | 0.474 (75%) | 18% | 20% | 15% | 13% | 13% | 9% | 9% |
| 960 x 540, 0.5, hue | 0.344 | 0.175 (51%) | 16% | 13% | 10% | 7% | **20%** | 5% | 4% |
| production band, 1.0, hue | 0.105 | ~0 | 3% | 2% | 2% | 1% | 0% | 0% | 0% |

"After the gather read" stubs everything from the read of the gather buffer
on - what is left (rasterising 1.7M px, the SDF, the derivative, the cuts, the
filament core, the read, the blend, and on a hue frame the emission and gather
passes) is the frame's floor: 0.148 ms with the hue rotating, 0.064 still.

- **The shading's arithmetic is the frame.** Three quarters of a default frame
  at 1.0, in a few transcendental-heavy terms; the corners and the straights'
  bloom alone are 30-43%.
- **Not dead fragments.** An occlusion count against the lit pixels: 97% of
  the fragments the default quad shades end up non-zero (88-91% for a smaller
  glow or rect), so a tighter quad would buy at most a tenth.
- **The production band is overhead, not shading.** Stubbing any term moves
  nothing; its 0.105 ms is the passes themselves.
- **Below 1.0 the gather pass is a fifth of a moving frame**, and the floor -
  the blit over the lit area, the ring - half.

### 11.2 Targets, ranked by what they could take off a default frame

**A. Factor the hue out of the shading (new; the largest by far).** *Built -
I46: at 1.0, 3.3x geometric mean with the hue rotating and 6.3x still at
1080p, animated frames unchanged, within 1/255, +3.3 MB for the default rect.* Under hue
rotation only two things in `neon.frag` move: the gathered hue `col` (through
the emission table) and the colour-stop alpha `baseAlphaPt`, which is constant
when the stops share one alpha (the default). Everything else - the halo, the
bloom, the pieces' coverage, the filament, the fade, the cuts - depends on the
config and the geometry alone. The output is

    out(p, t) = mask(p) * tonemap( col(p, t) * Fa(p) + segHue(p, t) * Fs(p) )

with `Fa`, `Fs` and `mask` time-invariant - and with no segments, `Fs` is 0
and the default config has no mask (both sides, no cutoffs). So `Fa` could be
baked once per config change into a full-resolution R16F field over the glow
quad, and a moving frame would be the gather plus a composite: read the field,
read the gather, tone-map. The floor above prices that composite: a default
frame 0.622 -> ~0.15 ms with the hue rotating (~4x) and 0.572 -> ~0.07 ms still
(~8x), at 1.0. The costs:

- memory: R16F over the quad, 3.4 MB at 1080p for the default rect (RG16F,
  6.8 MB, with a cut or cutoff; RGBA16F with segments), four times that at 4K
  - the renderer's largest allocation by far, so a cap or an opt-in;
- precision: half-float `Fa` and a reordered product move pixels by up to
  ~1/255, so it waits on item 1's headroom;
- a fallback for what breaks the factorisation: stops with differing alpha,
  segments with their own stops under a rotating hue.

Below 1.0 the same split would cache pass 1b's field at the reduced scale
(0.85 MB at 0.5) and leave only the ring at full resolution per frame. *Built -
I50, AMD Radeon Pro 5300M, 1080p: under a rotating hue only (every other
unchanged frame skips pass 1b already), 2.00x / 1.56x / 1.23x on hue frames at
0.75 / 0.5 / 0.25, 0.84 MB at 0.5, within 1 level. The blit stays per frame as
well as the ring, which is why 1.0 is still the cheaper scale for those frames
(0.22 against 0.37 ms on the default rect).*

*Prototyped (scratch build, not in the tree).* `neon.frag` compiled with a
`NEON_FIELD_BAKE` define sets the gathered hue to 1 and writes the
pre-tone-map result (`Fa`) and the masks applied to 1.0 (`mask`) into an
RG16F buffer, through the same transform as the glow pass, so texel and pixel
coincide; a 15-line composite reads it, reads the gathered hue, and tone-maps.
At 1.0, no segments and an opaque ring (anything else takes the existing
path). The bake is LAZY: a frame whose config just changed draws the existing
way, and only a config that has held for a frame is baked - otherwise an
animation that changes the config every frame paid a bake and a composite
(5-7% slower). Measured on the M2, 1080p, three interleaved rounds:

| scene | current | field | |
| ----- | ------: | ----: | -: |
| 960 x 540, hue rotating (the default) | 0.565 ms | 0.139 ms | **4.1x** |
| 960 x 540, still | 0.495 | 0.055 | **9.1x** |
| arc 0.5, hue rotating | 0.768 | 0.138 | 5.6x |
| screen-sized rect, hue | 0.532 | 0.146 | 3.6x |
| 640 x 360, hue | 0.474 | 0.144 | 3.3x |
| inside cutoff (a mask), hue | 0.477 | 0.145 | 3.3x |
| production band, hue | 0.101 | 0.099 | 1.0x |
| intensity pulse / arc wipe / glow pulse (config changes every frame) | 0.563 / 0.809 / 0.554 | 0.565 / 0.809 / 0.541 | 1.0x |
| segments, stops with differing alpha (fall back) | 0.817 / 0.621 | 0.822 / 0.619 | 1.0x |

Output against the existing path, one frame per scene after 12 hue frames:

| field format | pixels moved | by |
| ------------ | -----------: | -: |
| RG16F | 0.2-1.1% of the frame (4,683-23,000) | exactly 1 level |
| RG32F | 0-11 pixels | exactly 1 level |
| either, on a fallback config | 0 | - |

RG32F costs 3.3x instead of 4.1x on the default frame (the composite reads
twice the bytes) and twice the memory. The prototype's buffer was the whole
viewport (7.9 MB RG16F at 1080p); built for real it would cover only the glow
quad (`GetBufferRegion`), and be one channel where there is no mask - the
default config has none - so 3.3 MB for the default rect at 1080p, 13 MB at 4K.

What building it for real takes:

- the buffer region-bounded and R16F / RG16F by the mask, released like the
  scaled buffers;
- the field read offset by the host's viewport origin (the prototype assumed
  0, 0);
- a field per path below 1.0 (pass 1b's, at the reduced scale, with the ring
  still shaded per frame), or 1.0 only;
- its own staleness, sharing I40's: any config change, the viewport, and the
  gather's coverage channel - which the hue does not move;
- a one-level change on ~1% of pixels on the frame an animation stops and the
  field takes over (none with RG32F) - the paths differ by that much;
- item 1 first: `check` cannot absorb a 1/255 move at 1.0 today.

**B. Skip `perimeterPosition` and the gradient alpha fetch when nothing reads
them (exact, small).** *Done - I45: the default frame 1.11x at 1080p (0.633 ->
0.568 ms with the hue rotating), byte-identical where it engages; 1-2 pixels by
1 level elsewhere, from code generation.* `sPos` feeds three things: the gradient's alpha fetch,
the arc loop - which returns before reading it for an arc over the whole ring
- and the segment loop. On a uniformly lit ring with no segments and stops of
one alpha (the default), it is computed for nothing, and so is the fetch. A
uniform flag and the constant alpha from the CPU skip both: ~9% of a default
frame at 1.0 (13% still, 15% on a screen-sized rect), byte-identical, a few
lines in `neon.frag` and `uploadNeonUniforms`.

**C. The corner pieces (16-23%).** Item 9's far-field point source: past a
CPU-bounded radius an arc is a point of length `pi r / 2`, with no
development `atan`. Perhaps half the share; up to 1/255. *Measured and set
aside - see item 9: within the budget it applies to none of the default
rect's corner evaluations, and loosened to where it pays 1.04-1.07x it moves
3.4% of the frame.*

**D. The straights' bloom (14-20%).** One two-argument `atan` per straight
plus the two shared pedestals. A skip like the corners', bounded on the CPU,
for a straight seen from far past its end; or a polynomial `atan`. Needs
measuring - a cheaper `atan` is GPU-specific. Up to 1/255. *The polynomial
half is built - I48: `minimaxAtan` in `bloomSegment` (the straights' and the
corners' bloom) and in the corners' development, 1.07-1.11x on every animated
frame mode at 1.0 and 0.5 on an AMD Radeon Pro 5300M, at most 1 level on at
most 0.016% of channels. Unmeasured on the M2.*

On the AMD the attribution of a frame whose config animates (intensity pulse
/ arc wipe at 0.5, `default` at 1080p, scale 1.0, before I48) reads: corners
24% / 22%, the straights' bloom 21% / 20%, the per-piece glow fix 6% / 18%,
the straights' halo 7% / 9%, `perimeterPosition` 2% / 6%, filament and tone
map 1-2% each. Measured and set aside there: `minimaxAtan` in
`perimeterPosition` (0.1-0.4%, inside the noise), and moving the uniform-only
pedestal / renormalisation math to the CPU (at most 7.2% / 1.9% with the
values folded to constants - an upper bound - against the 4-7% section 8
records it losing on Apple).

**E. The gather pass on moving frames (8% at 1.0, 20% at 0.5).** It re-runs
every frame the hue rotates. A coarser grid (`GATHER_TEXELS_PER_KERNEL`) or
fewer samples trade quality; the two-level gather (perf review section 6) is
the exact-ish route. Worth it below 1.0 mostly.

**Not targets:** the filament and the tone map (0-9%), a tighter quad (at most
3-12% of fragments), and the production band's shading (none to take).

### 11.3 Recommended order

1. **B** - exact, a few lines, ~9% on every default frame at 1.0.
2. **A** - prototyped: 4.1x on the default frame (hue rotating), 9.1x still,
   no cost while animating, within 1/255; 3.3 MB at 1080p built properly. The
   decision is the memory, and item 1's headroom comes first.
3. **C and D** after item 1's headroom, measured one at a time.
