# Droplets: one region, two bounding shapes

What changed when the droplet band stopped being "a normalised coordinate
across a band" and became "an outer rounded rect minus an inner one", what the
band path cost, and what having two *independent* shapes buys - measured.

The short version: the droplets now carry geometry of their own and read
nothing from `Config::geometry` or `NeonConfig::glowSide`; a single-lane
perimeter band expressed through the new API renders within 1/255 of the old
one; the four scenes that move do so deliberately, and all four are
improvements; and two hand-maintained inversions between the CPU and the shader
are gone.

---

## 1. What the region is

Everything the shader shapes hangs off two signed distances:

```
region  = inside(outer) \ inside(inner)
depth(p) = min( -sdOuter(p), sdInner(p) )     // px, positive inside
```

That is the standard SDF subtraction, negated. A region with no inner shape is
the same expression with the second term dropped. `DropletsConfig::hasInner` is
the single flag that says which - and "is there an inner shape" is exactly the
band-versus-fill distinction, so there is one concept rather than two.

The two shapes are **independent**: different centres, extents and corner radii
are all legal. Nothing downstream assumes otherwise.

### 1.1 The shapes are this layer's own

`DropletsConfig::outer` and `inner` are read verbatim. The droplets consult
**neither** `Config::geometry` nor `NeonConfig::glowSide`, so moving or resizing
the glow does not move the rain; a host that wants them to agree copies the four
numbers across, and the agreement is deliberate rather than structural. Both
demos have a button for it and seed a band at startup.

A perimeter band is the concentric special case. A band of thickness `t` around
a rect is `inner` = that rect and `outer` = the same rect dilated by `t`: half
extents **and** corner radius, which is what keeps the two boundaries a constant
distance apart.

The one read of another layer's config that remains is
`NeonConfig::glowSideSoftness`, which feathers the region's boundary. That is a
look knob, not geometry.

### 1.2 Why dilation is exact, and where it can go wrong

`sdRoundBox` is an exact distance field, and dilating a rounded rect by `t`
gives `(halfSize + t, radius + t)`, which leaves the formula's `q` **entirely
unchanged**:

```
q' = |p| - (b + t) + (r + t) = |p| - b + r = q
```

so the dilated shape's SDF is just `sd(p) - t`. Building a band by dilating a
rect costs nothing and loses nothing.

**The invariance requires half extents and radius to move together.** That now
matters to whoever *builds* the pair rather than to the library, which no longer
derives one - and there are two ways to get it wrong, both real:

- **Clamping one and not the other.** An earlier draft of this work floored both
  at zero inside the renderer, and an INSIDE band wider than the corner radius -
  which is most of them - then resolved to the wrong shape entirely. Section 5.
- **Eroding past the shape's own extent.** The result is not a small shape, it
  is *no* shape. `RectGeometry` cannot spell that (negative extents are not
  meaningful and `GetEffectiveCornerRadius` clamps), and it does not have to:
  `hasInner = false` says it directly, and says it better.

### 1.3 What it replaced

The band used to be defined twice: `BandAcross` in the shader as a normalised
coordinate, and `GetBandExtent` on the CPU as a by-hand inversion of it, needed
only to size the draw geometry. Its own comment said the inversion's
`if / if / fallthrough` shape mirrored the shader's *deliberately*, so the two
could not drift.

Both sides are now handed the same two shapes. There is nothing to invert.

### 1.4 What independence costs

Two independent shapes mean two full `sdRoundBox` evaluations where a
concentric pair could share one `q` and differ by a subtraction. The evaluation
is hot: `RegionFade` runs 3 times per `Drops` call and `Drops` runs up to 3
times per fragment (the value plus two gradient taps, themselves gated on
`c.x > 0`), so up to 10 per fragment.

Because of that, `RegionDistances` returns **both** distances from one call and
`RegionDepth` / `RegionSpan` are pure functions of that pair. Asking for the
depth and the span separately - which `main()` did until the review in section 5
- evaluated the same two distance fields twice for a bit-identical result.

A concentric fast path is possible - a band's two shapes *are* dilations of one
rect, and could share a `q` - but it is a second code path through the hottest
function in the shader, and it was not taken. Worth revisiting if this layer
ever shows up in a profile.

### 1.5 Two things that stay band-only

