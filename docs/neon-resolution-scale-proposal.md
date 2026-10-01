# Matching the 1.0 reference at `resolutionScale` 0.5 and 0.25

A proposal for making the neon layer's reduced-resolution path render like the
full-resolution one, under the constraint that render time comes first. Targets
are `NeonConfig::resolutionScale` 0.5 and 0.25.

[`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html)
is the evidence this starts from: twelve scenes at six scales, diffed against
their own 1.0 render. This document is what to do about it, and
[`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md) is how to
build it, step by step. Nothing here is implemented yet. The quality numbers come from an emulation of the design
(section 8), and the cost numbers from timing the unmodified library.

**Result: render the glow at the reduced scale as today, and re-shade only a
thin ring around the rect edge at full resolution, reading the gather's output
from the reduced buffer.** Emulated on all twelve scenes, that stays within
max 2/255 of the 1.0 render at scale 0.5 and max 4/255 at 0.25, against today's
worst of 77 and 93. On the default scene it costs an estimated 3.1-3.3x less
than 1.0 at scale 0.5 and 7.9-9.5x less at 0.25, against today's 3.3x and 10.4x.

## 1. Why the scaled path drifts

Below 1.0, `neon.frag` draws the WHOLE layer into a buffer `scale` times the
viewport, and `neon-blit.frag` bilinearly upsamples it. The soft parts of the
glow survive that round trip. The sharp parts cannot, because the buffer never
held them:

- **The filament.** A line narrower than the buffer can sample would land
  wherever the edge falls between texels, so `neon.frag` floors the filament's
  sigma at a buffer-pixel Nyquist width (`FILAMENT_NYQUIST_*`). At 0.25 that
  turns a 1 px line into one 8.9 px wide at half maximum (3.1 px at 1.0), and a
  moving rect wanders up to +/-0.53 px off its true position.
- **The cutoffs.** `insideCutoff` / `outsideCutoff` are applied inside the
  reduced buffer. `neon-blit.frag` re-applies only the one-sided glow cut at
  destination resolution, so the cutoff edges blur by a buffer texel.
- **Flat-top profiles.** A high `filamentFalloff` has hard shoulders, which
  bilinear reconstruction rounds off at any reduced scale.

What makes a cheap fix possible is where the time goes. Per fragment, the
128-sample colour gather is the expensive part, and everything it produces is
SMOOTH: the gathered hue `col`, the segment hue `segColHue`, and the two
gathered coverages `emitCoverGathered` / `segCoverGathered`. The gather kernel
is a Lorentzian `COLOR_BLEND_PERIM_FRAC` of the perimeter wide, about 17 px on
the default geometry, so these vary slowly across the screen. Everything with sharp structure (the filament
core, the halo and bloom closed forms, the pointwise arc coverage, the tone map,
every mask) is cheap analytic math per pixel.

Measured, at 1280 x 720: rendering at scale 1.0 with `numSamples` 1, which is
everything except the gather, costs 18-27% of a full frame on the rounded-rect
scenes and 15% on the sharp-cornered one. The four corner-arc terms are the
difference. So the gather is roughly three quarters of the frame or more.

## 2. The proposal: low-res glow, full-res edge ring

| pass | resolution | geometry | does |
| ---- | ---------- | -------- | ---- |
| 0 | - | `N x 2` table | emission pre-pass, unchanged |
| 1 | `scale` | glow quad, as today | today's gather + shading into the colour target, PLUS the gather's outputs into one extra target (two with segments) |
| 2a | full | fullscreen, as today | the blit, now SKIPPING `abs(d) <= R`, and applying the inside/outside cutoffs at destination resolution as it already does the one-sided cut |
| 2b | full | annulus `abs(d) <= R` | the full `neon.frag` shading, with the gather loop replaced by bilinear reads of pass 1's gather target(s) |

Outside the ring, today's path is already within 1-2 levels of the reference:
the glow there is low-frequency, which is why `soft_wash` and `overdrive` match
to 4/255 even at 0.125. Inside it, the expensive input comes from the buffer and
everything sharp is computed at the destination rate. The ring is 3-5% of a
1280 x 720 frame on the default geometry, so its shading cost is small.

### 2.1 Pass 1: the gather target

One RGBA8 attachment beside the colour target: `col.rgb` and
`emitCoverGathered` encoded as `c / (1 + c)`. The coverage is not bounded by 1,
because arc intensity and segment boost both fold into it, and that encoding
keeps it in range with no float target. A second RGBA8 attachment carries
`segColHue.rgb` and `segCoverGathered` the same way, written only when
`uSegmentCount > 0`. That is the same uniform branch the gather loop already
takes.

