# Neon: performance review and the three changes it produced

The neon layer was the most expensive thing in the pipeline by a wide margin,
and almost all of that cost sat in one loop. This document records the
measurement that established that, the attribution that found where inside the
loop the time went, the three changes that followed, and the two directions
that were measured and deliberately not taken.

Read `lens-flare-perf-review.md` first if you have not: it is the same exercise
one layer over, and section 8 of it describes the harness this reuses.

Everything below was measured back-to-back in one session on one machine, with
the caveat in section 7 about which numbers to trust.

## 1. What is being compared

`main_v2` at `944267a` against the same tree with three changes:

| # | change | area | visual effect |
| - | ------ | ---- | ------------- |
| 1 | the gather's segment `texelFetch` is hoisted out of the loop behind a uniform branch | `neon.frag` | byte-identical |
| 2 | `CMAKE_BUILD_TYPE` defaults to `Release` when the caller sets none | `CMakeLists.txt` | none (CPU only) |
| 3 | the animated config composite reuses a member scratch instead of copy-constructing a local | `edge-lighting.cpp` | none (CPU only) |

## 2. The starting measurement

Per-layer GPU time, default config, rect at half the viewport, `cornerRadius`
48, layers enabled one at a time:

| layer | 1920x1080 | 3840x2160 | share at 1080p |
| ----- | --------- | --------- | -------------- |
| **neon** | **4.26 ms** | **10.59 ms** | **81%** |
| lens flare | 0.79 ms | 2.98 ms | 15% |
| droplets | 0.07 ms | 0.13 ms | 1.3% |
| all three | 5.27 ms | 14.30 ms | 100% |

At 4K the three layers together already exceed a 60 Hz budget before the host
draws anything of its own.

### Where the time went

Variants of `neon.frag` with individual terms stubbed out, each compiled and
timed against the same baseline in one session. 1920x1080, neon only,
`numSamples` 128:

| what was removed | ms | cost of that piece |
| ---------------- | -- | ------------------ |
| nothing (baseline) | 4.336 | - |
| **the entire gather loop** | 0.206 | **4.130 ms, 95.2%** |
| both `texelFetch`es (to constants) | 3.003 | 1.333 ms, 30.7% |
| the row-1 `texelFetch` only | 3.575 | 0.761 ms, 17.6% |
| the UBO sample read | 4.328 | 0.008 ms, 0.2% |

Two things fall out of that table.

The gather loop is not *part* of the neon cost, it is essentially all of it.
Anything outside the loop - the SDF, the analytic halo and bloom, the pointwise
arc and segment coverages, the tone map - totals 0.206 ms together.

And **UBO reads are free**. `uLoopSamples[i].xy` costs 0.2% of the loop, which
is inside the noise. Moving data *into* a uniform block and out of the emission
texture is therefore a live direction; shrinking the loop-samples block is not.

## 3. The three changes

### Change 1: hoist the segment fetch out of the gather

The loop performed two `texelFetch`es per iteration. Row 1 is the travelling
segment term, and `neon-emission.frag` writes it inside a loop bounded by
`uSegmentCount` - so at `uSegmentCount == 0` the row is exactly `vec4(0.0)` at
every texel, and the two accumulations it feeds are provably no-ops. A config
with no segments (the default, and the common one) was issuing 128 texture
reads per fragment to add zero, over a quad that covers roughly 82% of a 1080p
screen at the default `glowRadius`.

The branch has to sit **outside** the loop. Gating the fetch per iteration was
tried first and measured **slower** than leaving it alone:

| variant | ms |
| ------- | -- |
| baseline (both fetches, unconditional) | 4.23 |
| `uSegmentCount > 0 ? texelFetch(...) : vec4(0.0)` inside the loop | 4.52 |
| the branch hoisted, two straight-line loop bodies | 2.98 |

The per-iteration branch costs what the fetch it skips cost. Only hoisting it
so each body is straight-line pays.

The `uSegmentCount > 0` body is the original loop **verbatim**, which is what
makes a segmented config byte-identical by construction rather than by
measurement. The other body drops exactly the fetch and the two dead
accumulations; `segAcc` and `wsumSegW` keep the values they were initialised
with, which is what the deleted adds would have left them holding.

Branching on a uniform is safe here for the same reason the lens flare's
`uSpread` guard is: the condition is uniform across the draw, so control flow
stays uniform. Nothing in either body takes a derivative in any case -
`texelFetch` has no LOD to compute - so this is not the `fwidth` hazard that
blocks gating the flare's hex sprite.

### Change 2: a default build type

The root `CMakeLists.txt` never set `CMAKE_BUILD_TYPE` and never set an
optimisation flag, and an empty build type contributes **no `-O` flag at all**.
The command in `README.md` and `CLAUDE.md` therefore produced this:

```
clang++ -DPLATFORM_MACOS -I... -g -std=gnu++17 -arch arm64 -fPIC
        -c lib/src/renderer/neon-renderer.cpp
```