- **Drop pitch is its own knob** (`dropSize`). It
  has to be one GLOBAL px scalar, because every region-relative term in the
  shader is an amplitude and never a position, precisely so the screen-space
  grid never shears. The region's local width is not even constant once the two
  shapes stop being concentric, so there is nothing else it could track.
- **Orientation is a property of a curve.** `runs` is forced to 1 for a filled
  shape rather than computed: an interior fragment of an area has no local run
  direction, and computing one gives a constant ~0.5 - all beads, no streaks.
  For a ring it is read off the **outer** shape's face distances, the only one
  of the two that can speak for the region's orientation once the inner may sit
  anywhere.

### 1.6 `gSpan` couples the two shapes - and what the feather does about it

`depth` is `min(dOut, dIn)` - over the outer half of a band it is just `dOut`,
and the inner shape does not enter it. But the shader also derives

```glsl
gSpan = RegionSpan(fragPx) = dOut + dIn    // the region's local width, in px
```

which depends on **both** shapes at **every** fragment, and four things read it:

| reader | what it sets |
| --- | --- |
| `runs = S(-gSpan, gSpan, q.x - q.y)` | how fast streaks give way to beads at a corner |
| `RegionFade`'s `r = clamp(radiusPx, 0.02 * gSpan, 0.45 * gSpan)` | the whole-drop fade window |
| `flatPx = min(TRAIL_FLAT_SPAN * gSpan, ...)` | trail length on a horizontal run |
| the feather's guard, `min(..., gSpan * 0.5)` | never feather over more room than there is |

So **the inner shape is not a local influence on the inner half of the band.**
It sets the unit those quantities are measured in, everywhere.

The feather used to be on that list, and it was the dominant term:

```glsl
softPx = clamp(max(uGlowSideSoftness, gSpan * 0.25), gSpan * 0.02, gSpan * 0.5);
```

Measured - one outer rect, one drop field, only the inner shape moved, sampled
in an annulus **4-12 px inside the OUTER boundary** where `dOut` and therefore
`depth` are identical by construction:

| band width | mean alpha, span-relative feather | mean alpha, as shipped |
| --- | --- | --- |
| 30 | 267.5 | 284.4 |
| 60 | 161.5 | 294.5 |
| 120 | 49.4 | 295.8 |
| 175 | **22.8** | **296.0** |

**11.7x dimmer at the same distance from the same outer edge**, purely from
moving the hole. The shipped column holds to 4%.

#### What shipped, and why

The feather's own comment says its job is that *"whatever still overhangs after
the per-drop fade has to vignette out, not be cut off"*. An overhang is
**drop**-sized. The band was only ever a stand-in for the drop, and an exact one
while drop size was derived from band width (`cellPx = bandWidth / lanes`).
Decoupling `dropSize` broke the stand-in; this restores it:

```glsl
softPx = min(max(uGlowSideSoftness, uDropPitch * 0.25), gSpan * 0.5);
```

Same formula, scale swapped back to what it was proxying for, plus the one thing
the region legitimately decides - you cannot feather over more room than there
is, so it never exceeds half the local width. That guard engages only on a band
thinner than twice the drop feather.

Three alternatives were built and rendered before this one was chosen:

| form | rule | outer edge independent? | vs. today at 1 lane |
| --- | --- | --- | --- |
| span-relative (was shipping) | `gSpan * 0.25` | **no**, 11.7x | - |
| flat 1 px | `max(softness, 1.0)` | yes | differs everywhere |
| drop-relative, no guard | `dropPitch * 0.25` | yes | over-feathers a thin band |
| **drop-relative + guard (shipped)** | `min(dropPitch * 0.25, gSpan * 0.5)` | yes | **byte-identical** |

Two things the comparison corrected, both of which I had backwards from
reasoning alone:

- **Thin bands were never at risk.** At a 6 px band the old feather is 1.5 px
  and the new one 3.0 px - the new one is *softer*, so it cannot hard-cut. The
  divergence is on THICK bands, where 30 px of feather washed out both edges of
  a 120 px band.
- **Flat 1 px looked like the obvious fix and is the wrong one.** It differs
  from today at every band width, where the drop-relative form differs at none
  (at one lane).

#### Residual coupling

The feather is off the `gSpan` list; `runs` and the trail cut are not, and that
is the 4% drift left in the table above. Both are genuinely about the band's
shape at the fragment being shaded, so reading the fragment's span is right for
them.

`RegionFade`'s clamp was a third reader and is no longer one in the same sense:
it now takes the span at the **drop's own centre** rather than at the fragment,
which is the frame its window belongs in. See section 5.2.

