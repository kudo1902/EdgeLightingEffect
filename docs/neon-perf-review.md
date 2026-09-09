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

## 9. What is left open

- **The gather loop is still 95% of the layer.** Change 1 removed a fetch from
  it; it did not change its shape. The two-level gather in section 6 is the next
  real move, and it is unbuilt.
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