Every host linking `libedge-lighting.a` got an unoptimised build of the LUT
bakes, the colour conversions, the contour tracer, the whole C ABI and the
per-frame config path. This is not a neon finding at all; it was found while
measuring one.

Only a default: an explicit `-DCMAKE_BUILD_TYPE=Debug` is honoured, and a
multi-config generator (which drives the choice per-build and leaves the
variable empty by design) is left alone.

### Change 3: the animated composite allocated every frame

`EdgeLightingEffect::refreshActiveConfig` began the animated path with

```cpp
Config active = mBaseConfig;   // copy-CONSTRUCT
```

which allocates fresh storage for every vector the config owns: the base colour
stops, both segment pools, the arcs, and each of their own stop lists. The
`std::move` at the end then freed the previous frame's. Measured with a global
`operator new` counter and one animation attached:

| config contents | allocations/frame | frees/frame | bytes/frame |
| --------------- | ----------------- | ----------- | ----------- |
| default | 3 | 3 | 168 |
| 4 segments + 4 arcs | 11 | 11 | 784 |
| 8 segments + 8 arcs (the caps) | 19 | 19 | 1488 |

Copy-**assigning** into a member scratch reuses the capacity already there, and
**swapping** with `mActiveConfig` (rather than moving) hands the scratch the
outgoing config's buffers, so they are still the right size next frame. Steady
state after the change is zero allocations on the per-frame path.

A move-assign instead of a swap would undo the whole thing: it leaves the
scratch holding moved-from empty vectors, and the next frame reallocates all of
them.

## 4. Performance

### Change 1, interleaved A/B

Absolute timings drift between sessions on this machine, so the two builds were
run alternately rather than one after the other:

| round | before | after | ratio |
| ----- | ------ | ----- | ----- |
| 1080p 1 | 4.916 | 3.280 | 1.50x |
| 1080p 2 | 4.984 | 3.163 | 1.58x |
| 1080p 3 | 5.171 | 3.448 | 1.50x |
| 1080p 4 | 4.971 | 3.569 | 1.39x |
| 1080p 5 | 4.713 | 3.400 | 1.39x |
| 4K 1 | 12.140 | 8.377 | 1.45x |
| 4K 2 | 11.073 | 7.737 | 1.43x |

Median **1.46x** on the neon layer. Re-run against the landed tree after all
three changes, same method: 4.973 / 4.320 / 4.317 ms before against 3.028 /
3.165 / 2.957 ms after.

Whole frame with all three layers enabled: 5.27 -> 3.7 ms at 1080p,
14.30 -> 10.6 ms at 4K.

The gain is one texture read removed from a 128-iteration per-fragment loop.
That mechanism gets *stronger*, not weaker, on the more texture-bound mobile
parts this library targets - but see the caveat in section 7 about the single
GPU behind every number here.

### Changes 2 and 3, CPU

Per-frame config path, no renderers, one `IntensityPulse` attached, at the
segment and arc caps:

| build | ms/frame | allocations/frame |
| ----- | -------- | ----------------- |
| before (no `-O`, copy-construct) | 0.0012 | 19 |
| change 2 only (`-O3`, copy-construct) | 0.0008 | 19 |
| changes 2 + 3 | **0.0002** | **0** |

`libedge-lighting.a`: **11.6 MB -> 845 KB**.

Read the millisecond column honestly. On a desktop allocator change 3 is worth
about one microsecond a frame, which is 0.03% of a 4 ms frame and will not move
anyone's FPS. What it is worth is roughly 1100 malloc/free pairs a second that
no longer happen, forever, in a library aimed at edge devices - a fragmentation
and jitter cost rather than a cost in cycles, and one that is invisible on the
machine it was measured on.

## 5. Visual comparison

Change 1 is byte-identical. Six scenes, captured through `OffscreenCapture` at
1920x1080 and compared as raw RGBA8:

| scene | pixels changed | max delta |
| ----- | -------------- | --------- |
| default, no segments | 0 / 2073600 | 0 |
| one plain segment | 0 / 2073600 | 0 |
| one segment with its own colour stops | 0 / 2073600 | 0 |
| four arcs, each with colour stops | 0 / 2073600 | 0 |
| both cutoffs enabled | 0 / 2073600 | 0 |
| `resolutionScale` 0.5 | 0 / 2073600 | 0 |

Changes 2 and 3 touch no shader and no uniform, so there is nothing to compare.

## 6. What was measured and NOT taken

### The windowed gather

The obvious way to attack the remaining 95% is to stop walking all 128 samples.
The fragment's own continuous perimeter position is already recovered just
below the loop (`perimeterPosition(vPos)`), so the walk can be centred there
and run over a window of `+/- K` samples. It is fast, and it is not free:

| window | iterations | ms | speedup | pixels changed | mean abs delta | max delta |
| ------ | ---------- | -- | ------- | -------------- | -------------- | --------- |
| K = 8 | 17 | 0.93 | 4.6x | 74.6% | 2.73 | 22 |
| K = 16 | 33 | 1.51 | 2.8x | 74.3% | 2.24 | 22 |
| K = 24 | 49 | 2.28 | 1.9x | 73.6% | 1.83 | 19 |
| K = 32 | 65 | 2.83 | 1.5x | 71.4% | 1.57 | 16 |

