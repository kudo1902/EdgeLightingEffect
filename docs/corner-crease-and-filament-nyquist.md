# Two defects in the neon layer, measured

Both of these were reported from the same screenshot: dark diagonal wedges
running in from the four corners, and a `resolutionScale` 0.5 render that does
not match the 1.0 one at `lineWidth` 1 on geometry `(3, 3, 1000, 500)`.

They are unrelated. The first is a physics approximation in the analytic halo
and bloom; the second is a sampling-rate problem confined entirely to the
filament. Both are measured below and both are fixed - section 3 lists what
landed, and sections 1.6 and 2.5 are what deliberately did not.

Every number here comes from the offscreen probes described in section 4,
rendered through `OffscreenCapture` at an explicit size (never the window
backbuffer - see `util/capture-util.h` for why).

---

## 1. The corner wedges: halo and bloom count only the nearest edge

### 1.1 What the code does

[`neon.frag`](../lib/shaders/neon.frag), in the analytic halo + bloom block:

```glsl
float halo  = HALO_NORM_FACTOR  * 2.0 * kh * kh / (ad * ad + kh * kh);
float bloom = BLOOM_NORM_FACTOR * PI * bw / sqrt(ad * ad + bw * bw);
```

`ad = abs(sdRoundBox(vPos, halfSize, uCornerRadius))`.

Both expressions are the closed form of a line integral over an **infinite
straight emitter** at perpendicular distance `ad`. Inside the shape the
rounded-box SDF returns the distance to the **nearest** edge, so a fragment is
lit as though exactly one infinite edge existed, at whichever distance the
nearest one happens to be.

That is exact nowhere except directly off the middle of a long edge. It is
worst on the medial axis - the four corner diagonals plus the central spine -
where two edges are equidistant and only one of them is counted.

### 1.2 The measurement

Flat white ring (one colour stop, so brightness is the only variable), rect
1000x500 at cornerRadius 40, `glowRadius` 60, `bloomStrength` 1.0,
`lineWidth` 4. Two probe points at matched `ad`: one inside the left edge at
mid-height (one edge nearby), one on the top-left corner diagonal (two edges
nearby, equidistant).

| ad (px) | one nearby edge | two nearby edges | ratio |
| ------- | --------------- | ---------------- | ----- |
| 20 | 197 | 198 | 1.005 |
| 40 | 192 | 192 | **1.000** |
| 60 | 187 | 187 | **1.000** |
| 80 | 183 | 183 | **1.000** |
| 100 | 179 | 179 | **1.000** |
| 120 | 176 | 176 | **1.000** |

The ratio is exactly 1.000. Two edges deliver the light of one. Physically the
diagonal should read roughly twice as bright, and the shortfall is the dark
wedge.

Sweeping a constant-radius arc from the corner into the interior (0 degrees =
down the left edge, 90 = along the top edge, 45 = the diagonal):

| R | 0 | 15 | 30 | 45 | 60 | 75 | 90 |
| - | - | -- | -- | -- | -- | -- | -- |
| 60 | 246 | 198 | 195 | **192** | 195 | 198 | 246 |
| 90 | 246 | 197 | 191 | **186** | 191 | 197 | 246 |
| 120 | 246 | 195 | 187 | **182** | 187 | 195 | 246 |
| 160 | 246 | 192 | 183 | **177** | 183 | 192 | 246 |

A V with its vertex at exactly 45 degrees, deepening with radius: 21 levels at
R = 160.

### 1.3 Why the previous conclusion was wrong