Multiple render targets are core in GL 3.3 and GLES 3.0, and RGBA8 keeps the
library's rule against float textures. The emulation includes 8-bit storage; it
adds about 0.4 levels of mean error and no more than 1 level of max error. If
that ever shows as banding, `RGB10_A2` is also renderable on GLES 3.0.

The gather target must hold valid values everywhere the ring's bilinear reads
can reach, which is one buffer texel past `R`. Today pass 1 discards the dark
side of a one-sided glow from `BLIT_SIDE_GUARD_PX` past the line, and everything
past the cutoffs. Both discards need to leave at least one buffer texel of guard
for the gather target, as `BLIT_SIDE_GUARD_PX` already does for the colour
target.

### 2.2 Pass 2b: the ring

Geometry is an annulus around the rect, `|d| <= R`, bounded by geometry like
`setupFillGeometry`'s rect annulus rather than by a fullscreen discard. The
fragment program is `neon.frag` itself, with the gather loop behind a uniform
branch that reads pass 1's target(s) with `texture()` instead. That keeps one
copy of the shading, so the ring and the direct path cannot drift.

Inside the ring the shader runs at the destination rate, so:

- the Nyquist filament floor is OFF, as on the direct path today;
- the one-sided cut and the cutoffs are applied in the shader, below the grade,
  exactly as at 1.0.

### 2.3 Pass 2a: the blit

Two changes. It skips `|d| <= R`, computing `d` with the same expression as the
ring, so the two passes partition every pixel exactly once. Both composite
premultiplied, so a pixel drawn by both would composite twice. It also applies
the inside/outside cutoffs at destination resolution, mirroring what it already
does for `glowSide`. Section 5 measures that change on its own.

### 2.4 The ring width `R`

`R` has to cover the filament's reach at both resolutions plus the bilinear
footprint of one buffer texel. The emulated sweep, worst max error over all
twelve scenes (`bounded_band` with its cutoffs moved to the blit):

| `R` (px) | 4 | 6 | 8 | 12 | 16 | 24 |
| -------- | - | - | - | -- | -- | -- |
| scale 0.5 | 41 (`crisp_tube`) | 17 (`crisp_tube`) | **2** | 2 | 2 | 2 |
| scale 0.25 | 68 (`hairline`) | 39 (`crisp_tube`) | 8 (`crisp_tube`) | **4** (`small_rect`) | 4 | 4 |
| share of frame, default geometry | 1.7% | 2.5% | 3.4% | 5.0% | 6.7% | 10.1% |

So `R` is 8 px at 0.5 and 12 px at 0.25 for these scenes. The candidate rule,
still to be checked against a wider `lineWidth` range, is
`R = filamentReach + 1 / scale`. Here `filamentReach` is the
`sigma * reachSigmas` that `setupGeometry` already computes to size the quad.
That gives about 9 px at 0.5 and 11 px at 0.25 for the defaults, and 10 and
12 px for `crisp_tube`.

## 3. Quality (emulated)

Error against the scene's own 1.0 render, in 8-bit levels. "Today" columns are
p99 over visibly lit pixels / max over the whole frame, on the current tree. The
ring and split-only columns are max over the whole frame.

| scene | today 0.5 | **ring, 0.5** | today 0.25 | **ring, 0.25** | split only, 0.5 / 0.25 |
| ----- | --------- | ------------- | ---------- | -------------- | ----------------------- |
| `default` | 4 / 7 | **2** | 9 / 24 | **2** | 2 / 2 |
| `hairline` | 59 / 77 | **1** | 86 / 93 | **1** | 1 / 1 |
| `crisp_tube` | 25 / 41 | **1** | 43 / 60 | **1** | 1 / 1 |
| `soft_wash` | 1 / 1 | **1** | 1 / 2 | **2** | 1 / 1 |
| `sharp_corners` | 4 / 13 | **2** | 8 / 29 | **2** | 2 / 2 |
| `small_rect` | 8 / 11 | **2** | 30 / 47 | **4** | 2 / 4 |
| `glow_inside` | 3 / 4 | **1** | 7 / 11 | **1** | 1 / 1 |
| `card_outside` | 2 / 5 | **1** | 2 / 15 | **1** | 1 / 1 |
| `bounded_band` | 41 / 43 | **2** | 69 / 72 | **2** | 2 / 2 |
| `arcs` | 4 / 7 | **1** | 7 / 26 | **2** | 1 / 1 |
| `segments` | 6 / 8 | **1** | 16 / 27 | **1** | 1 / 1 |
| `overdrive` | 1 / 1 | **1** | 2 / 2 | **1** | 1 / 1 |