Against a 4.23 ms baseline in that session.

The important column is the last one, and what it does NOT do: the error barely
falls as the window doubles. That is the mechanism talking. The gather weight
`1 / (dd + kc2)` is a Lorentzian with `1/d^2` tails, and `col` is normalised by
the same weight it was gathered with - so the far side of the ring genuinely
contributes to the denominator, and truncating it shifts the hue no matter
where the cut is made. A window is the wrong shape for this sum.

The direction that should work, and which was **not** built: a two-level
gather - the near window at full rate, the far samples read from a coarser mip
of the emission table. The far field is smooth precisely where the flat window
is wrong, so its error should sit well below these numbers. That needs building
and diffing before anyone believes it.

### The interior of the draw quad

`setupGeometry` builds a solid quad covering rect + margin, and the fill and
droplet passes both show the pattern for cutting a hole in one. It does not
help here at the default config: the margin is
`glowRadius * 48 * (1 + bloomStrength * intensity)` = 312 px, which is already
larger than half the rect's height on a 960x540 rect, so there is no interior
left to cut. It only becomes worth doing for rects much larger than twice the
glow reach, and it would need an inner fade mirroring the existing
`uQuadMargin` one or the hole's edge would show.

**Since built, without the fade** (`GetGlowInnerReach` in
`neon-renderer.cpp`; the full write-up, with figures, is
[`glow-inner-reach.md`](glow-inner-reach.md)). Reported as "the neon is slow
without cutoffs, even when a cutoff far past the visible glow makes it fast": on
a screen-sized rect the interior is most of the frame, and an inside cutoff was
the only thing that cut it. The fade turned out not to be needed: the hole is
cut where an upper bound on everything the glow can still write there is under
half an 8-bit level, so it changes no pixel. Glow-only frame, 1920 x 1080, a
1840 x 1000 rect, AMD Radeon Pro 5300M, best of 2 interleaved rounds:

| config | before | after | |
| --- | ---: | ---: | ---: |
| defaults (glowRadius 5) | 5.98 ms | 5.25 ms | 1.14x |
| glowRadius 2 | 5.97 ms | 3.01 ms | 1.98x |
| glowRadius 2, sharp corners | 5.04 ms | 2.29 ms | 2.20x |
| glowRadius 2, `GlowSide::INSIDE` | 5.34 ms | 2.33 ms | 2.29x |
| glowRadius 0 (filament only) | 5.41 ms | 0.49 ms | 11.04x |
| glowRadius 2, scale 0.5 | 1.20 ms | 0.86 ms | 1.40x |
| defaults on the 960 x 540 demo rect | 4.94 ms | 4.96 ms | 1.00x |

## 7. Method, and what not to trust

The harness from `lens-flare-perf-review.md` section 8: a throwaway binary
linked against `build/lib/libedge-lighting.a`, a hidden GLFW window, an
`OffscreenCapture` at the stated size, `glFinish` inside the timed region so
the numbers are GPU time rather than command submission.

One addition worth carrying forward: **render N times per captured frame** and
divide, so the FBO bind and restore amortise out. At one render per frame the
lens flare measured 0.05 ms against its true 0.79 ms - a 15x error, and one
that reads as a plausible number rather than an obviously broken one. Every GPU
figure here is at 32 renders per frame, cross-checked against 8.

CPU allocation counts come from replacing the global `operator new` /
`operator delete` in the harness translation unit, which catches every
allocation in the process including the ones inside the static library.

Pixel diffs are exact byte comparisons of the RGBA8 capture, not perceptual.

Machine: Apple Silicon, macOS, OpenGL 3.3 core, arm64 build.

### Caveat

- **Read ratios, not milliseconds.** The same specialised shader measured 2.98
  and 3.46 ms an hour apart. Every ratio quoted here is within-session, and
  change 1's is an interleaved A/B specifically because of that drift.
- **One GPU.** A driver whose compiler already hoists the loop-invariant branch
  would show less than 1.46x; a more texture-bound part should show more. The
  attribution table in section 2 is the part most likely to reorder elsewhere -
  the 95% figure for the loop is structural and should hold anywhere, but the
  31% / 64% split between its texture reads and its ALU is this GPU's answer.
- **One geometry.** Rect at half the viewport, `cornerRadius` 48, default neon
  config. The `glowRadius` row in section 8 shows how far the quad area, and
  therefore everything here, moves with the config.

## 8. The knobs a host already has

Measured on the landed tree, neon only, 1920x1080, one variable at a time.
Recorded here because the shape of each curve is not obvious, and two of the
three surprise:

| knob | measurements |
| ---- | ------------ |
| `resolutionScale` | 1.00 = 4.86 ms, 0.75 = 2.70, 0.50 = 1.21, 0.35 = 0.64, 0.25 = 0.36 |
| `numSamples` | 128 = 4.86 ms, 96 = 3.77, 64 = 2.57, 48 = 1.99, 32 = 1.39 |
| `glowRadius` | 20 = 5.20 ms, 10 = 5.67, 5 = 4.89, 2 = 2.40, 0 = 1.90 |