[`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) carries a KNOWN
LIMITATION note on this, and [`review-findings.md`](review-findings.md) V4
closes it as documented-not-fixed, on the grounds that:

> softening `ad` near the axis would need a second distance field whose blend
> would reintroduce exactly the rect-size dependence the analytic form removed.

That is true of the fix it considered, and the framing is what blocked the real
one. The problem is not that `ad` is too large near the axis. The problem is
that **one term is being evaluated where four belong**. Nothing about `ad`
needs softening, and no second distance field is involved.

### 1.4 The fix: finite-segment closed forms, summed over the four edges

The same two per-sample kernels, integrated along a straight segment running
from `t1` to `t2` at perpendicular distance `a` (with `t` measured along the
segment from the fragment's foot of perpendicular), have elementary
antiderivatives:

```
haloSegment(a, t1, t2, k)  = k^2/(a^2+k^2) * ( t2/sqrt(a^2+k^2+t2^2)
                                             - t1/sqrt(a^2+k^2+t1^2) )

bloomSegment(a, t1, t2, k) = k/c * ( atan(t2/c) - atan(t1/c) ),  c = sqrt(a^2+k^2)
```

As `t1 -> -inf` and `t2 -> +inf` these reduce to `2k^2/(a^2+k^2)` and
`PI*k/sqrt(a^2+k^2)` - **exactly** the two expressions in section 1.1. So
`HALO_NORM_FACTOR` and `BLOOM_NORM_FACTOR` keep their meaning and their
calibration, and the peak values they were tuned against (0.86 and 1.005) are
unchanged. This is a strict generalisation, not a retune.

The four straight edges of the rect, for a fragment at local `p` with half-size
`b`, give perpendicular distances `|p.x +/- b.x|` and `|p.y +/- b.y|` and
segment extents `-b.y - p.y .. b.y - p.y` and `-b.x - p.x .. b.x - p.x`.

### 1.5 Measured result

Corner sweep, same probe as section 1.2, at 15 / 30 / 45 / 60 / 75 degrees
(0 and 90 sit on the edges themselves and read 246 throughout):

| R | before | after |
| - | ------ | ----- |
| 60 | 198 195 **192** 195 198 | 202 201 **201** 201 202 |
| 90 | 197 191 **186** 191 197 | 201 198 **197** 198 201 |
| 120 | 195 187 **182** 187 195 | 199 195 **193** 195 199 |
| 160 | 192 183 **177** 183 192 | 197 192 **190** 192 198 |

| | before | after |
| --- | ------ | ----- |
| equal-`ad` ratio (section 1.2) | 1.000 | 1.011 - 1.021 |
| dip depth at R = 160 | 21 levels, kinked at 45 | 11 levels, smooth |
| neon frame cost, 1280x720, median of 5 | 1.67 ms | 1.88 ms (**1.13x**) |

The residual bowl is genuine falloff, not a crease: that point really is eight
times further from the nearest edge than the sample beside it. What changed is
the shape - a kinked V became a smooth basin.

### 1.5.1 One regression the fix caused, and closed

The bloom is pedestal-subtracted so it reaches zero at the draw quad's edge
rather than being chopped there. That pedestal was the infinite-line value at
`reach`, and against a sum of finite segments it **over-subtracts** - a finite
segment is always dimmer than the infinite line it is cut from, so the sum
clamps to zero before the quad edge. Measured on the exterior at `glowRadius` 5:
the tail died 300 px out where it should run past 420.

Closed by giving each edge its own pedestal - the same segment evaluated at
`reach`, in `bloomSegmentPedestalled` - so that edge lands on zero at `reach`
from it whatever the other three are doing. The tail runs the full distance
again, with a largest adjacent-pixel step of 2/255 (i.e. no seam) at every
`glowRadius` tested.

The renormalisation that follows still uses the infinite-line pedestal as its
gain. On the line the nearest edge is the whole of the sum to within a few
percent, so the two pedestals differ by a few percent of a tuning constant;
deriving the gain from the near edge's own extents would mean branching to find
which edge that is.

### 1.6 What the fix does not cover

The four segments run to the **sharp** corner; the quarter-arc emitter above
`cornerRadius` 0 has no elementary closed form and is omitted. The
over-extension of the straights past the tangent point roughly compensates for
the missing arc, which is why the equal-`ad` ratio drifts 1.021 -> 1.011 rather
than holding. At `cornerRadius` 0 the sum is exact.

It also changes how a **small** rect glows, and that is the point of the change
rather than a side effect of it. An edge shorter than a few multiples of `bw`
subtends less than the infinite line the old form assumed, so its bloom is now
dimmer - correctly, since a short tube emits less light. Measured 30 px off the
middle of the top edge at the default `glowRadius` 5:

| rect width | 20 | 40 | 80 | 160 | 320 | 640 | 1200 |
| ---------- | -- | -- | -- | --- | --- | --- | ---- |
| before | 62 | 67 | 72 | 76 | 80 | 82 | 83 |
| after | 35 | 54 | 71 | 80 | 82 | 81 | 83 |

Under about 100 px wide it is a visible dimming; at 320 px and above the
difference is within 2/255. The old behaviour gave a 20 px rect 87% of the glow
of a 1200 px one, which is the infinite-line fiction showing through.

This also closes a second note in the same block of `neon.frag`, which recorded
that the gather this replaced ran corners about 40% hotter because it picked up
both incident edges. That is precisely what the segment sum restores.

---

## 2. resolutionScale 0.5 does not match 1.0: it is entirely the filament

### 2.1 Isolating it

Same geometry the report used - `(3, 3, 1000, 500)`, cornerRadius 0 - rendered
at 1.0 and at 0.5 and differenced. With `lineWidth` set to 0, so the filament
is gated off and only the halo and bloom remain:

| scene | max delta, 1.0 vs 0.5 |
| ----- | --------------------- |
| lw 0, glowRadius 5, bloom 0.30 | **4 / 255** |
| lw 0, glowRadius 30, bloom 1.0 | **1 / 255** |

The transform, the perimeter gather, the colour LUTs, the halo, the bloom and
`neon-blit.frag` all survive the half-res round trip. Every bit of the
discrepancy is the filament.

### 2.2 Cause

[`neon.frag`](../lib/shaders/neon.frag), filament block:

```glsl
float minHalf   = FILAMENT_MIN_HALF_WIDTH * uResolutionScale;   // 0.5 * scale
float halfWidth = uLineWidth * 0.5;                             // lineWidth * scale * 0.5
float sigma     = max(halfWidth, max(minHalf, 1e-3));
```

`uLineWidth` arrives pre-multiplied by the scale and `FILAMENT_MIN_HALF_WIDTH`
is converted into the same space, which is correct for keeping the line's
*stated* width scale-independent. The consequence is that **nothing is left
that floors the filament in BUFFER pixels**. At scale 0.5 a 1 px line has
sigma = 0.25 buffer px, well under the buffer's Nyquist limit.

The buffer point-samples that, and `neon-blit.frag` bilinearly upsamples it. So
the rendered line is decided by where the rect edge falls between buffer texel
centres. The reported geometry puts `x = 3` at scaled coordinate 1.5, which is
exactly a texel centre - the most favourable phase there is.

### 2.3 The measurement

Row through the left edge, defaults (`glowRadius` 5, `bloomStrength` 0.30),
`lineWidth` 1, sweeping the rect's sub-pixel position:

| rect x | scale 1.0 | scale 0.5 | max delta |
| ------ | --------- | --------- | --------- |
| 3.00 | `143 150 230 230 149 143` | `146 170 217 217 170 142` | 38 |
| 3.25 | `141 147 211 239 158 144` | `144 168 215 216 170 144` | 38 |
| 3.50 | `139 146 180 241 180 146` | `143 165 208 210 169 146` | 38 |
| 3.75 | `137 144 158 239 211 147` | `141 158 193 198 171 152` | 50 |
| 4.00 | `136 143 150 230 230 149` | `139 149 170 180 180 170` | **59** |
| 4.50 | `132 139 146 180 241 180` | `136 139 146 169 210 208` | 38 |

The 1.0 reference holds a 230 - 241 peak at every sub-pixel position. The 0.5
peak swings between 217 (edge on a texel centre) and 180 (edge halfway between
two), and the line spreads over four pixels instead of two. That is the
reported symptom: the line breathes as the rect moves.

Max delta against `lineWidth` at scale 0.5:

| lineWidth | 0 | 1 | 2 | 3 | 4 | 6 | 8 |
| --------- | - | - | - | - | - | - | - |
| max delta | 4 | 38 | 28 | 28 | 18 | 13 | 10 |

### 2.4 Options considered

**A. Parameter rule, no code.** Keep `lineWidth * resolutionScale` at or above
about 2 buffer px, so `lineWidth >= 2/scale` - 4 or more at scale 0.5, where
the delta is 18/255. Costs nothing and needs no change, but it removes the thin
line from the design space at any reduced scale, which is the case that was
reported.

**B. Energy-conserving Nyquist floor.** Floor sigma at a constant in BUFFER
pixels - explicitly *not* multiplied by `uResolutionScale`, unlike every other
px constant in the shader - and scale the filament's amplitude to keep its
integrated energy. Bit-identical at scale 1.0 by construction: sigma there is
already at or above the floor for any `lineWidth`, so the compensation factor
is exactly 1.

**C. Draw the filament full-res, outside the scaled buffer.** The same
exception the opaque fill already takes ("always full-res on the caller's
framebuffer, since its analytic SDF edge is the whole point of it"). Section
2.1 shows nothing else needs the full rate. It would be cheap: the filament's
support is `reachSigmas` = `pow(log2(FILAMENT_GAIN / FILAMENT_CUTOFF), 1/N)` =
3.54 sigmas at the default falloff, so at `lineWidth` 4 a 14 px band over a
3000 px perimeter is about 42k fragments against the blit's 922k at 1280x720 -
a band-fitted ring, the shape `DropletsRenderer` already uses.

### 2.5 Why B and not C

C is the structurally right answer and it is written down here so the next
person does not have to rederive it, but it is not a free refactor and it was
not taken.

`result` currently accumulates filament + halo + bloom and tonemaps **once**,
at the end of the fragment program. Splitting the filament into its own pass
means it gets its own Reinhard + gamma and then composites premultiplied over
the already-tonemapped glow. Where the two overlap that is not the same image:
`tonemap(f) over tonemap(g)` is not `tonemap(f + g)`. Applying the split only
on the scaled path would keep 1.0 bit-identical but would trade the sampling
mismatch for a compositing mismatch, which has to be measured before it can be
called an improvement.

So C is a design decision about where the tonemap belongs, not a bug fix, and
it needs its own comparison document.

### 2.6 The amplitude compensation, and why it was dropped

B as first written paired the width floor with an amplitude scale of
`sigma / sigmaFloored`, to conserve the line's integrated energy. That is the
textbook move and it is wrong here, because the grade at the end of `neon.frag`
is a Reinhard tonemap followed by a gamma and it runs per-fragment **inside**
the scaled buffer. With `FILAMENT_GAIN` at 12 the line core sits deep in
saturation, so scaling the linear amplitude down barely moves the 8-bit value -
it just dims the peak.

Sweeping the compensation as `pow(sigma / sigmaFloored, P)` over the worst
sub-pixel phases, at `lineWidth` 1 / 2 / 4 / 8, against the 1.0 reference:

| P | 0 | 0.5 | 1.0 | 1.5 | 2.0 |
| - | - | --- | --- | --- | --- |
| peak error, scale 0.5 | **14** | 16 | 23 | 31 | 39 |
| peak error, scale 0.25 | **10** | 19 | 37 | 55 | 69 |
| worst delta anywhere, 0.5 | 75 | 68 | 60 | 52 | **48** |

The two metrics point opposite ways, and peak error is the one that matches the
report: the complaint is that the line's brightness is wrong and moves, not that
its skirt is a few levels off. P = 0 - no compensation at all, just the width
floor - is what shipped.

What that leaves is a line that reads **wider** at a reduced scale. That part is
not tunable away: a 1 px line cannot exist in a buffer sampled every 2 px. The
floor buys the narrowest line the buffer can actually carry, at the right
brightness, stable wherever the rect sits.

### 2.7 Result

Peak error against the 1.0 reference, worst case over `lineWidth` 1 / 2 / 4 / 8
and four sub-pixel rect positions:

| | before | after |
| --- | ------ | ----- |
| scale 0.50 | 50 / 255 | **14 / 255** |
| scale 0.25 | 91 / 255 | **10 / 255** |

The row through the left edge at the worst phase (rect x = 4.00), `lineWidth` 1,
default glow:

```
reference (1.0)   136 143 150 230 230 149 143 136
before    (0.5)   139 149 170 180 180 170 149 135
after     (0.5)   141 163 208 230 230 208 163 137
```

Scale 1.0 is untouched by this: `minHalf` there is already 0.5, so the floor is
a no-op and the expression reduces to exactly what it was.

---

## 3. What changed

| file | change |
| ---- | ------ |
| [`neon.frag`](../lib/shaders/neon.frag) | `haloSegment` / `bloomSegment` / `bloomSegmentPedestalled` helpers; halo and bloom summed over the four straight edges with per-edge pedestals; `reach` hoisted above the bloom; filament sigma floored in buffer px |
| [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) | `FILAMENT_NYQUIST_HALF_WIDTH` added beside `FILAMENT_MIN_HALF_WIDTH`, with the stated-width vs sampling distinction written out; the halo block's KNOWN LIMITATION note replaced by the segment-sum derivation |
| [`neon-renderer.cpp`](../lib/src/renderer/neon-renderer.cpp) | `setupGeometry` applies the Nyquist floor in buffer px, after the scale, so the draw quad clears the filament the shader actually draws |
| [`review-findings.md`](review-findings.md) | V4 closed as fixed, with the reason its original premise was wrong |

Not changed, and deliberately: the tonemap stays where it is, so option C in
section 2.4 remains available and unattempted. Scale 1.0 moves only by the
four-edge halo and bloom, which is intentional; the filament change is a no-op
there.

## 4. Reproducing

The probes are not build targets - there is no test target in this repo. They
are single translation units that link `build/lib/libedge-lighting.a` and drive
the effect through `OffscreenCapture`:

```bash
clang++ -std=c++17 -O2 -w -DGL_SILENCE_DEPRECATION -o probe probe.cpp \
  external/src/glad.c -Ilib/include -Iexternal/include -Ibuild/lib/generated \
  -Lexternal/lib/arm64 -lglfw.3 -Wl,-rpath,$PWD/external/lib/arm64 \
  build/lib/libedge-lighting.a -framework OpenGL -framework Cocoa -framework IOKit
```

Six shapes cover everything above. All of them set a **flat white ring** (one
colour stop) where brightness is the measurement, and **all** of them set
`hueRotationRate` to 0 - otherwise two captures are taken at different phases of
the gradient and every delta is meaningless.

| probe | what it answers | section |
| ----- | --------------- | ------- |
| corner | constant-radius arc from a corner, plus an equal-`ad` pair. A crease is a V with its vertex at 45 degrees; the ratio is the one-number version | 1.2, 1.5 |
| scale | one config at 1.0 and at 0.5, max / mean delta plus a row through the left edge. Sweeping `lineWidth` to 0 is what isolates the filament | 2.1, 2.3 |
| cal | the same across `lineWidth` and sub-pixel phase, reporting worst-case peak error - the metric the compensation sweep was decided on | 2.6, 2.7 |
| tail | exterior falloff outward from an edge, reporting where it hits zero and the largest adjacent-pixel step. This is what caught the pedestal regression | 1.5.1 |
| size | peak at a fixed distance across rect widths | 1.6 |
| smoke | a matrix of glow sides, cutoffs, opaque modes, degenerate sizes and falloffs, at 1.0 and 0.5, reporting mean / max / lit fraction so before and after can be diffed for blowouts and black frames | - |

The smoke probe is the one to run first after any change to this shader. It is
what showed that the small-rect dimming in section 1.6 was real behaviour rather
than a NaN.