"Ring" is the proposal: `R` 8 at 0.5 and 12 at 0.25. "Split only" is section
6's variant, which shades the WHOLE quad at full resolution. Its numbers are the
ceiling the ring is measured against: the ring matches it everywhere except
`small_rect` at 0.25, where both read 4.

The design also holds at 0.125: split-only stays within max 2 on eleven scenes
and max 11 on `small_rect`, whose whole rect is 20 x 12 buffer texels there.
That scale is outside this proposal's targets.

## 4. Cost

GPU time for the neon layer alone, 1280 x 720, Apple M2 Pro, ms. All columns come
from one interleaved session, min over eight rounds of 30 frames, so read the
ratios rather than the absolute times.

| scene | 1.0 | today 0.5 | **ring, 0.5** | today 0.25 | **ring, 0.25** |
| ----- | --- | --------- | ------------- | ---------- | -------------- |
| `default` | 1.87 | 0.56 (3.3x) | **0.57-0.61 (3.1-3.3x)** | 0.18 (10.4x) | **0.20-0.24 (7.9-9.5x)** |
| `hairline` | 1.58 | 0.47 (3.4x) | 0.47-0.52 (3.1-3.3x) | 0.18 (8.7x) | 0.19-0.24 (6.6-8.2x) |
| `crisp_tube` | 1.74 | 0.51 (3.4x) | 0.52-0.56 (3.1-3.4x) | 0.18 (9.6x) | 0.20-0.24 (7.3-8.8x) |
| `soft_wash` | 1.89 | 0.57 (3.3x) | 0.58-0.62 (3.0-3.3x) | 0.18 (10.2x) | 0.20-0.24 (7.8-9.5x) |
| `sharp_corners` | 1.73 | 0.52 (3.3x) | 0.54-0.56 (3.1-3.2x) | 0.17 (10.3x) | 0.19-0.21 (8.2-9.2x) |
| `small_rect` | 1.33 | 0.39 (3.4x) | 0.40-0.42 (3.2-3.3x) | 0.18 (7.5x) | 0.19-0.21 (6.3-7.1x) |
| `glow_inside` | 0.56 | 0.26 (2.1x) | 0.27-0.30 (1.8-2.1x) | 0.14 (4.0x) | 0.15-0.19 (3.0-3.7x) |
| `card_outside` | 1.58 | 0.48 (3.3x) | 0.49-0.53 (3.0-3.3x) | 0.19 (8.5x) | 0.19-0.24 (6.7-8.1x) |
| `bounded_band` | 0.18 | 0.13 (1.4x) | 0.14-0.18 (1.0-1.4x) | 0.09 (2.1x) | 0.10-0.14 (1.3-1.8x) |
| `arcs` | 1.94 | 0.58 (3.3x) | 0.59-0.65 (3.0-3.3x) | 0.19 (10.4x) | 0.19-0.25 (7.7-10.1x) |
| `segments` | 2.53 | 0.74 (3.4x) | 0.75-0.79 (3.2-3.4x) | 0.25 (10.2x) | 0.26-0.31 (8.1-9.6x) |
| `overdrive` | 1.88 | 0.57 (3.3x) | 0.58-0.63 (3.0-3.3x) | 0.18 (10.2x) | 0.20-0.24 (7.7-9.6x) |

The ring estimate is a range because it was priced, not built. The ring pass
was timed by reusing the renderer's own cutoffs to bound its quad to
`|d| <= R`, at scale 1.0 and `numSamples` 1:

- **Upper bound:** today's scaled time plus that whole ring `Render()`, about
  0.05-0.06 ms. This double-counts the per-render fixed overhead, around
  0.04 ms, which a ring drawn inside the same `Render()` would not pay again.
- **Lower bound:** today's scaled time plus the ring's AREA cost alone. That is
  measured as the difference between an 8 px and a 16 px ring (about 0.011 ms
  on the default geometry), scaled to the ring's width.

Neither bound includes writing the extra target in pass 1. At 0.25 that is
57,600 texels and should be negligible, but it is not measured.

`glow_inside` and `bounded_band` gain little at any scale, with or without the
ring. Their quads are already bounded by geometry, so there is little for a
reduced scale to save. That is not specific to this proposal.

## 5. Quick wins, independent of the ring

