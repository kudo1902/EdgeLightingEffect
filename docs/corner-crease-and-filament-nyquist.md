# Two defects in the neon layer, measured

Both of these were reported from the same screenshot: dark diagonal wedges
running in from the four corners, and a `resolutionScale` 0.5 render that does
not match the 1.0 one at `lineWidth` 1 on geometry `(3, 3, 1000, 500)`.

They are unrelated. The first is a physics approximation in the analytic halo
and bloom; the second is a sampling-rate problem confined entirely to the
filament. Both are measured below and both are fixed - section 3 lists what
landed, and sections 1.6 and 2.5 are what deliberately did not.

A **third** item joined them afterwards, reported the same way: the first fix
made a rounded corner read square. It came from the one thing that fix left
unmodelled, and sections 1.7 and 1.8 measure and close it. Section 1.5.2 is a
fourth thing, and not a defect at all - the largest change the first fix made
turned out to be one nobody had written down.

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

### 1.5.2 The other half of the change: the interior fills

The corner sweep in 1.5 is the crease, and the crease is what this fix was
aimed at. It is not the largest thing the fix moved.

Both old expressions were functions of `ad = abs(d)` and of nothing else, so
the old profile was **symmetric about the line**: a fragment 120 px inside the
edge and one 120 px outside it were lit identically, by construction. That
symmetry is a fiction. Outside, the emitter is behind you and recedes; inside,
it wraps around you, and the opposite edge and both perpendicular edges all
contribute. A sum over four finite segments knows the difference. A
nearest-distance profile cannot.

Same probe as 1.2, reading straight in and straight out from the middle of the
left edge:

| ad (px) | 20 | 40 | 60 | 80 | 100 | 120 | 160 | 200 | 240 |
| ------- | -- | -- | -- | -- | --- | --- | --- | --- | --- |
| inward, before | 197 | 192 | 187 | 183 | 179 | 176 | 171 | 167 | 163 |
| inward, after | 201 | 197 | 194 | 191 | 189 | **188** | **187** | **186** | **186** |
| outward, before | 197 | 193 | 188 | 183 | 179 | 176 | 171 | 167 | 163 |
| outward, after | 200 | 195 | 189 | 184 | 180 | 176 | 169 | 163 | 157 |

The two "before" rows agree to within 1/255 at every distance, which is the
symmetry showing. After the change the exterior falls off slightly FASTER than
it used to - a finite segment carries less light than the infinite line it is
cut from - while the interior stops falling off at all past about 120 px and
settles onto a floor. Centre of the rect 162 -> **186**; interior mean, inset
60 px, 174.5 -> **188.8**.

Across `glowRadius`, on an 800x400 rect at `cornerRadius` 40, `bloomStrength`
0.30, 1280x720:

| glowRadius | centre, before -> after | interior mean | exterior mean |
| ---------- | ----------------------- | ------------- | ------------- |
| 5 | 13 -> 21 | 31.6 -> 37.0 | 38.0 -> 36.9 |
| 15 | 52 -> 81 | 74.8 -> 94.5 | 79.7 -> 78.1 |
| 30 | 81 -> 113 | 100.2 -> 121.6 | 105.4 -> 103.5 |
| 60 | 105 -> 131 | 125.0 -> 141.8 | 128.6 -> 124.3 |

So the glow **reads as bigger**, and the region that grew is the interior, not
the halo outside the line: the exterior mean goes slightly DOWN in every row.

Every "after" figure in this section is `b2fead5` itself, so that the table
isolates the change this section is about. Section 1.8's corner work moves them
again, by 1/255 or less on these probes (centre 186 -> 185, interior mean
188.8 -> 187.9), because it takes emitter length off the straights and gives
the same length back as arcs.
What a viewer sees is the mitred picture frame becoming an evenly lit panel.

This is the physics the four-segment form was adopted for, not a side effect to
tune back out - a real rectangular tube does light its own interior, and
`HALO_NORM_FACTOR` cannot be lowered to undo it without dimming the line as
well. A caller who wants the glow to keep hugging the perimeter has the knobs
that were always meant for it: `insideCutoff` caps how far in the emission
reaches, and `glowSide = OUTSIDE` removes the interior half outright.

### 1.6 What the fix does not cover