---

---

## 2. The mask: a product of two ramps, or one ramp on the nearer boundary

The old mask multiplied a rising ramp at the inner boundary by a falling ramp
at the outer. Px depth to the *nearer* boundary wants a single ramp. Those are
the same function wherever the two ramps cannot overlap.

They cannot. The rising ramp spans `[0, soft]` and the falling one
`[1 - soft, 1]` in normalised units, so their interiors intersect only when
`soft > 0.5` - and `soft` is clamped to `[0.02, 0.5]`. The same holds for the
per-drop fade: its ramps overlap only when `r > 0.5`, and `r` is clamped to
`[0.02, 0.45]`. Below overlap, a product of two monotonic ramps **is** their
min, because the far one is exactly 1.

> **These two clamps are load-bearing for that equivalence.** They were added
> for other reasons - a drop wider than the band could never otherwise reach
> full brightness - and raising either ceiling above 0.5 silently makes the two
> forms diverge.

Measured directly, both forms compiled into one shader behind a uniform and
rendered at identical clock time, 900x640, full RGBA readback:

| scene | lit px | differing px | max abs delta |
| --- | --- | --- | --- |
| OUTSIDE bw24, softness 0 / 6 / 12 / 24 / 100 | 5.6-6.1k | 0 | 0 |
| INSIDE bw24 softness 18 | 4,278 | 0 | 0 |
| BOTH bw24 softness 18 | 6,479 | 0 | 0 |
| OUTSIDE bw6 softness 30 | 1,735 | 0 | 0 |
| OUTSIDE bw60 softness 45, 4 lanes | 21,494 | 0 | 0 |
| OUTSIDE bw120 softness 60, 2 lanes | 44,985 | 0 | 0 |
| OUTSIDE bw24 softness 12, sharp corners | 5,894 | 0 | 0 |
| OUTSIDE bw24 softness 12, **24 lanes** | 9,833 | **2** | **1** |

The two pixels:

```
(467,123)  product=(135,136,137,137)  min=(136,136,137,137)
(730,168)  product=(9,9,9,9)          min=(9,9,10,10)
```

Both are `min` one LSB *brighter* - a GPU `smoothstep` returning `1 - eps`
rather than exactly 1 at the clamp boundary, so the product attenuates by that
eps where the min does not. The min form is the marginally more faithful of the
two.

---

## 3. What a perimeter band cost

Section 2 isolates the mask. The change also moves the region computation into
pixels and splits one shared `q` into two independent distance fields, both of
which reorder arithmetic. That is not free in the last bit.

Twelve band scenes rendered by the **pre-change library**, then the same twelve
expressed through the new API - `inner` on the rect, `outer` dilated by the band
width - and rendered by the **post-change library**. Identical clock time, a
fresh effect per scene.

| scene | lanes | differing px | % of lit | max abs delta |
| --- | --- | --- | --- | --- |
| OUTSIDE bw24 | 1 | 1 | 0.02% | 1 |
| OUTSIDE bw24 offset +12 | 1 | 1 | 0.01% | 1 |
| OUTSIDE bw24 offset -8 | 1 | 3 | 0.04% | 1 |
| OUTSIDE bw8 softness 20 | 1 | 0 | - | 0 |
| INSIDE bw24 | 1 | 0 | - | 0 |
| BOTH bw24 | 1 | 1 | 0.01% | 1 |
| OUTSIDE bw24 radius 120 | 1 | 1 | 0.02% | 1 |
| OUTSIDE bw16 amount 0.2 | 1 | 0 | - | 0 |
| OUTSIDE bw60 | **3** | 10,496 | 44.09% | 177 |
| INSIDE bw90, sharp corners | **2** | 11,658 | 46.24% | 124 |
| BOTH bw40 offset 10 softness 6 | **2** | 3,895 | 34.22% | 95 |
| INSIDE bw400 (a band as wide as the rect) | 8, filled | 46,361 | 94.71% | 254 |

**Every single-lane ring is within one 8-bit step** - in both directions, the
signature of ordinary float reassociation rather than of any rule change. That
is a weaker guarantee than the neon and lens-flare unifications, which were
byte-identical throughout, and it is the same class of exception
`lens-flare-perf-review.md` documents for one of its five changes: a change of
units cannot be bit-exact.

The other four rows are not float noise, and they sort into exactly two causes.