- **`resolutionScale` is the strongest lever and it behaves.** 0.50 is 4.0x and
  0.25 is 13.6x, close to quadratic - so the blit really is cheap, and the
  scaled path is not eating its own saving.
- **`numSamples` is linear** and defaults to its own ceiling
  (`NEON_MAX_LOOP_SAMPLES` = 128). Halving it halves the layer. The quality
  cost is on the books as V9 in `review-findings.md`.
- **`glowRadius` saturates above about 5**, because the quad is viewport-clipped
  by then. Trimming a glow from 20 to 10 buys nothing at all; trimming 5 to 2
  buys 2x. Worth knowing before anyone tunes it for speed rather than for looks.

**The `resolutionScale` row predates two changes, and its shape no longer
holds.** The edge ring (`neon-resolution-scale-plan.md` step 5) added a fixed
cost below 1.0. Then section 13 of that plan split the gather out: below 1.0
the loop runs once, alone, at its own coarse scale (about 2 texels per colour
kernel, typically an eighth of the viewport), and the reduced pass only shades.
So below 1.0 the cost is no longer close to quadratic in the scale - 0.5 is
nearly as cheap as 0.25 (llvmpipe, default scene: 13.5x and 19x faster than
1.0) - and `numSamples` is paid only on the gather's few thousand texels, so it
barely moves the scaled path. Both rows describe 1.0 as they stand.

## 9. What is left open

> **Later:** the first item is answered for most configs by section 10 - at
> 1.0 the loop now runs on the gather grid too, and the shading pass is the
> cost. The two-level gather is still unbuilt, and still the move for a rect
> small enough to keep the loop inline.

- **The gather loop is still 95% of the layer at 1.0.** Change 1 removed a
  fetch from it; it did not change its shape. The two-level gather in section 6
  is the next real move there, and it is unbuilt. Below 1.0 the loop is no
  longer per texel: `neon-resolution-scale-plan.md` section 13 runs it once on
  a coarse grid, and the reduced pass's shading is now the largest pass.
- **Gating the lens flare's hex sprite**, worth about 1.7x on that layer, is
  still open exactly as `lens-flare-perf-review.md` section 7 left it. It was
  written when the flare was the expensive layer; it is now second, so the item
  is worth roughly 0.3 ms at 1080p and 1.2 ms at 4K.
- **The emission pre-pass re-bakes every animated frame.** `OnConfigChanged`
  fires on every renderer every frame under an attached animation and sets
  `mEmissionDirty` unconditionally, so pass 0 runs every frame. The pass is 256
  fragments and genuinely free here, which is what `review-findings.md` I2
  concluded when it declined this. That reasoning is sound for an
  immediate-mode GPU and may not hold on a tile-based mobile one, where an
  extra render pass costs a tile flush and restore regardless of fragment
  count. Worth one measurement on target hardware before the item stays
  declined; it is a flag, not a finding.

  > **Later:** done, without the staleness risk I2 declined it for -
  > [`neon-perf-plan.md`](neon-perf-plan.md) item 6. The table is keyed on
  > what its pass binds (two uniforms by value, three LUTs and two light
  > blocks by upload count), not on config fields, so an animation that
  > moves none of those re-bakes nothing.

## 10. Scale 1.0 again: the gather split out, and the shading pass

Sections 1-9 left scale 1.0 as one program running the 128-sample gather loop
in every fragment of the glow quad, while below 1.0 the loop had already moved
onto a coarse grid of its own (`neon-resolution-scale-plan.md` section 13).
This round brings that to 1.0, then works on what is left: the shading.

Everything here was measured on an **AMD Radeon Pro 5300M** (i7-9750H MacBook
Pro, x86_64 Release build), against `80d71a7`, builds interleaved and the
median taken - read ratios, not milliseconds. Timings come from
`neon-scale-check time` (the twelve check scenes, their layout scaled to the
frame) plus a throwaway probe for the attribution and the extra scenes; the
probe patched shader source at run time through a `glShaderSource` hook, so
every variant in a table ran in one process on one GPU.

### 10.1 The gather at 1.0 (`SplitsGatherAtFullRes`)

`GetGatherScale` is a property of the rect, not of the resolution scale: the
gather's outputs are Lorentzian means whose kernel is never narrower than
`kc = perimeter * COLOR_BLEND_PERIM_FRAC`, so two texels per `kc` carry them
through a bilinear read at ANY scale - 0.11 for a 640 x 360 rect. At 1.0 the
renderer now runs pass 1a (`neon-gather.frag`) onto that grid and shades the
whole glow quad from it with the edge ring's program (`mNeonRingShader`,
`NEON_READS_GATHER`), at full resolution, on the caller's framebuffer. Reusing
the ring's program keeps the plan's rule of one target and one blend state per
program per frame: at 1.0 the ring never draws.