The four segments ran to the **sharp** corner; the quarter-arc emitter above
`cornerRadius` 0 has no elementary closed form and was omitted, on the grounds
that the over-extension of the straights past the tangent point roughly
compensates for the missing arc. It does not: the over-extension is a phantom
emitter sitting a few px from a fragment that is tens of px from the real tube,
and at a narrow `glowRadius` it is worth tens of levels. That is sections 1.7
and 1.8, which measure it and close it. At `cornerRadius` 0 the sum was and remains
exact.

What the fix genuinely does not cover is how a **small** rect glows, and that
is the point of the change rather than a side effect of it. An edge shorter than a few multiples of `bw`
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

### 1.7 The corner over-extension, measured

Reported from a render rather than from the source: at the default
`glowRadius` 5 with `cornerRadius` 40, the glow just outside each rounded
corner is too bright and the corner reads **square**, as though the tube were
mitred rather than rounded.

The cause is the omission 1.6 opens with. The left edge's segment runs from
`-halfSize.y` to `+halfSize.y`, i.e. to the SHARP corner, so it continues
`cornerRadius` px past the point where the real tube turns away. A fragment
just outside the rounded corner sits close to BOTH over-extended straights and
is lit as though it were almost touching a tube. On an 800x400 rect at
`cornerRadius` 40, the point 20 px outside the arc on the diagonal is 2.4 px
from each phantom straight and 20 px from the real emitter.

Measured against a numerically integrated ground truth - the same two kernels
summed over the true rounded-rect perimeter, four straights to the tangent
points plus four quarter arcs, 1600 elements - on the corner diagonal, `R`
measured from the arc centre so the tube itself is at `R = 40`:

| R | 30 | **40** | 45 | **50** | **60** | 80 | 120 | 200 |
| - | -- | ------ | -- | ------ | ------ | -- | --- | --- |
| exact | 130 | **175** | 147 | **119** | **93** | 67 | 42 | 24 |
| shipped | 120 | **135** | 149 | **170** | **147** | 80 | 47 | 25 |

Forty levels too dark ON the tube and fifty too bright just outside it, in a
band about one `cornerRadius` wide. The same probe run against the shipped
renderer rather than the model agrees: the point 20 px outside the arc goes
98 -> 146 across `b2fead5`.

It shrinks as the profile widens, because a wide kernel cannot tell 2.4 px from
20 px. Worst absolute error over a 10 px grid covering one quadrant, inside and
out, against the integrated truth:

| geometry | gr 5 | gr 15 | gr 30 | gr 60 |
| -------- | ---- | ----- | ----- | ----- |
| 800x400 r=40 | 76 | 36 | 17 | 9 |
| 800x400 r=120 | 112 | 77 | 52 | 29 |
| 800x400 r=200 | 130 | 95 | 74 | 48 |
| 300x200 r=30 | 67 | 26 | 14 | 8 |
| 200x200 r=100 (a circle) | 109 | 76 | 51 | 31 |
| 1200x600 r=8 | 19 | 5 | 3 | 2 |

So it is worst exactly where the corner is the visible feature: a large
`cornerRadius` against a tight glow.

---

### 1.8 The fix: trim the straights, develop the arcs

Two halves, both in [`neon.frag`](../lib/shaders/neon.frag).

**The straights stop at the tangent points.** Their extents become
`halfSize - cornerRadius` rather than `halfSize`, so a straight runs exactly as
far as the flat run of tube it stands for and the phantom is gone.

**Each arc becomes a fifth through eighth segment.** A circular arc has no
elementary antiderivative under either kernel, so `arcTangentSegment` **develops
it** onto a straight line instead: the tangent at whichever arc point is
nearest the fragment, carrying the arc's full length `PI*r/2` and split about
that point. Two properties fall out, and they are the whole reason this form
was picked over a fixed tangent:

- The perpendicular distance it reports is the TRUE distance to the arc,
  `abs(length(w) - r)`, for every fragment that faces it.
- The two halves run exactly as far as the real arc does in each direction, so
  the developed arc abuts the trimmed straights in arclength. No gap, no
  overlap, and the emitter is continuous all the way round.