### 3.1 More than one lane

The three multi-lane rings differ because the boundary feather now scales with
the **drop**, not the band - section 1.6. At `lanes` 3 the drops are a third the
size, so their overhang is a third as large, and a third as much feather is what
they want; the old rule gave them the same feather as a single-lane band and
left them sitting half-faded. The lane count is the only thing separating these
three rows from the eight above them: force every scene to one lane and all
twelve agree.

This is the one deliberate look change that touches an ordinary configuration.
It makes multi-lane bands crisper.

### 3.2 An INSIDE band wider than its corner radius

The old code described the band's inner boundary by pushing the rect's corner
radius **negative** and handing the shader the resulting field. That field's
zero set is the right shape - a shrunken, sharp-cornered box - but away from
that zero set its magnitude is not the distance to it. Near a corner the two
disagree by up to the corner's own size.

The new API takes an inner `RectGeometry`, so the shader gets that rect's *true*
distance field. The region is identical, and so is `depth` over the outer half
of the band. What differs is **`gSpan`**, by up to ~40% near a corner.

Measured before the feather changed, when `gSpan` still set it: of 1,649
differing pixels, **99.9% lay in a corner region and 46.5% of those were within
12 px of the OUTER boundary** - a distribution a `depth` explanation cannot
produce and a `gSpan` one predicts.

With the drop-relative feather shipped (section 1.6) that path is closed: the
corner's `gSpan` difference now reaches the output only through `runs`, the
trail cut and `RegionFade`'s clamp. What remains in this scene's row above is
dominated by its lane count, not by its corners.

The new behaviour is the more correct of the two. The old one is not reachable
through the new API at all, and nothing in it was load-bearing. Bands whose
erosion keeps the radius positive are unaffected - `INSIDE bw24` against radius
40 is byte-identical.

### 3.3 A band as wide as the rect

This was always the degenerate case: an INSIDE band 400 px wide on a rect whose
inradius is 180 covers the whole interior, so it *was* a filled pane, described
as a band. Everything the shader denominates in band widths then took its cue
from 400 px:

- the boundary feather floors at a quarter of the band, giving a **100 px
  vignette** eating inward from the rect's edge;
- `runs` transitions over a band width, so it sat at a constant ~0.5 everywhere
  - half-strength streaks and boosted beads, over the entire pane.

Said as a filled shape - `hasInner = false` - the same region gets a 1 px edge
and `runs` = 1: crisp boundary, full-length streaks, rain reaching the bottom
edge. That is what a pane of rain should look like, and it is why the region
model was worth building. The 254/255 in that row is the vignette going away.

---

## 4. What independent shapes buy, and the independence check

**A band expressed through the new API reproduces the old one exactly** where
the two describe the same thing: a 24 px OUTSIDE band, once as the old
`glowSide` + `bandWidth` pair and once as two explicit rects, renders 6,052 lit
pixels each with **0 differing channels**.

**The shapes the concentric model could not express all work:**

| scene | lit px |
| --- | --- |
| band thicker at the corners than the straights (sharp outer, round inner) | 10,596 |
| band that vanishes at the corners (round outer, sharp inner) | 4,342 |
| hole shoved 180 px right and 120 px down | 25,213 |
| pane with a bite out of one corner | 32,614 |
| filled shape elsewhere entirely, 240x520 at (40, 60) | 32,825 |

The off-centre cases are what forced `setupGeometry`'s four strips to take four
independent sides rather than a centred pair of half-extents. For a concentric
pair those four collapse back to the symmetric pair it used to build.

**Independence holds.** Starting from a droplet region and then changing
`Config::geometry` to a different rect (120x90 at (10,10) instead of 520x360)
and flipping `glowSide`: identical lit count, **0 differing channels**. The
droplets read neither.

---

## 5. The defect verification caught

The first two-shape build regressed two scenes badly - `INSIDE bw400` deviated
by up to **139/255 on 86% of lit pixels**, and `INSIDE bw90` with sharp corners
by up to **35/255 on 6.7%**. The other ten were within 1/255, which is what made
it worth chasing rather than dismissing.

The cause was `Dilate` flooring its outputs at zero:

```cpp
// wrong
return {base.center,
        glm::max(base.halfSize + t, glm::vec2(0.0f)),
        std::max(base.radius + t, 0.0f)};
```