Quality: `neon-scale-check check` reads scale 1.0 at most **1/255** off the
committed images on all twelve scenes (0 before), and every reduced scale
unchanged against it. The ring has always read this same gather at full
resolution, so nothing about the read is new.

It is not free, and the gate is what the measurements below set:

| gather scale (rect) | split / inline |
| ------------------- | -------------- |
| 1.00 (60 x 36) | 0.98x |
| 0.94 (80 x 48) | 1.05x |
| 0.75 (100 x 60) | 1.27x |
| 0.54 (140 x 84) | 1.53x |
| 0.19 (400 x 240) | 1.96x |

Split only at a gather scale of 0.5 or below (`FULL_RES_SPLIT_GATHER_MAX_SCALE`):
the gain above it is small, and the bound keeps the gather buffer no larger
than the scaled path's at 0.5 already is.

The second condition is the surprise. On this GPU the FIRST offscreen pass of
a frame costs ~0.15 ms whatever it draws, and a second one in the same phase
almost nothing, so a split that is the frame's only offscreen pass has to earn
that back. Thin cutoff bands (`bounded_band`'s glow), split against inline,
both with the shading of 10.3-10.4:

| quad area | still | hue rotating |
| --------- | ----: | -----------: |
| 16k px (160 x 96 rect) | 0.27x | 1.00x |
| 66k px (640 x 360) | 0.67x | 1.22x |
| 109k px (960 x 540) | 0.86x | 1.37x |
| 187k px (1600 x 900) | 1.17x | 1.59x |

With the hue rotating - the default, 0.5 - the emission table re-bakes every
frame, the frame leaves the caller's target anyway, and the split never loses.
Still, it loses below ~137k px (a fit of the still column: 1.07 ns of loop per
quad pixel at 128 samples against 0.15 ms fixed). So the direct path splits
when the hue rotates OR the quad's area, times `numSamples / 128`, is at least
`FULL_RES_SPLIT_MIN_AREA_PX` (140k). With the gate the still bands above read
1.00x / 1.01x and the 1600 x 900 one 1.25x. Both inputs are stable from frame
to frame, which matters: choosing per frame from whether the emission table
happened to re-bake would alternate the two paths in a still scene, and they
differ by 1/255.

The constants are this GPU's. The fixed cost is the driver's and the
rasteriser's, and a tiler's will differ - re-measure both on target hardware.

### 10.2 What the shading costs

With the loop gone from the quad, the shading is the pass. Terms stubbed one
at a time in the split program, 1920 x 1080:

| what was removed | default | sharp_corners | arcs | screen-edge band |
| ---------------- | ------: | ------------: | ---: | ---------------: |
| nothing | 2.29 ms | 1.37 | 2.77 | 0.373 |
| the four corner pieces | 1.66x | 1.00x | 1.61x | 1.41x |
| the straights' bloom | 1.36x | 1.86x | 1.24x | 1.24x |
| the straights' halo | 1.03x | 1.05x | 0.96x | 1.03x |
| `perimeterPosition` | 1.03x | 1.06x | 1.03x | 1.03x |
| the filament | 1.00x | 1.01x | 1.00x | 1.01x |
| everything after the gather read | 6.70x | 4.09x | 5.34x | 2.94x |

(The screen-edge band is a 1840 x 1000 rect, `GlowSide::OUTSIDE`, outside
cutoff 20 px - the production target.) The corner pieces and the straights'
bloom are most of it, and both are `atan`s: the straights evaluate
`bloomSegment` twelve times (four lines, each against its own pedestal), the
corners another eight times with a development `atan` each.

### 10.3 One `atan` per bloom segment

`atan(t2/c) - atan(t1/c)` is the angle the segment subtends, and
`atan(c * (t2 - t1), c^2 + t1 * t2)` is the same angle as one two-argument
`atan`: exact, since c > 0 and t2 >= t1 put it in [0, PI]. 1.13-1.26x on the
shading pass; 0-16 pixels a frame move by 1/255 from the rounding.

### 10.4 Skipping pieces that add nothing

- **A straight's bloom past `reach`.** `bloomSegment` falls monotonically with
  the distance `a` from its line, so at `a >= reach` the segment is no
  brighter than its own pedestal and the clamp makes it 0. Skipping it there
  changes no pixel (measured byte-identical on fifteen scenes). The form
  matters: a ternary on the old call cost `soft_wash` - whose reach covers
  the frame, so nothing skips - 5%; computing each pair's shared pedestal once
  and skipping only the segment costs it nothing, for 1.04-1.19x elsewhere.
- **A corner arc, whole, far from its circle** (`uCornerSkip`,
  `GetCornerSkip`). Every point of an arc's development is at least
  `length(w) - r` from the fragment, so the closed forms `GetGlowInnerReach`
  already bounds the interior with - halo `pi r kh^2 / (2 c^3)`, bloom
  `pi r bw / (2 c^2)` less the shared pedestal - bound what the arc adds
  there. `uCornerSkip` is where that falls under a quarter of half an 8-bit
  level, so four skipped arcs add less than half a level, and since the tone
  map is concave no pixel moves by more than the rounding of the one it lands
  on: measured 0-7628 pixels per frame at 1/255, never 2. The bloom's own zero
  is what places it (320 px at the defaults, `reach` 312); a stricter budget
  runs into the halo's 1/c^3 tail and moves it out fast (355 px at 1/16, 563
  at 1/64), so a quarter it is. 1.22-1.47x on the shading pass where it
  engages.

