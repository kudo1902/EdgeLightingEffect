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

**1. Regenerate the comparison page's 1.0 images.** The four steps in
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

Do **not** retire `FULL_RES_SPLIT_MIN_AREA_PX` on step 1 alone. It would make
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
| 1. Regenerate the comparison page's 1.0 images | open - waits on which GPU owns the reference images (the page's are the AMD's) |
| 2. `time --mode` | **done**: `still`, `hue`, `intensity`, `arc-wipe`, `segment-travel` |
| 3. Target-device run | open |
| 4. Skip the offscreen passes when nothing moved | **step 1 done** (I40); step 2, the keyed gather, open |
| 5. Per-piece glow coverage bake | **done** (I42) |
| 6. Emission table keyed on its inputs | **done** (I39) |
| 7. Complete texture on `uGatherSeg` | **done** (I41) |
| 8, 9 | open (wait on item 1) |
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