The frame costs one `atan` and one `length` per corner. `w` is the fragment's
offset from the arc centre in that corner's own frame, x along one incident
edge's outward normal and y along the other's, so the arc is exactly the first
quadrant of `w` and clamping into it is `max(w, 0)`. Off the ends the nearest
arc point is a tangent point, and which one follows from
`|w - (r,0)|^2 - |w - (0,r)|^2 = 2r*(w.y - w.x)`; that is what the degenerate
fallback picks, so it is the correct clamp for its whole region and not only
for the one point (a fragment exactly on an arc centre) that forces it to
exist.

**Why not a fixed tangent, or two, or three.** A stub tangent at the arc's
midpoint is the same cost as the developed form, minus the `atan`. It is not
the same accuracy. Worst error in 8-bit levels against the integrated truth,
`glowRadius` 5, sweeping along the top edge through the tangent point - the
path where a fixed stub is furthest from the arc it stands for:

| | shipped (phantom) | 1 fixed stub | 2 stubs | 3 stubs | developed |
| --- | ----------------- | ------------ | ------- | ------- | --------- |
| along the edge | 57 | 32 | 10 | 3 | **3** |
| corner diagonal | 54 | 4 | 12 | 5 | **5** |

One fixed stub trades the phantom for a smaller ripple at the tangent point;
three stubs match the developed form at three times its segment count. The
developed arc gets there with one.

### 1.8.1 Measured

Same grid as 1.7 - worst absolute error over a 10 px grid covering one
quadrant, inside and out, against the numerically integrated perimeter:

| geometry | gr 5 | gr 15 | gr 30 | gr 60 |
| -------- | ---- | ----- | ----- | ----- |
| 800x400 r=40 | 76 -> **6** | 36 -> **5** | 17 -> **6** | 9 -> **3** |
| 800x400 r=120 | 112 -> **6** | 77 -> **6** | 52 -> **6** | 29 -> **7** |
| 800x400 r=200 | 130 -> **8** | 95 -> **11** | 74 -> **10** | 48 -> **11** |
| 300x200 r=30 | 67 -> **5** | 26 -> **6** | 14 -> **5** | 8 -> **2** |
| 200x200 r=100 (a circle) | 109 -> **18** | 76 -> **20** | 51 -> **19** | 31 -> **23** |
| 1200x600 r=8 | 19 -> **3** | 5 -> **1** | 3 -> **1** | 2 -> **1** |

And on the renderer rather than the model, 800x400 at `cornerRadius` 40,
`glowRadius` 5, the exterior corner diagonal with `R` from the arc centre:

| R | 45 | 50 | 60 | 70 | 80 | 100 | 120 | 160 |
| - | -- | -- | -- | -- | -- | --- | --- | --- |
| before `b2fead5` | 174 | 128 | 101 | 86 | 73 | 57 | 45 | 29 |
| `b2fead5` | 171 | **164** | **159** | 101 | 78 | 54 | 40 | 24 |
| fixed | 173 | **126** | **96** | 80 | 65 | 47 | 36 | 22 |

The bulge is gone and the corner reads round again. The interior change from
1.5.2 is untouched by this: centre of the rect stays at 21 (`glowRadius` 5) and
112 (`glowRadius` 30), against 13 and 81 before `b2fead5`.

### 1.8.2 The arc pedestal is shared, and what that costs

1.5.1 established that the bloom's pedestal has to be **per piece**. The arcs
are the exception, and for a reason that does not apply to an edge: an arc's
length is `PI*r/2` no matter where the fragment is, so evaluating its pedestal
with that length CENTRED leaves an expression in uniforms alone. It is exact
for every fragment that faces an arc, and an over-subtraction only for ones off
to the side, where the arc term is small and the clamp takes it to zero anyway.
An edge cannot do this because its half-length routinely exceeds `reach`, which
is exactly the 1.5.1 failure.

Measured on the worst case that exists - a circle, four arcs and no straights,
so nothing else carries an exact pedestal. `glowRadius` 5, emitter radius 200:

| | per-arc pedestal | shared centred | `bw*L/c^2` |
| --- | ---------------- | -------------- | ---------- |
| last lit radius, 0 deg | 494 | 476 | - |
| last lit radius, 45 deg | 644 | 506 | - |
| lit fraction, 1280x720 | 0.638 | 0.589 | 0.576 |
| largest adjacent step | 1/255 | 1/255 | - |