The two bounds share one CPU mirror of the shader's halo and bloom terms
(`GlowBoundTerms`, `GetCornerArcBound`), so changing those terms has one place
to follow it. A straight cannot be skipped whole the same way: its halo's
1/a^2 tail needs ~2x `reach` to fall under the budget, and the halo is cheap.

### 10.5 Result

`neon-scale-check time`, AMD Radeon Pro 5300M, `80d71a7` against this
change, three interleaved rounds, median. The check scenes freeze the hue (rate 0),
so these are the split's least favourable case - the gate of 10.1 decides on
area alone.

1920 x 1080, ms (before -> after):

| scene | 1.0 | 0.5 | 0.25 |
| ----- | --: | --: | ---: |
| default | 4.97 -> 1.23 (4.04x) | 1.04 -> 0.68 (1.52x) | 0.64 -> 0.47 (1.36x) |
| hairline | 3.27 -> 0.77 (4.23x) | 0.80 -> 0.52 (1.53x) | 0.55 -> 0.39 (1.40x) |
| crisp_tube | 4.14 -> 0.99 (4.19x) | 0.91 -> 0.60 (1.52x) | 0.60 -> 0.42 (1.44x) |
| soft_wash | 6.01 -> 2.29 (2.62x) | 1.23 -> 1.07 (1.15x) | 0.77 -> 0.67 (1.15x) |
| sharp_corners | 4.18 -> 0.95 (4.39x) | 0.68 -> 0.55 (1.24x) | 0.45 -> 0.40 (1.14x) |
| small_rect | 1.94 -> 0.75 (2.58x) | 0.64 -> 0.51 (1.25x) | 0.46 -> 0.38 (1.24x) |
| glow_inside | 1.55 -> 0.67 (2.30x) | 0.55 -> 0.48 (1.16x) | 0.49 -> 0.38 (1.29x) |
| card_outside | 4.71 -> 1.51 (3.12x) | 1.04 -> 0.76 (1.37x) | 0.61 -> 0.51 (1.21x) |
| bounded_band | 0.28 -> 0.23 (1.19x) | 0.50 -> 0.41 (1.22x) | 0.40 -> 0.36 (1.11x) |
| arcs | 5.44 -> 1.64 (3.31x) | 1.18 -> 0.79 (1.50x) | 0.71 -> 0.51 (1.39x) |
| segments | 5.46 -> 1.46 (3.73x) | 1.12 -> 0.77 (1.45x) | 0.69 -> 0.52 (1.33x) |
| overdrive | 6.01 -> 2.28 (2.63x) | 1.21 -> 1.04 (1.16x) | 0.71 -> 0.64 (1.11x) |

1280 x 720, ms (before -> after):

| scene | 1.0 | 0.5 | 0.25 |
| ----- | --: | --: | ---: |
| default | 2.70 -> 0.80 (3.37x) | 0.69 -> 0.51 (1.37x) | 0.49 -> 0.38 (1.27x) |
| hairline | 2.05 -> 0.55 (3.74x) | 0.59 -> 0.40 (1.47x) | 0.44 -> 0.32 (1.39x) |
| crisp_tube | 2.32 -> 0.66 (3.54x) | 0.64 -> 0.46 (1.37x) | 0.46 -> 0.34 (1.37x) |
| soft_wash | 2.70 -> 1.10 (2.45x) | 0.72 -> 0.64 (1.13x) | 0.52 -> 0.47 (1.10x) |
| sharp_corners | 2.29 -> 0.59 (3.91x) | 0.46 -> 0.41 (1.14x) | 0.34 -> 0.33 (1.00x) |
| small_rect | 1.66 -> 0.78 (2.13x) | 0.64 -> 0.57 (1.12x) | 0.39 -> 0.37 (1.06x) |
| glow_inside | 0.70 -> 0.40 (1.76x) | 0.47 -> 0.39 (1.21x) | 0.40 -> 0.29 (1.39x) |
| card_outside | 2.15 -> 0.85 (2.53x) | 0.62 -> 0.51 (1.20x) | 0.46 -> 0.41 (1.13x) |
| bounded_band | 0.18 -> 0.16 (1.18x) | 0.36 -> 0.34 (1.07x) | 0.37 -> 0.30 (1.21x) |
| arcs | 2.98 -> 1.06 (2.80x) | 0.76 -> 0.58 (1.31x) | 0.52 -> 0.40 (1.30x) |
| segments | 3.01 -> 0.95 (3.19x) | 0.73 -> 0.56 (1.31x) | 0.51 -> 0.40 (1.28x) |
| overdrive | 2.69 -> 1.10 (2.45x) | 0.71 -> 0.63 (1.12x) | 0.50 -> 0.44 (1.14x) |