Section 1.2's invariance needs half extents and radius to move **together**. An
INSIDE band wider than the corner radius - which is most of them - erodes the
radius negative, the floor pins it at 0, and `q` becomes some other rect's. Both
failing scenes are exactly that: `bw90` against radius 0, and `bw400` against
radius 40. `INSIDE bw24` against radius 40 never trips it and was byte-identical
throughout.

`bw400` was the worse of the two because it erodes past the shape's *inradius*:
the inner shape floors to a zero-size, zero-radius rect, whose distance field is
`length(p - centre)` - a point. `min()` reads that as a real boundary and masks
a disc out of the middle of the region.

The fix is to clamp nothing. The one place a floor is still right is the
corner-inset term in `setupGeometry`, which stands in for an arc a negative
radius does not have.

---

### 5.1 A zero-size inner shape is a point, not an empty set

`ResolveRegion` passed `inner` through whenever `hasInner` was set. A 0x0
`RectGeometry` does not describe an empty set to a distance field: `sdRoundBox`
with zero half extents reduces to `length(p)` - a POINT - and with one zero
extent to a line segment. Both have a perfectly good distance field, which
`min(dOut, dIn)` reads as a real boundary.

`setupGeometry` reached the opposite conclusion from the same input: the
inscribed box of a zero-size shape comes out empty, so it cut no hole and
uploaded a solid quad. The two disagreed.

The damage was not local. Because the region's local width is `dOut + dIn`, a
phantom point makes the span vary *radially* around it, which re-scales the drop
fade, the orientation mix and the trail cut everywhere. Measured on a 520x360
outer with a 0x0 inner at its centre, against the identical config with
`hasInner` false: **23% of lit pixels lost (52,511 -> 40,301), with differences
300 px away from a shape that has no size.** With the guard, the same comparison
is **0 differing pixels**.

The test is for DEGENERACY only. A small but real inner shape is a real
boundary and is honoured, even when it is too small for the draw geometry to
bother cutting around it - that direction only wastes fill.

### 5.2 The whole-drop fade was clamped in the wrong frame

`RegionFade` evaluates its depth at the DROP'S CENTRE but clamped the fade
radius with `gSpan`, which `main()` had set from the FRAGMENT. Those are the
same number for any concentric pair, so no perimeter band could tell - but once
the shapes are independent the span varies, and one drop got different fade
windows across its own body. That is the sliced-drop artefact the function
exists to prevent, reintroduced through the clamp.

It now takes the span from the same `RegionDistances` call that gives it the
depth, so both are in the drop's frame. Measured: concentric bands **0
differing pixels** across all twelve scenes, a filled shape **0** (no inner
boundary, so no clamp), and the four non-concentric shapes 18-44% of lit pixels,
max 81-225 - exactly the cases it was wrong for and nothing else.

---

## 6. One defect closed on the way past

`OnConfigChanged`'s geometry-rebuild gate did not include `neon.glowSide`,
while the resolve read it. Switching the glow side therefore left the ring sized
for the *previous* side until some other gated field happened to change,
clipping the band.

The bug is gone by construction rather than by being fixed: the resolve no
longer reads `neon.glowSide`, or `Config::geometry`, so neither can desynchronise
the buffer. The gate now lists exactly the three fields the region is built
from.

A third re-derivation of the same interval turned up in the demo while this
landed - `Detail::GetBandSpan` in `demo/src/ui-controls.h`, which inverted the
band a second time so the config dump could print it. It is gone too; the dump
prints the two rects.

---

## 7. Reproducing

The probes behind sections 2, 3 and 4 are offscreen (`OffscreenCapture` +
`CaptureUtil::WritePNG`, hidden GLFW window, ~150 lines each), built against
`edge-lighting` and `glfw` and deleted afterwards - the repo has no test target
and no `tools/` directory by convention. Section 3's before/after needs the
pre-change `droplets.frag`, `config.h` and `droplets-renderer.cpp`; build the
`edge-lighting` target alone, since the C ABI and both demos reference the new
fields.

`droplets-tuning.h` is gone. It held one constant, `DROPLET_BAND_GUARD` - the
margin past each boundary that the draw geometry and the shader's discard had to
agree on. The region rework made that margin provably zero: the boundary feather
is a hard zero at depth 0, so nothing outside the region is ever written and
there is nothing to clear. With no constant left to share, the header, its
`@DROPLETS_TUNING@` injection in `shaders.h.in` and its two entries in
`lib/CMakeLists.txt` were all removed. Three tuning headers remain.