The whole difference sits in values of 4/255 and below, and the step stays at
1/255, so the tail **ends sooner rather than being chopped** - which is the
distinction 1.5.1 turns on. On a rounded rect, where the straights' own exact
pedestals dominate, it is 0.1% of the lit fraction. The further collapse to
`bw*L/c^2` is a ~7% over-subtraction and is called out at the call site so
nobody tries it again.

### 1.8.3 Cost, and what stays bit-identical

Neon pass at 1280x720, `GL_TIME_ELAPSED`, median of 15, on an AMD Radeon Pro
5300M. That machine is several times slower than whatever produced 1.5's
1.67 / 1.88 ms, so only the ratios here are comparable:

| scene | `b2fead5` | fixed | |
| ----- | --------- | ----- | - |
| r=0 gr=5 | 8.83 ms | 8.84 ms | **1.00x** |
| r=0 gr=60 | 8.83 ms | 8.84 ms | **1.00x** |
| r=40 gr=5 | 8.85 ms | 10.52 ms | 1.19x |
| r=40 gr=60 | 8.85 ms | 10.52 ms | 1.19x |
| r=200 gr=30 | 8.88 ms | 10.46 ms | 1.18x |

A sharp-cornered rect pays **nothing**, because `uCornerRadius` is a uniform and
the block branches uniformly. That parity is not free by construction, though -
it is a register-pressure result. With the per-arc pedestal in place the block
was heavy enough to cost the SHARP path 1.14x for code it never executes
(8.83 -> 10.06 ms); shrinking the body is what returned it to parity. Anything
added here should be re-timed on a `cornerRadius` 0 scene as well as a rounded
one.

`cornerRadius` 0 is also bit-identical, verified by capture rather than by
argument: nine scenes covering both glow sides, an outside cutoff, an opaque
fill, `resolutionScale` 1.0 / 0.5 / 0.25, `glowRadius` 0 and a wide glow, all
`cmp`-equal to the same scene rendered by `b2fead5`.

### 1.8.4 What was still approximate

A **circle** was the residual case, at 18 to 23 levels: with
`cornerRadius == halfMin` the perimeter is four developed arcs and no straights,
so every fragment is served entirely by the approximation, and one of the four
is always in the degenerate fallback (its `w` has both components negative
wherever the fragment sits on an axis). Everything between a sharp rect and a
circle was single digits.

That was the right order of magnitude and the wrong shape of claim, which is
why it read as acceptable and was not. Twenty levels of SMOOTH error is
invisible. What the circle actually had was four levels of error shaped like a
cross, and that is not - see 1.9.

### 1.9 The development rate: a cross at every arc's centre of curvature

#### 1.9.1 The defect

`arcTangentSegment` laid each arc out at its own arclength: one unit of tangent
per unit of arc, `t1 = -r*th - off`, `t2 = r*(HALF_PI - th) - off`. That is the
natural reading of "develop the arc", and it is only correct for a fragment
sitting ON the arc.

Two things went wrong with it, and they are the same thing seen from two sides.

**At the centre of curvature it under-counted.** A fragment there is at
distance `r` from every point of the arc, so the honest answer is
`f(r) * PI*r/2`. Rate `r` cannot say that: the nearest arc point is degenerate,
the fallback picks an endpoint, and the whole arc develops to ONE side of the
foot, `t` running `0 .. r*PI/2`. Both kernels peak at `t = 0`, so a one-sided
range collects less than a straddling one. On a circle - where all four arc
centres coincide at the middle of the shape - the sum came to **54%** of the
true value (0.0334 against 0.0619 for the halo integrand at `r` 200, `k` 20).

**And it creased.** Crossing the line `w.y = 0` the arc's endpoints slide at
`-r * d(th)` on the facing side, which is `-r/w.x`, and at `-d(off)` on the
clamped side, which is `-1`. Those agree only at `w.x == r`, the tangent point.
Every other point of the two lines through an arc centre carried a C1 kink, and
the kink grows without bound as the fragment approaches the centre.

Rendered, that is an L-shaped seam per corner - one arm horizontal, one
vertical, meeting at the arc centre - and on a circle, where the four coincide,
one unmistakable dark cross at the middle of the shape. It is confined to the
neighbourhood of the centre of curvature, so it only reaches the eye when that
point is inside the lit region: a large `cornerRadius`, a large `glowRadius`, or
both. At the stock `cornerRadius` 40 the vertical second difference along the
crease ray is 0 to 1 levels, i.e. at the quantisation floor.

