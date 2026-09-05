# Lens flare: performance review and the five changes it produced

`LensFlareRenderer` was the most expensive layer in the pipeline - 1.6x the
neon and 27x the droplets - for a decorative pass. This document is the
measurement that established that, the attribution that found where the time
went, and the before/after for the five changes that followed.

Design rationale for the individual pieces lives beside the code, in
`lib/shaders/lens-flare.frag` and `lib/include/renderer/lens-flare-tuning.h`;
this document is the evidence.

## 1. What is being compared

| side | state | contains |
| ---- | ----- | -------- |
| before | `75fefb7` "Merge pull request #51" | the flare as ported from the reference shader |
| after | that plus the five changes in section 3 | same picture, ~2x the speed |

Everything below was measured back-to-back in one session on one machine, with
the "before" side rebuilt from a `git stash` of the working tree rather than
recalled from an earlier run.

## 2. The starting measurement

Per-renderer GPU time, each renderer driven alone into an `OffscreenCapture` at
3840x2160:

| layer | ms/frame |
| ----- | -------- |
| **lens flare** | **9.94** |
| neon (emission pre-pass + gather) | 6.21 |
| droplets | 0.37 |
| clear only, flare disabled | 0.43 |

At 1920x1080 the flare was still 2.54 ms, about a third of a 60 Hz frame for
one decorative layer.

### Where the time went

Variants of `lens-flare.frag` with individual terms stubbed out, each compiled
and timed the same way (3840x2160):

| what was removed | ms | cost of that piece |
| ---------------- | -- | ------------------ |
| nothing (baseline) | 9.92 | |
| **the entire 10x ghost loop** | 1.20 | **8.72 ms, 88%** |
| `hexCoverage()` only | 5.64 | **4.28 ms, 43%** |
| `c1` ring term | 8.60 | 1.32 ms, 13% |
| per-ghost `cos()` palette | 9.00 | 0.93 ms, 9% |
| `c` bloom term | 9.03 | 0.90 ms, 9% |
| all sun core / haze / glow / rays | 9.23 | 0.69 ms, 7% |

The sun - the thing the renderer is named for - was 7% of the cost. 88% was the
ghost loop, and nearly half the whole shader was a single `atan` inside it.

One more number from the same session, because it names a defect rather than a
cost: `spread = 0`, which is the documented way to suppress ghosts entirely,
measured **9.95 ms**. The uniform multiplied the loop's result instead of
skipping it.

### What was checked and found NOT to be the problem

The droplets pass earns its speed by drawing a band-fitted ring instead of a
fullscreen quad, so the same question was asked here. Alpha coverage of a real
frame: **100% of pixels exceed 1/255**. The flare's global `exp(1 - sunDist)`
envelope never reaches zero inside the viewport, so the fullscreen quad is
honest and there is no geometry win available. The cost had to come out of
per-fragment work, which is what every change below does.

## 3. The five changes

| # | change | output |
| - | ------ | ------ |
| 1 | `hexCoverage` computes the hexagon's support distance as a `max` of three `abs`-ed dot products instead of `atan` -> snap -> `cos` -> `length` | byte-identical |
| 2 | the bloom and ring terms are gated on the exact bound outside which each is provably zero, derived once per frame on the CPU | byte-identical |
| 3 | the per-ghost distance and palette, pure functions of the ghost index and the config, are baked into the std140 `GhostBlock` | shifts placement in the 3rd decimal, section 5 |
| 4 | the whole ghost loop is skipped when `uSpread` is zero | byte-identical |
| 5 | `LensFlareRenderer::mCurrentConfig`, assigned and never read, removed | byte-identical |

Changes 1 and 2 are value-preserving rewrites. 3 is the only one that moves a
pixel, and it moves them for a reason that turns out to be a portability
finding in its own right - see section 5.

### Why 2 needed a tuning header

The bounds are solved on the CPU from constants that live in the shader terms,
which is exactly the coupling `droplets-tuning.h` exists to prevent. So change
2 also added `lib/include/renderer/lens-flare-tuning.h`, injected into
`lens-flare.frag` via `@LENS_FLARE_TUNING@` and `#include`d by the renderer,
following the droplets and neon precedent. Both derivations are written out in
that header, next to the constants they consume.

The two bounds:

```
BLOOM   c > 0  <=>  pow(lq, size * 1.4) < 0.01  <=>  lq < 0.01^(1 / (size * 1.4))
        Exact both ways: pow is strictly increasing for a positive exponent.

RING    c1 > 0 <=>  sin(l * 30) > pow(l - 0.3, 1/40) - 0.001
        l >= size * 0.5 and pow is increasing, so the right-hand side never
        falls below pow(size * 0.5 - 0.3, 1/40) - 0.001. Testing the cheap sin
        against that constant first errs only toward running the term.
```

### Why 3 uses a uniform block and not a uniform array

`uniform vec4 uGhosts[10]` is the obvious way to hand ten vec4s to a shader,
and it is the wrong one here: the bare uniform-array form is not available on
the restricted GL targets this library ships against. Every per-index array in
this tree is packed into a `layout(std140) uniform` block instead -
`LoopSamplesBlock`, `SegmentBlock`, `ArcBlock` in the neon shaders, and now
`GhostBlock` - uploaded through the `UniformBuffer` wrapper and bound to its
own binding point. The array form was tried first and measured identical on
this desktop GL machine, in both output and time, which is exactly why the
constraint has to be written down rather than discovered by testing.

### The one thing that was deliberately NOT gated

`hexCoverage` calls `fwidth`, and a derivative taken in non-uniform control
flow is undefined in GLSL. Gating the hex sprite the way the other two terms
are gated measured a further 1.7x and is the obvious next move, which is
exactly why the shader carries a comment saying not to make it. The `uSpread`
guard in change 4 is safe for the opposite reason: it branches on a uniform, so
every invocation in the draw takes the same side and the control flow stays
uniform by definition.

## 4. Performance

Same method as section 2, end-to-end through `LensFlareRenderer::Render`.

### 3840x2160

| scene | before | after | speedup |
| ----- | ------ | ----- | ------- |
| default | 9.94 ms | 5.04 ms | **1.97x** |
| `spread = 0` (ghosts suppressed) | 9.95 ms | 1.17 ms | **8.5x** |
| `resolutionScale` 0.5 | 3.29 ms | 2.10 ms | 1.57x |
| `resolutionScale` 0.25 | 1.34 ms | 1.04 ms | 1.29x |

### 1920x1080

| scene | before | after | speedup |
| ----- | ------ | ----- | ------- |
| default | 2.54 ms | 1.33 ms | **1.91x** |

### Accumulating, at 3840x2160

```
before            9.94 ms
changes 1 + 2     6.73 ms   (1.48x, byte-identical)
+ change 3        5.02 ms   (1.98x)
+ changes 4 + 5   5.04 ms   (unchanged at spread 1.0, 4.2x at spread 0)
```

The speedup is flat across resolution - 1.97x at 4K and 1.91x at 1080p - which
is what a pure per-fragment ALU win should look like. Nothing here scales with
anything but shaded fragments.

### The result that matters

The flare is no longer the pipeline's most expensive layer. Against the same
neighbours as section 2, at 3840x2160:

```
before   flare 9.94  >  neon 6.21  >>  droplets 0.37
after    neon 6.19   >  flare 5.04  >>  droplets 0.37
```

And a host that turns ghosts off now pays 1.17 ms rather than 9.95 ms, which
makes `spread` an actual performance control instead of a purely visual one.

## 5. Visual comparison

Frames captured through the real renderer at 1920x1080 (8,294,400 bytes/frame),
deterministic scene, fixed clock, the same config on both sides. The sweep
covers the demo sliders' `ghostSize` range plus two values only the C ABI can
reach, because the gates in change 2 are functions of exactly that field.

### Changes 1 + 2, and 4 + 5

| scene | differing bytes | max delta |
| ----- | --------------- | --------- |
| ghostSize 1.0 / 2.2 / 3.5 / 5.0 | 1 / 0 / 2 / 0 | 1 |
| ghostSize 0.5 / 0.0 (below slider range) | 1 / 1 | 1 |
| spread 0, half-res | 0 / 0 | 0 |

Byte-identical for all practical purposes: at most two bytes in a frame, off by
one LSB, from the `atan` rewrite's float noise. Changes 4 and 5 measured **zero
differing bytes in every scene**, `spread = 0` included - which is the scene
where change 4's branch actually fires, so that is a real confirmation and not
a vacuous one.

### Change 3