**Apply the cutoffs in the blit.** It is a mirror of the existing glow-side
code, and the blit already computes `d`, so it costs nothing measurable. It is
also step 2a of the proposal. Emulated on its own, with no ring:

| `bounded_band` | today p99 / max | cutoffs in blit p99 / max |
| -------------- | --------------- | ------------------------- |
| scale 0.5 | 41 / 43 | **4 / 5** |
| scale 0.25 | 69 / 72 | **12 / 16** |

**Offer only 1/n scales.** 0.35 measured WORSE than 0.25 on `default`,
`sharp_corners` and `arcs` (p99 12 against 9, 8 and 7), because its buffer
texels do not line up with destination pixels. 0.5 and 0.25 do.

Neither fixes the filament: `hairline` and `crisp_tube` need the ring.

## 6. Alternatives measured and not taken

**Split without a ring.** Gather at the reduced scale, shade the whole quad at
full resolution. It is the cleanest design and its quality is the "split only"
column in section 3: max 2 at both scales. But full-res shading is about a quarter
of a full frame on its own, so it lands at 2.0x at 0.5 and 3.1x at 0.25 on the
default scene. It fails the render-time constraint. The ring exists to keep its
quality without paying that.

**A better upsampling filter** (bicubic, Lanczos, an SDF-guided joint
bilateral). Each reconstructs the buffer more smoothly, but none can recover a
filament the Nyquist floor widened before the buffer was written. It would
reduce staircasing at 0.125, and nothing more.

**Fewer samples, or a windowed gather, at full resolution.** Both were already
measured: `numSamples` is linear in cost but beads the arc gradients (V9 in
[`review-findings.md`](review-findings.md)). The windowed gather tops out at
max 16-22 error whatever the window, because the Lorentzian's far field moves
the normalisation ([`neon-perf-review.md`](neon-perf-review.md) section 6).
Neither matches the reference.

## 7. Risks and open questions

- **The numbers are emulated.** The quality columns are the exact arithmetic of
  the design, evaluated inside one shader (section 8). The real build adds
  culling, guard bands, partition exactness and extra attachments, each of
  which can introduce error the emulation cannot. Re-run the twelve scenes
  against it before trusting section 3.
- **The ring scales with the perimeter, not the area.** On a large rect at 4K
  it is a larger absolute cost, though still a thin ring. Under `GlowSide`
  INSIDE or OUTSIDE it only needs the lit half.
- **Tile-based GPUs.** Pass 2b is one more DRAW, not one more pass: it lands on
  the same framebuffer the blit just drew to, so there is no render-target
  switch and no extra tile load or store. Its cost is its fragments. The one
  switch the scaled path makes, into the reduced buffer and back, exists today.
  Measure on target hardware anyway before the default scale changes.
- **The partition must be bit-exact.** Pass 2a's skip and pass 2b's discard
  must compute `d` with the identical expression on the identical inputs.
  Otherwise a pixel on the boundary composites twice or not at all.

## 8. Method

A standalone harness against `libedge-lighting.a` at `51c4bb0`, rendering the
neon layer alone through `OffscreenCapture` at 1280 x 720. Hue rotation and the
colour cross-fade are off, so every render is deterministic. Like the harnesses
behind the other comparison documents, it is scaffolding and is not checked in.

**The split emulation** ran in a scratch worktree, with `neon.frag` changed so
that at scale 1.0 the gather is evaluated at the four reduced-buffer texel
centres around each pixel and blended bilinearly with that pixel's weights.
That is exactly what sampling a reduced-resolution gather target returns. The
8-bit variant quantises each corner's hue to 8 bits and its coverages through
`c / (1 + c)` before blending, then decodes. With the emulation off, the
modified shader matched the unmodified one to within 1 level on every scene,
which is the compiler re-associating the moved loop.

**The ring** was composited offline from two captures: the emulated split
inside `|d| <= R`, and today's scaled render outside it. `d` is the rounded-box
SDF, evaluated on the CPU at each pixel centre. For "cutoffs in the blit", the
outside part is a scaled render with the cutoffs disabled, masked at full
resolution with the shader's own cutoff ramps (`inMid` / `outMid`, a 1 px floor),
composited as `m * out + (1 - m) * background`. That is exact for premultiplied
output.

**Metrics** are the largest per-channel difference against the 1.0 capture.
"p99" counts visibly lit pixels only, meaning the brightest channel is more than
16 levels above the clear in either image.

**Timing** is `glFinish`-bracketed loops of `Render()`, each variant in a freshly
initialised effect, interleaved across rounds, min taken.