#### 1.9.2 The rate the kernels actually want

The exact distance from a fragment to the arc point `dphi` away from the nearest
one, with `rho = length(w)`:

```
D^2 = a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,     a = abs(rho - r)
```

which is the straight-segment form `D^2 = a^2 + t^2` under the substitution
`t = 2*sqrt(rho*r)*sin(dphi/2)`. Its slope at the foot is **`sqrt(rho*r)`**, not
`r`. So the tangent coordinate the closed forms want is developed at that rate,
and since rate times length has to stay the emitter's arclength, the segment is
scaled by `r/rate` to put the measure back.

`arcTangentSegment` now returns that weight as `.w` and uses

```
lam = sqrt(rho * min(rho, r))
```

The `min` is the inner branch. Outside the arc `sqrt(rho*r)` is the
linearisation above; inside, the rate has to be `rho` for the clamp to join
smoothly, which is the same condition that produced the crease - the facing
side slides at `-lam/w.x` and the clamped side at `-1`, and they agree
everywhere exactly when `lam` is `length(w)`. The two branches meet at
`rho == r`, where `lam` is `r` and the weight is 1, so **a fragment on the tube
is bit-identical to the rate-`r` form** and the calibration `HALO_NORM_FACTOR`
and `BLOOM_NORM_FACTOR` carry is untouched.

At `rho -> 0` the segment collapses to zero length against an unbounded weight,
and the limit is `f(a) * lam*HALF_PI * r/lam = f(r) * PI*r/2` - the honest
answer, exactly. `rho` is floored by `ARC_FRAME_EPSILON` so the division cannot
be by zero; the floor is far under a pixel and the limit it lands on is the
value above anyway.

The shared bloom pedestal (1.8.2) follows: the developed extent is
`lam*HALF_PI`, and `lam` is per-fragment, so the centred evaluation stops being
an expression in uniforms alone. It becomes one again by pinning `lam` to what a
fragment AT `reach` from the arc would carry - such a fragment sits `reach + r`
from the arc centre, where `lam` is `sqrt(rho*r)`. That is the only distance the
pedestal is meant to be exact at, and it was already an approximation
everywhere else.

#### 1.9.3 Measured

Against a numerically integrated emitter (6000-point quadrature over the true
rounded-rect perimeter), on a 40x28 grid covering the interior and out to 1.5x
the rect. Halo kernel, worst error as a percentage of truth:

| geometry | `glowRadius` | rate `r` | rate `lam` |
| -------- | ------------ | -------- | ---------- |
| 600x400 r=40 | 20 | 16.5% | **9.8%** |
| 600x400 r=40 | 60 | 7.2% | **4.1%** |
| 600x400 r=150 | 20 | 21.2% | **16.1%** |
| 600x400 r=150 | 60 | 19.7% | **13.5%** |
| 600x400 r=200 (stadium) | 20 | 27.6% | **23.7%** |
| 600x400 r=200 (stadium) | 60 | 26.6% | **20.9%** |
| 400x400 r=200 (circle) | 20 | 37.3% | **26.3%** |
| 400x400 r=200 (circle) | 60 | 36.1% | **23.8%** |

Better on the worst case of every geometry tested, and better on the mean of
all but one (600x400 r=40 at `glowRadius` 60, 1.4% -> 1.6%). The accuracy is a
side effect; the crease is the point:

**Spurious curvature** - the second difference of `(model - truth)`, which is
zero for any smooth model because truth is smooth - along the crease ray, one
quarter arc at `r` 100, `k` 60, as a percentage of the local value:

| | rate `r` | rate `lam` |
| --- | -------- | ---------- |
| worst over the ray | 19.9% | **0.5%** |

**Rendered**, the same thing read off the framebuffer: the vertical second
difference over an 8 px span, walking the horizontal ray out from an arc centre
at 6, 10, 16 and 24 px.

| scene | `65c95d8` | fixed |
| ----- | --------- | ----- |
| 400x400 r=200 circle, `glowRadius` 20 | 4, 3, 2, 2 | **0, 0, 0, 0** |
| 400x400 r=200 circle, `glowRadius` 60 | 4, 4, 3, 3 | **0, 0, 0, 0** |
| 600x400 r=150, `glowRadius` 60 | 0, 0, 0, 0 | 0, 1, -1, -1 |
| 600x400 r=40, `glowRadius` 60 | 1, -1, -1, 0 | 1, -1, 0, 0 |