| scene | differing | > 4/255 | > 8/255 | max delta |
| ----- | --------- | ------- | ------- | --------- |
| ghostSize 1.0 | 9.50% | 0.454% | 0.013% | 10 |
| ghostSize 2.2 (default) | 14.73% | 0.039% | 0.000% | 8 |
| ghostSize 5.0 | 33.92% | 0.104% | 0.000% | 7 |
| ghostSize 0.5 | 15.53% | 1.535% | 0.127% | 30 |
| ghostSize 0.0 | 24.65% | 5.236% | 1.537% | 24 |
| spread 0 | 0% | - | - | 0 |

Wide but shallow: a third of the frame can differ while essentially none of it
differs by more than 8/255, because what moves is smooth gradient. The two
degenerate rows are larger for a specific reason - at small `ghostSize` the hex
sprite's feathered edge is the sharpest feature in the image, so a sub-percent
placement shift moves a hard edge by a pixel instead of sliding a gradient.
Neither row is reachable from the demo UI.

Rendered side by side at `ghostSize` 5.0, the row with the largest differing
percentage, the two frames are indistinguishable: same ghosts, same positions,
same colours.

## 6. Why change 3 cannot be byte-identical

The ghost distances come from the reference shader's hash:

```glsl
float rnd(float w) { return fract(sin(w) * 1000.0); }
```

This is precision-chaotic. GLSL guarantees `sin` to only about 2^-11, and
multiplying by 1000 before taking the fractional part turns that slack into a
different value. Dumping what the GPU actually produces, against the same
formula evaluated on the CPU:

| i | GPU `rnd(i*20)` | CPU float64 | CPU float32 |
| - | --------------- | ----------- | ----------- |
| 1 | 0.9451 | 0.9453 | 0.9453 |
| 2 | 0.1137 | 0.1132 | 0.1131 |
| 3 | 0.1922 | 0.1894 | 0.1894 |
| 7 | 0.2431 | 0.2397 | 0.2396 |
| 9 | 0.8549 | 0.8474 | 0.8474 |

Agreement to about three decimals, and no closer - the CPU's own float32 and
float64 paths agree with each other far better than either agrees with the GPU,
which locates the divergence in the GPU's `sin`, not in the port.

The consequence worth recording: **the ghost layout was never portable.** It
had a per-driver answer, and this repository's usual byte-identity bar cannot
be met for this change by anyone, in either direction. Computing the ten
distances once in double on the CPU makes them the same on every GPU for the
first time; the price is that they no longer match what any one GPU used to
produce. Section 5 measures that price.

## 7. What was left open

- **`pow` of a negative base.** `pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP)` in
  `circle()` takes a negative base when `ghostSize < 0.6`, which is undefined
  in GLSL. `GetGhostRingFloor` clamps the CPU-side bound so the gate stays
  correct there, but the shader expression itself is untouched. Closing it
  means clamping in the shader or in `LensFlareConfig`, which is a behaviour
  decision. Tracked as **I13** in `review-findings.md`.
- **Gating the hex sprite.** Worth a further ~1.7x, blocked on the `fwidth`
  rule in section 3. The way through is an analytic edge width - the mapping
  from `gl_FragCoord` to the hex's argument is affine, so `fwidth` has a closed
  form - which measured 1.61x against the original in a prototype but changes
  0.07% of pixels by one LSB. Not attempted here.
- **`rnd(vec2)`** was dead before this work and is now gone with its overload,
  which is noted only so nobody looks for it.

## 8. Method

A throwaway harness linked against `build/lib/libedge-lighting.a`: a hidden
GLFW window, an `OffscreenCapture` at the stated size, `glFinish` inside the
timed region so the numbers are GPU time rather than command submission, mean
of 100 frames after a 20-frame warmup. Per-term attribution used a second
harness that compiles a `.frag` from disk against the same vertex shader and
uniform set, so a stubbed variant differs from the baseline only in the term
under test.

Machine: AMD Radeon Pro 5300M, OpenGL 4.1 ATI-7.1.6, macOS, x86_64 build. Both
sides of every comparison ran back-to-back in one session.

### Caveat

Absolute timings on this machine drift between sessions, so read the ratios
rather than the milliseconds. Every ratio here is within-session and
same-binary-shape, and the two independent resolutions agreeing to within 0.06x
(1.97x and 1.91x) is the main reason to trust them. The `spread = 0` figure is
the exception worth taking literally, because 9.95 -> 1.17 ms is far outside
any plausible session drift.