Below 1.0 the gain is 10.3-10.4 alone: pass 1b and the edge ring run the same
shading. With the hue rotating at the default 0.5 - the case the check scenes
do not cover - one interleaved session (four rounds, 200 frames) read, at 1.0:

| scene | still | hue rotating |
| ----- | ----: | -----------: |
| the screen-edge band of 10.2 | 0.522 -> 0.227 ms (2.30x) | 0.522 -> 0.250 ms (2.09x) |
| default | 4.966 -> 1.232 ms (4.03x) | 4.974 -> 1.260 ms (3.95x) |
Construction and initialisation are unchanged (7.3 -> 7.1 ms median at 1080p); a frame
that splits compiles two programs on first use where the inline path compiled
one, as the scaled path always has. The gather buffer at 1.0 is small - 144 x
96 RGBA16F, 110 KB, for the default scene at 1080p - and never larger than the
scaled path's at 0.5.

Verification, all on the AMD:

- `neon-scale-check check`: PASS; 1.0 at most 1/255 off the committed images,
  every reduced scale within its bound, the hairline sweep unchanged
  (0.021-0.050 px).
- `neon-scale-check partition --seed 1`: PASS, 1000 configs, no overlap, no
  gap (the partition is untouched, but `setupRingGeometry` changed).
- Each change isolated against the build without it, fifteen scenes: the
  straight skip byte-identical everywhere; the corner skip and the single
  `atan` at most 1/255; the whole change against `80d71a7` at most 1/255.
- A long-lived effect against a fresh one through twelve steps - split, inline,
  hue on and off, 0.5, 0.25, disabled and re-enabled, segments, a small rect -
  byte-identical at every step, at 1080p and 720p. No WARN or ERROR line.

### 10.6 What is left open

- **One GPU.** Every number here is the AMD 5300M's. The split's gate rests on
  a fixed cost (~0.15 ms for the first offscreen pass of a frame) that is the
  driver's and the rasteriser's; on a tiler it is a tile store and load of a
  small target, and `FULL_RES_SPLIT_MIN_AREA_PX` should be re-measured there
  and on the Apple M2 Pro before it is trusted. The attribution in 10.2 is the
  part most likely to reorder on another GPU.
- **The shading is the cost now.** At the defaults, 1080p, about 0.34 ms of
  the 1.23 is the floor (rasterising the quad, the gather read, the SDF, the
  masks and the blend), and most of the rest is the corner development's
  `atan` and the straights' remaining bloom near the line. Below 1.0 the same
  shading is pass 1b and the ring.
- **The gather pass re-runs on still frames.** Like the emission table, its
  output is a pure function of the config and the time, so it could be
  skipped when neither moved. That would remove the fixed cost of 10.1 in a
  still scene - the case the area gate exists for - at the price of one more
  staleness gate.

  > **Later:** done, with the reduced-scale shading pass alongside it -
  > [`neon-perf-plan.md`](neon-perf-plan.md) item 4 (`mOffscreenCurrent`).
  > The area gate stays: an animated frame still leaves the caller's target.
- **A rect small enough to keep the loop inline** (gather scale above 0.5)
  pays the full loop, as before; the two-level gather of section 6 is still
  the move there.
- **Docs that describe 1.0 as one inline pass**:
  `docs/neon-onboarding-guide.md`, `docs/neon-shader-outputs.html` and the
  comparison page's 1.0 images (1/255 off, inside `check`'s bound).
  `tools/neon-guide-figures` keys its direct-path figures on the inline
  program (`PassKind::P1`), so where its scene splits it now skips
  `pass-p1-direct.png` and draws `geometry-direct.png` with no overlay.

### 10.7 Status (2026-10-06)

| item | state |
| ---- | ----- |
| The split at 1.0, its gate, the single-`atan` bloom, both piece skips, `CLAUDE.md` | landed in `cdcc19c` |
| 10.5's tables at three rounds, the hue-rotating table, 10.7 and 10.8 | in the working tree, not committed |
| `neon-scale-check check` / `partition --seed 1` | PASS / PASS |
| Each change isolated, fifteen scenes; long-lived against fresh effect, twelve steps | at most 1/255; byte-identical |
| Full build - library, C ABI, both demos, both tools | clean, no new warnings |
| AMD Radeon Pro 5300M | every number in this section |
| Apple M2 Pro | measured since, in [`neon-perf-plan.md`](neon-perf-plan.md): `check` PASS with the 1.0 column at its bound (2 on two scenes), 4.22-6.33x over `80d71a7` on still frames at 1.0 |
| The Intel UHD 630 in the same Mac, the target device | **not measured** |
| Either demo run on screen | **not done** - offscreen captures only |
| `neon-guide-figures` re-run, comparison page regenerated, onboarding guide / `neon-shader-outputs.html` updated | **not done** (10.6) |