The two rounded-rect rows are at the quantisation floor on both sides, which is
the point of the 1.9.1 caveat: below a large `cornerRadius` the crease is real
but sub-LSB. High-passing the `r` 150 frame at one arc centre shows the seam
plainly on `65c95d8` and nothing on either side of it.

The interior value the under-count was eating comes back with it. Circle,
`glowRadius` 20, 400x400:

| | `b2fead5` | `65c95d8` | fixed |
| --- | --------- | --------- | ----- |
| centre pixel | 68 | 53 | **66** |
| interior mean | 74.8 | 62.2 | **78.7** |

`b2fead5` is not the target - it is the build with the phantom straights, which
is why its interior is in the right range for the wrong reason. The fixed column
is the one with neither the phantom nor the cross.

#### 1.9.4 Cost, and what stays bit-identical

`cornerRadius` 0 is **still** bit-identical: `cmp`-equal to both `65c95d8` and
`b2fead5` on the sharp scene, since the branch this lives behind never runs.

1280x720, 1200x700 rect, `glowRadius` 30, best of five runs of 240 frames with
`glFinish` either side, interleaved to keep thermal drift out of it:

| scene | `65c95d8` | fixed | ratio |
| ----- | --------- | ----- | ----- |
| `cornerRadius` 0 | 1.556 ms | 1.597 ms | 1.026x |
| `cornerRadius` 40 | 1.866 ms | 1.930 ms | 1.034x |
| `cornerRadius` 150 | 1.882 ms | 1.972 ms | 1.048x |
| `cornerRadius` 350 | 1.887 ms | 1.949 ms | 1.033x |

The sharp row is the one to notice. It executes none of this and still pays
1.026x, which is the register-pressure effect 1.8.3 recorded from the other
direction - re-time a `cornerRadius` 0 scene as well as a rounded one after
touching this block.

A formulation trading the `length`, the `sqrt` and the divide for two
`inversesqrt`s was measured and landed inside the run-to-run noise, so the
readable form stays.

---

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

### 2.8 The floor was a fixed half width, and that is the wrong question

#### 2.8.1 The report

`lineWidth` 1, `filamentFalloff` 0.27. At `resolutionScale` 1.0 nothing is
wrong. Below it the filament renders about **twice as wide** at 0.5 and roughly
four times at 0.25 - not two pixels wider, thirty.

#### 2.8.2 Why a soft falloff is the case that breaks it

`sigma` does not only set the core. It multiplies `reachSigmas`, so it sets the
whole profile:

```
reachSigmas = clamp(pow(log2(FILAMENT_GAIN / FILAMENT_CUTOFF), 1/N),
                    FILAMENT_REACH_MIN_SIGMAS, FILAMENT_REACH_MAX_SIGMAS)
```

At `filamentFalloff` 0.27, N is 0.54 and that expression wants 108, so it
clamps at `FILAMENT_REACH_MAX_SIGMAS` = 64. The filament's own tail is 64
sigmas. Holding `sigma` at a flat 0.5 BUFFER px means holding it at 1.0
full-res px at scale 0.5, against the 0.5 the caller asked for - so the tail
goes from 32 full-res px to 64.

Tail extent, in px outward from the edge to where the filament reaches 0, at
`lineWidth` 1, `glowRadius` 0, measured on the top edge at x = 500:

| `filamentFalloff` | N | scale 1.0 | 0.5, no floor | 0.5, flat 0.5 floor |
| ----------------- | - | --------- | ------------- | ------------------- |
| **0.27** | 0.54 | 31 | 31 | **61** |
| 0.40 | 0.80 | 11 | 12 | 22 |
| 0.50 | 1.00 | 7 | 8 | 13 |
| 0.70 | 1.40 | 4 | 4 | 8 |
| 1.00 | 2.00 | 3 | 4 | 6 |
| 1.50 | 3.00 | 2 | 4 | 4 |

The ratio is ~2x everywhere, because the floor scales sigma by 2 at this
resolution scale whatever the falloff. What changes with the falloff is what 2x
is worth in pixels: three at the default, thirty at 0.27.

#### 2.8.3 The floor was buying almost nothing there

Max per-phase peak error against the 1.0 reference at the SAME sub-pixel phase,
scale 0.5:

| `filamentFalloff` | no floor | flat 0.5 floor |
| ----------------- | -------- | -------------- |
| 0.27 | 13 | 8 |
| 1.00 | **82** | 12 |

At the default the floor is doing essential work - without it the peak collapses
to 153 against a 235 reference, and at `filamentFalloff` 1.5 to 24. At 0.27 it
buys five levels and costs a doubled filament.

The reason is that a soft profile is already many buffer pixels wide. Section
2.2's mechanism - the peak landing between texel centres - needs the
NEIGHBOURING texel to be dark. At N = 2 and `sigma` 0.25 buffer px, one buffer
px out the profile is `exp2(-(1/0.25)^2)`, which is 2e-8: the neighbour carries
nothing and the bilinear filter has nothing to rebuild the peak from. At
N = 0.54 the same point is `exp2(-(1/0.25)^0.54)` = 0.22 - the neighbour carries
a fifth of the peak, and the reconstruction is fine without any help.

#### 2.8.4 The fix: floor on the SHARE, not on the half width

So the floor is stated as what actually matters, and inverted for sigma:

```
core(FILAMENT_NYQUIST_SAMPLE_PX) >= FILAMENT_NYQUIST_MIN_SHARE
  <=>  sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
```

with `SAMPLE_PX` = 1.0 (one buffer texel) and `MIN_SHARE` = 0.0625. Those two
are today's behaviour restated rather than a retune: at N = 2 the right-hand
side is `1.0 / pow(4, 0.5)` = exactly 0.5, the constant it replaces.

Gated to `resolutionScale < 1.0` in both `neon.frag` and
`NeonRenderer::setupGeometry`. The flat constant was a no-op at 1.0 by
arithmetic coincidence - it equalled the converted stated floor - and this
expression is not, above N = 2, so the gate is written down now.

#### 2.8.5 Measured

`lineWidth` 1, `glowRadius` 0, scale 0.5. `floor` is what the expression asks
for in buffer px; `p` is the peak and `z` the tail extent:

| falloff | N | floor | ref (1.0) | no floor | flat 0.5 | share form |
| ------- | - | ----- | --------- | -------- | -------- | ---------- |
| 0.27 | 0.54 | 0.077 | p235 z31 | p228 z31 | p235 z**61** | **p228 z31** |
| 0.40 | 0.80 | 0.177 | p235 z11 | p223 z12 | p235 z**22** | **p223 z12** |
| 0.50 | 1.00 | 0.250 | p235 z7 | p217 z8 | p235 z**13** | **p217 z8** |
| 0.70 | 1.40 | 0.371 | p235 z4 | p201 z4 | p235 z8 | **p227 z6** |
| 1.00 | 2.00 | 0.500 | p235 z3 | p153 z4 | p235 z6 | **p235 z6** |
| 1.50 | 3.00 | 0.630 | p235 z2 | p24 z4 | p235 z4 | **p240 z4** |

The default row is identical to the flat floor, by construction. Above it the
floor engages slightly harder, which is the direction the evidence points -
`filamentFalloff` 1.5 unfloored is a peak of 24 against 235.

Blast radius, over the 47-scene capture set: **34 scenes byte-identical, 13
changed, and every one of the 13 is at a reduced resolution scale with a
non-default falloff.** Every `resolutionScale` 1.0 scene is untouched, and so is
every default-falloff scene including `w_fo100_s050` and the four sub-pixel
phase captures at scale 0.5.

Cost: none measurable. The block gains one `pow` and one compare per fragment
(the `log2` is of literals and folds). Five interleaved runs of the section
1.9.4 harness on the most stable scene gave 1.613 to 1.658 ms before and 1.580
to 1.661 ms after - the two ranges overlap completely.

#### 2.8.6 What this deliberately does not fix

Around `filamentFalloff` 0.4 to 0.5 the floor now stops engaging while the
unfloored peak error is still 12 to 18 levels - the `p223` and `p217` cells
above. That band trades a correct 12 px tail for an 18-level peak error where
the flat floor traded a 6 px tail error for none.

It is a deliberate crossover, not an oversight. Lowering `MIN_SHARE` to cover
that band raises the floor at N = 2 as well, which stops the default case being
bit-identical - and the default case is the one every existing render sits on.