### 10.8 Open questions

Each is a call for the owner rather than for a measurement this machine can
make. The recommendation comes first; the rest is what it trades.

1. **Which GPU sets the split's gate?** `FULL_RES_SPLIT_MIN_AREA_PX` (140k)
   and `FULL_RES_SPLIT_GATHER_MAX_SCALE` (0.5) are this AMD's crossovers, and
   rest on a ~0.15 ms cost for leaving the caller's framebuffer that belongs
   to the driver. *Recommended: calibrate on the target device* - run
   `neon-scale-check time` there before and after `cdcc19c`, plus a still
   cutoff band at a few sizes split and inline (10.1's table). A lower fixed
   cost there means a lower area bound, and the hue-rotating case needs no
   bound at all. The M2 Pro is the next best check; it is the machine the
   earlier sections were timed on.

   > **Later:** measured on the M2 Pro
   > ([`neon-perf-plan.md`](neon-perf-plan.md) section 10.2): the first
   > offscreen pass there costs ~6 us and the split won at every size and in
   > every mode, 1.1-4.75x where this gate kept the loop inline. The owner
   > chose the M2's values, 1.0 and 0 - every rect splits at 1.0 - and the
   > AMD's 0.5 and 140000 are recorded in `neon-tuning.h` for a GPU that needs
   > them back (section 10.6 of the plan).
2. **Does production run with the hue still?** The gate splits on any
   `hueRotationRate != 0`, and with the hue still only on area. If the
   production config is still, two follow-ups get more valuable: skipping the
   gather pass on frames whose inputs did not move (10.6), and calibrating the
   area bound on the target. *Recommended: tell me the production
   `hueRotationRate`, and whether animations are usually attached* - an
   attached animation re-bakes the emission table every frame too, which the
   gate does not currently count.
3. **Is 1/255 at scale 1.0 acceptable, and should the comparison page's 1.0
   images be regenerated?** `check` reads 1 against a bound of 2 on every
   scene, so the headroom for GPU-to-GPU variance on that column has halved.
   *Recommended: accept, and regenerate the page* (its README's four steps,
   one `--images` run plus three interleaved timing runs per build), so `check`
   measures drift from the current 1.0 again rather than from the old one.
4. **Keep a way to force the inline loop at 1.0?** There is none now: the
   exact path is reachable only through a config the gate keeps inline. It
   would be a reference for future comparisons and an escape hatch if a
   driver mis-renders the split. *Recommended: no new `NeonConfig` field* (it
   would need `operator==`, a C ABI setter and getter, and both demos), but a
   compile-time switch on `FULL_RES_SPLIT_GATHER_MAX_SCALE` (0 disables the
   split) is already enough for a reference build, and it is what the
   measurements here used.
5. **Is the gather buffer's memory at 1.0 acceptable?** 110 KB for the default
   scene at 1080p. Its bound is the scaled path's at 0.5: about 4 MB at 1080p
   (8 MB with segments) for a small rect whose glow covers the whole frame,
   where 1.0 used to allocate nothing. *Recommended: accept* - only that
   corner case is large, and lowering `FULL_RES_SPLIT_GATHER_MAX_SCALE` caps it
   at the price of the gains in 10.1's first table.

   > **Later:** the max scale is now 1.0 (question 1's note), which lifts that
   > cap: a tiny rect whose glow fills the frame holds 16 MB of gather buffer
   > at 1080p, 33 MB with segments, where this answer assumed ~4 / 8 MB.
   > Measured in [`neon-perf-plan.md`](neon-perf-plan.md) section 10.6.
6. **What is next for speed?** The neon at 1.0 is now 2-4x cheaper, and the
   production band costs 0.25 ms. *Recommended: measure the whole production
   frame - every layer the host enables - on the target before choosing*,
   since the neon may no longer be the largest layer. The candidates:
   - the neon's shading pass (10.6: the 0.34 ms floor at the defaults, the
     corner development's `atan`);
   - skipping the gather pass on still frames (question 2);
   - the two-level gather of section 6, for rects that stay inline;
   - the lens flare's hex sprite gating, still open from section 9 at about
     1.7x on that layer.

   > **Later:** answered for the neon by
   > [`neon-perf-plan.md`](neon-perf-plan.md), measured on the Apple M2 Pro.
   > The largest costs left turned out not to be in the shading: work repeated
   > on frames where nothing moved (skipping the gather and the reduced-scale
   > shading there was prototyped byte-identical, 2.0-6.0x on still frames at
   > 0.5 and 1.15-1.66x at 1.0), and the glow coverage table re-baking in full
   > every animated frame (~0.15 ms with one travelling segment; re-baking only
   > the changed pieces is exact). The second bullet above is its item 4.
7. **Who updates the docs that still describe 1.0 as one inline pass?**
   *Recommended: a separate change* - re-run `neon-guide-figures` (after
   teaching it to find pass 1 as `PassKind::P2C` when the split engages),
   then revise the onboarding guide's pass chapter and
   `neon-shader-outputs.html` from what it writes.