A variant that covers both was measured and not taken: keep the flat floor but
cap how many px of EXTENT it may add, `sigma = min(max(stated, FLAT), stated +
K/reachSigmas)`. With K = 3 buffer px that holds the default unchanged, keeps
the 0.4-0.7 band floored, and bounds 0.27 to a 38 px tail against the correct
31. It is strictly better on this sweep and strictly harder to explain - two
constants and a clamp, against one inverted equation. Worth revisiting if
anyone reports the 0.4-0.5 band.

---

## 3. What changed

| file | change |
| ---- | ------ |
| [`neon.frag`](../lib/shaders/neon.frag) | `haloSegment` / `bloomSegment` / `bloomSegmentPedestalled` helpers; halo and bloom summed over the four straight edges with per-edge pedestals; `reach` hoisted above the bloom; filament sigma floored in buffer px |
| [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) | `FILAMENT_NYQUIST_HALF_WIDTH` added beside `FILAMENT_MIN_HALF_WIDTH`, with the stated-width vs sampling distinction written out; the halo block's KNOWN LIMITATION note replaced by the segment-sum derivation |
| [`neon-renderer.cpp`](../lib/src/renderer/neon-renderer.cpp) | `setupGeometry` applies the Nyquist floor in buffer px, after the scale, so the draw quad clears the filament the shader actually draws |
| [`review-findings.md`](review-findings.md) | V4 closed as fixed, with the reason its original premise was wrong |

Then, for the corner over-extension in sections 1.7 and 1.8:

| file | change |
| ---- | ------ |
| [`neon.frag`](../lib/shaders/neon.frag) | straights trimmed to the tangent points; `arcTangentSegment` added and one developed-arc segment summed per corner, behind a `uCornerRadius > 0` uniform branch; one shared centred pedestal for the four arcs |
| [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) | `ARC_FRAME_EPSILON` added; the halo block's "runs to the SHARP corner" paragraph replaced by the developed-arc derivation, and the interior change from 1.5.2 written down beside it |
| [`review-findings.md`](review-findings.md) | V10 |

Then, for the development rate in section 1.9:

| file | change |
| ---- | ------ |
| [`neon.frag`](../lib/shaders/neon.frag) | `arcTangentSegment` returns a `vec4` - the development rate becomes `sqrt(rho * min(rho, r))` and `.w` carries the `r/lam` measure the caller multiplies in; the shared arc pedestal is evaluated at the `lam` a fragment at `reach` would have |
| [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) | the halo block's developed-arc paragraph gains the rate and why it is not `r`; `ARC_FRAME_EPSILON` gains the second thing it now floors |
| [`review-findings.md`](review-findings.md) | V12 |

Then, for the sampling floor in section 2.8:

| file | change |
| ---- | ------ |
| [`neon.frag`](../lib/shaders/neon.frag) | `N` hoisted above the filament's floors; the flat sampling floor replaced by the inverted share expression, gated to `resolutionScale < 1.0` |
| [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h) | `FILAMENT_NYQUIST_HALF_WIDTH` replaced by `FILAMENT_NYQUIST_SAMPLE_PX` and `FILAMENT_NYQUIST_MIN_SHARE`, with the inversion and the crossover written out |
| [`neon-renderer.cpp`](../lib/src/renderer/neon-renderer.cpp) | `setupGeometry` mirrors the expression and the gate, so the quad still clears what the shader draws |
| [`review-findings.md`](review-findings.md) | V13 |

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
| arc | the corner work. A Python model of both kernels over the true rounded-rect perimeter (straights plus quarter arcs, 1600 elements) is the ground truth; the candidate emitter decompositions are evaluated against it on a grid and through the grade, which is what the 8-bit error tables are | 1.7, 1.8.1 |
| sharp | nine `cornerRadius` 0 scenes captured on both sides of a change and `cmp`-ed, which is how the uniform branch's bit-identity claim is checked rather than argued | 1.8.3 |
| timer | the neon pass under `GL_TIME_ELAPSED`, median of 15 after 5 warm-up frames, across sharp and rounded scenes - both, because the register pressure of a branch nothing takes is measurable | 1.8.3 |

The smoke probe is the one to run first after any change to this shader. It is
what showed that the small-rect dimming in section 1.6 was real behaviour rather
than a NaN.
