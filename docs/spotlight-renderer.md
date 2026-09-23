# Spotlight Renderer - Parameter Reference

Per-parameter behaviour of `SpotlightRenderer` and `Config::spotlight`, in the
shape [`effect-reference.md`](effect-reference.md) uses for the neon layer:
each field, its default, and what pushing it up or down does to the image.

For why the renderer is built the way it is - the solved strip bound, the
vertex-attribute per-lamp data, the resolution paths - see
[`spotlight-renderer-plan.md`](spotlight-renderer-plan.md). When either
disagrees with [`renderer/spotlight-renderer.h`](../lib/include/renderer/spotlight-renderer.h)
or [`core/config.h`](../lib/include/core/config.h), the headers win.

---

## 1. What the layer is

**Freely placed and aimed cones of light, and nothing else.** No backdrop, no
fixture housings, no floor, no framebuffer capture. The layer composites
additively over whatever is already there.

It is the odd one out in this library. Every other renderer parameterises its
look by position along the rect's perimeter; this one does not touch the rect
at all:

- A lamp sits at a `SpotLight::position` in **app coordinates** and points at a
  `SpotLight::angle`. **Moving `geometry.position` moves the rect and leaves
  the lamps where they are.** If a rig should travel with the frame, the caller
  moves it.
- **Nothing occludes a cone.** It crosses the rect, the droplets and the flare
  freely. There is no shadow model and no rect gating. The one way to stop
  light is `spotlight.clipArea` (section 4.6) - an explicit area, opted into per
  lamp, that cuts the light off rather than casting a shadow from anything.
- **Light only adds.** The pass is fully additive - `glBlendFuncSeparate(GL_ONE,
  GL_ONE, GL_ONE, GL_ONE)` - in the alpha channel as well as in colour, which
  also makes the layer order-independent. The order of
  `SpotlightConfig::lamps` cannot change the image.
- **It writes a coverage alpha**, the same max-of-channels rule neon and the
  flare use. It did not always: the shader emitted a literal `0.0`, which is
  invisible on a desktop window (opaque, nobody reads the alpha back) and
  erases the whole layer on an embedded surface a compositor or hardware video
  plane blends by alpha - `out = ui.rgb * ui.a + video * (1 - ui.a)` multiplies
  every lit pixel by zero. That was the Tizen bug: the spotlight missing over a
  playing video while every other layer came through. Measured offscreen at
  640x360 over a transparent clear, one lamp lit 72,615 pixels of colour and 0
  of alpha. Adding the alpha left the **colour byte-identical** in all six
  verification scenes, both resolution scales included.

Each lamp's brightness is the sum of two terms, both evaluated in that lamp's
own frame - `along` the axis and `across` it:

- **Cone** - a gaussian across the beam, faded in at the lamp, decaying along
  the throw, and divided by how wide the beam has become. That last factor is
  what stops a wide beam reading as brighter than a narrow one at equal
  intensity.
- **Aperture bloom** - an inverse-square glow centred on the lamp itself, the
  bright spill at the fixture.

Both are multiplied by one colour, `colorTemp` baked to linear RGB and scaled
by `tint`. The colour is per lamp and constant across it - there is no gradient
along or across a beam.

The full expression is written out at the top of
[`spotlight.frag`](../lib/shaders/spotlight.frag), because the renderer inverts
exactly it to decide where to stop drawing.

### There is no hard beam edge, by design

The cross-beam falloff is a **gaussian, not a smoothstep cut**. The look this
renderer targets has no visible cone boundary anywhere, so `softness` sets how
broad the gaussian is and there is deliberately no value that produces an edge.
If you want a sharp-edged cone, this is not the layer for it.

### The output is dithered, and it has to be

That gaussian is also the flattest gradient in this library: the outer cone
crosses one 8-bit step every 13 to 50 px at default settings. An RGBA8 target
quantises a gradient that flat into a few very wide bands, and since a cone's
iso-contours are near-straight rays, each band edge is a long straight line -
so the light stops looking like a pool and starts looking like a hard-edged
strip sitting on whatever is behind it.

`spotlight.frag` therefore adds half a destination step of ordered noise
before the framebuffer rounds, which spreads each band edge across the whole
band. `SPOT_DITHER_STEPS` in
[`spotlight-tuning.h`](../lib/include/renderer/spotlight-tuning.h) is the
knob; 0.0 turns it off and restores the exact pre-dither output. Nothing about
the falloff, the strip bound or the per-lamp cost changes with it. See **V11**
in [`review-findings.md`](review-findings.md) for the measurements, including
what the noise costs an eight-lamp rig.

If you are chasing a straight edge in this layer, read V11 before reading
`buildStrips`: the reported symptom points at the geometry and the cause was
not the geometry.

### The highlights roll off, so a hot core keeps its colour

A lamp's core peaks near `intensity * (1 + bloom)`, which is past full scale
for anything much brighter than `intensity` 0.7 at the default bloom. Written
straight to RGBA8 that clips each channel on its own - and clipping channels
one at a time is a **hue** change, not a brightness one, so an amber lamp's
core would walk up to white as `intensity` rose.

`spotlight.frag` therefore compresses the brightest channel through a Reinhard
shoulder starting at `SPOT_HIGHLIGHT_KNEE` (0.30) and scales the other two with
it, so the vector's ratios - the colour - survive. The shoulder is C1 continuous
at the knee and asymptotic above it, which matters for the same reason the
dither does: a hard clamp at 1.0 would put a visible contour around the core, in
a layer whose whole premise is that nothing here has an edge. Setting the knob
to 1.0 disables it and restores per-channel clipping.

**The knee is also what decides how hard `intensity` can be driven**, and that
is the reason it sits as low as 0.30. Raising `intensity` multiplies the whole
field; the cone's near field is already ~1 at the lamp, so what grows is not the
peak but the AREA at the top of the curve - the emitter stretches into a white
streak down the beam. At `intensity` 8 and `throwLength` 3000, pixels at or
above 240 go 8,577 at knee 0.75, 1,076 at 0.50, **29 at 0.30**, while the far
field is unchanged at every one of them. The cost is the pinpoint hotspot at low
intensity: a default lamp's core reads 156 rather than 211, and nothing from 100
px out is affected.

It is per LAMP, though, and the pass is additive - two beams overlapping
brightly can still clip their sum. See **V12**, **V12a** and **V12b** in
[`review-findings.md`](review-findings.md).

---

## 2. What the default `Config()` renders

**Nothing.** `spotlight.enable` defaults to `false` and `spotlight.lamps`
defaults to empty, so the layer costs one branch per frame until a host both
enables it and adds a lamp.

A default-constructed `SpotLight` is a fairly narrow, warm-white downlight:

| Field | Default |
|---|---|
| `position` | `(0, 0)` - the viewport's top-left corner |
| `angle` | `90` degrees (straight down) |
| `beamAngle` | `26` degrees |
| `throwLength` | `215` px |
| `apertureWidth` | `13` px |
| `softness` | `0.55` |
| `spreadFalloff` | `1.0` - the physical spread loss |
| `intensity` | `1.15` |
| `bloom` | `0.4` |
| `bloomRadius` | `20` px |
| `colorTemp` | `5600` K |
| `tint` | `(1, 1, 1)` - white, i.e. no tint |
| `enable` | `true` |

Note the position: a lamp added with no placement sits in the corner and throws
down the left edge. Both demos' Add button therefore place new lamps above the
rect rather than at the origin.

---

## 3. Coordinates

**App coordinates, origin at the viewport's TOP-LEFT, +x right, +y DOWN** - the
same space as `geometry.position`, and the reason the VBO can hold its vertices
verbatim (the y-flip lives in the projection, so a resize costs one uniform
rather than a rebuild).

`angle` is in degrees, `0` = +x (right), **increasing clockwise on screen**,
which is what a +y-down space makes natural:

| `angle` | Direction |
|---|---|
| `0` | right |
| `90` | down (the default) |
| `180` | left |
| `270` | up |

Values outside `[0, 360)` are fine - it is fed straight to `cos` / `sin`, so
`-90` and `270` are the same lamp. That also makes `angle` safe to drive with
an unbounded modulator; a sawtooth sweeping 0 to 360 rotates the lamp forever
without a wrap discontinuity in the image.

---

## 4. Field-by-field

### 4.1 Renderer top-level

**`spotlight.enable`** (default `false`)
Master switch. Nothing renders when false, and the scaled buffer (if one was
allocated) is released on the config change rather than held for the session.

**`spotlight.lamps`** (default empty)
The lamps. **At most `SPOT_MAX_LAMPS` (8) are drawn**, and the clamp is by
INDEX - entries from slot 8 on are ignored whatever their `enable` says, so a
rig whose only enabled lamps sit at slots 8 and 9 draws nothing. A longer list
is a legitimate thing to keep around, because indices (and any animation
binding that addresses them) stay stable while you enable a subset; the
renderer logs one line per overflow so the truncation is not silent.

**`spotlight.resolutionScale`** (default 1.0)
Fraction of the viewport the lamps are rendered at before being bilinear-
blitted back. `1.0` is the direct path: no offscreen buffer, no blit, nothing
allocated. Clamped to `[0.125, 1]` at draw time; above 1.0 is refused rather
than supersampled. **Read section 6 before lowering it** - unlike the neon and
flare scales, this one usually costs more than it saves, and it is **held at
1.0 outright while any enabled lamp is clipped** (section 4.6).

### 4.2 Placing and aiming

**`SpotLight::position`** (default `(0, 0)`)
Where the lamp is, in app coordinates (section 3). Not rect-relative.

**`SpotLight::angle`** (default 90 degrees)
Where it points (section 3).

**`SpotLight::enable`** (default `true`)
Skips the lamp without removing it from the list, so a host can keep indices
stable. A disabled lamp costs nothing but its slot - no geometry is emitted for
it - but see `spotlight.lamps` above for what "its slot" means.

### 4.3 Beam shape

**`SpotLight::beamAngle`** (default 26 degrees)
Full field angle of the cone. Clamped internally to `[0, 170]`, so the half
angle stays at or under 85 and its tangent stays finite - a "cone" at 180
degrees is a half-plane with no axis left to speak of, and the 10 degrees of
headroom under that limit keeps the tangent away from the knee where it stops
being a useful number rather than merely finite.

The beam has **no hard edge at this angle**; it is the width of the gaussian,
not a cut. Widening it does not brighten the lamp: the cone carries a factor of
`apertureWidth / (width at this distance)`, so the same light is spread over a
wider path. Narrow values read as a searchlight, wide ones as a wash.

**`SpotLight::throwLength`** (default 215 px)
Distance along the axis at which the beam falls to `1/e` of its peak. **Not
where it ends** - the renderer solves for that, and the visible reach is
several times this. Floored at 1 px.

Raising it is the control for "how far down the wall does this lamp read",
and it is also the main lever on how much geometry the lamp rasterises.

**It lights further without lighting brighter**, which is worth knowing because
the obvious alternative - raising `intensity` - blows the core out instead. At
the lamp its term is exactly `exp(0)`, so the core cannot move. Measured at
1920x1080, defaults otherwise, sampling brightness down the axis:

| `throwLength` | core | 400 px | 700 px | 1000 px | 1800 px |
| ------------- | ---- | ------ | ------ | ------- | ------- |
| 215 (default) | 211 | 6 | 1 | 0 | 0 |
| 900 | 211 | 23 | 10 | 5 | 1 |
| 3000 | 211 | 32 | 18 | 11 | 5 |

The cone's shape is untouched - `throwLength` does not appear in `halfW` - and
the lit half-width at a fixed distance converges rather than growing: 86 px at
200 px out for every value from 600 up, tracking the analytic cone half-width at
a constant ratio.

**It has a ceiling, and it is not the one you would guess.** Going from 3000 to
effectively infinite moves the value at 1000 px only from 11 to 16. It is NOT
the only factor in the cone that varies with distance along the axis - an
earlier version of this section said so, and the mistake matters, because the
other one is what is actually dimming the beam out there. Once the exponential
is spent the remaining decay is the `apertureWidth / halfW` spread term, which
falls off as roughly 1/distance - the beam's own divergence. At this lamp's
defaults, 1000 px out, the throw term at `throwLength` 3000 is still at **72%**
while the spread term is at **5%**.

So 900 to 1500 buys most of what `throwLength` has, and past 3000 there is
nothing; the demo sliders stop there for that reason. Reaching further than
that is `spreadFalloff`'s job.

**`SpotLight::spreadFalloff`** (default 1.0, clamped to `[0, 2]`)
The exponent on that spread term: the cone carries
`pow(apertureWidth / halfW, spreadFalloff)`. `1.0` is the physical
inverse-linear spread and what every lamp did before this field existed; `0.0`
removes the spread loss entirely, leaving `throwLength` as the only thing that
dims the beam with distance; above `1` decays faster than physical - a tighter
pool with a dimmer surround.

Nothing about the near field moves at any value: at the lamp `halfW` **is**
`apertureWidth`, so the term is 1 whatever it is raised to. Measured at
1920x1080 on one lamp at `throwLength` 3000, `intensity` 8, sampling the red
channel down the axis:

| `spreadFalloff` | core | 100 px | 300 px | 600 px | 1000 px | 1500 px | 2000 px |
| --------------- | ---- | ------ | ------ | ------ | ------- | ------- | ------- |
| 1.0 (default) | 216 | 216 | 174 | 125 | 78 | 45 | 28 |
| 0.75 | 216 | 224 | 199 | 168 | 135 | 99 | 70 |
| 0.5 | 217 | 230 | 217 | 201 | 183 | 160 | 139 |
| 0.25 | 217 | 235 | 230 | 223 | 216 | 205 | 194 |
| 0.0 | 217 | 240 | 239 | 237 | 235 | 231 | 227 |

**It costs fill, and that is the light travelling further rather than a tax on
it.** The strip is solved from this same falloff, so lowering the exponent grows
the geometry to match. Same lamp and viewport, fragments rasterised
(`GL_SAMPLES_PASSED`), against a 2,073,600 px viewport:

| `spreadFalloff` | fragments | of the viewport |
| --------------- | --------- | --------------- |
| 1.0 | 1,552,089 | 75% |
| 0.5 | 1,845,522 | 89% |
| 0.0 | 1,993,939 | 96% |

Bounded here only because the strip is already viewport-sized at this throw. In
open space it is not: at `spreadFalloff` 0 the solved reach is
`throwLength * log(solveIntensity / floor)`, about 25,000 px for this lamp, and
what stops the cost is the rasteriser rather than the solve. Lower it *with*
`throwLength`, not on top of it.

**Why an exponent and not some other reshaping.** The renderer inverts the whole
falloff in closed form to size the strip (section 6), and an exponent comes out
of that logarithm as a plain factor - `spreadFalloff * log(halfW / apertureWidth)`
in `SolveConeAcross`. The bound therefore stays exact at every value rather than
becoming a conservative guess. Verified against a CPU evaluation of the shader
over 2.07 M pixels at each of 1.0, 0.75, 0.5, 0.25 and 0.0: **zero pixels that
the falloff lights and the strip failed to draw**.

`1.0` is byte-identical to the pre-`spreadFalloff` renderer, and that took one
deliberate line in the shader: `pow(x, 1.0)` is `exp2(1.0 * log2(x))` on a GPU
and moved 15 channels out of 8.3 M by 1/255, so the exponent-1 case takes a
uniform branch back to the plain divide. Measured after that: 0 channels differ.

**Negative values are refused, not honoured.** A negative exponent turns the
spread term into gain, so the cone would brighten along its own axis without
limit and the solve would find no crossing to stop the strip at.

**`SpotLight::apertureWidth`** (default 13 px)
Half-width of the beam at the lamp itself, in px. Floored at 1 px.

It does two things at once, which is worth knowing before reaching for it:

- it sets how wide the beam starts, and therefore how quickly `beamAngle`
  actually opens it up;
- it sets how tight the bright core is, through the `apertureWidth / halfW`
  factor. A small aperture with a wide beam angle gives a hot, sharply
  narrowing core; a large aperture gives an even, slab-like beam.

It also scales the fade-in at the lamp, which runs from `-apertureWidth` to
`SPOT_NEAR_FADE * apertureWidth` along the axis - that fade is what stops a cone
painting backwards out of the fixture, and a wider aperture stretches it.

**`SpotLight::softness`** (default 0.55, range `[0, 1]`)
How broad the cross-beam gaussian is. `0` is the tightest, `1` the broadest;
the renderer maps it to the exponent between `SPOT_SOFT_MAX` and
`SPOT_SOFT_MIN`. Values outside the range are clamped.

Note this runs the *opposite* way to the exponent it drives - higher softness
is a smaller exponent - and that **no value produces a visible beam edge**
(section 1).

### 4.4 Brightness and colour

**`SpotLight::intensity`** (default 1.15)
Master brightness for this lamp, multiplying the cone and the bloom together.
`0` (or below) skips the lamp entirely: no geometry is emitted for it.

It is also the term the strip solve reads to decide how far the lamp reaches,
so raising it grows the geometry as well as the brightness. That is deliberate,
and it is the answer to "why did the lit area get bigger when I only asked for
more light" - a brighter lamp stays above the visibility floor further out. A
lamp that brightened without growing would be the bug.

**It no longer blows the core out.** The shading passes through a highlight
shoulder (`SPOT_HIGHLIGHT_KNEE`) that compresses the brightest channel and
scales the other two with it, so a hot core saturates in brightness while
holding its colour instead of clipping channel by channel and walking up
amber -> yellow -> white. See V12 in [`review-findings.md`](review-findings.md)
for the before/after, V12b for why the knee then had to come down to 0.30 to
stop the EMITTER growing as well as staying coloured, and V12a for the case
neither covers: the shoulder is per lamp, so two beams overlapping brightly can
still clip their sum.

**So this is now the lever for a stronger beam**, which it was not before.
Because the shoulder compresses above the knee and is exactly linear below it,
`intensity` adds light where the beam is dim and barely moves it where the beam
is already bright. At 700 px out, `intensity` 1.15 -> 8 gives 18 -> 112, a ratio
of 6.2 against a nominal 7.0, while the core moves only 156 -> 236. Measured at
1920x1080, `throwLength` 3000, at the shipped knee:

| `intensity` | core | 400 px | 700 px | 1000 px | 1800 px | px at full white |
| ----------- | ---- | ------ | ------ | ------- | ------- | ---------------- |
| 1.15 | 156 | 32 | 18 | 11 | 5 | 0 |
| 3 | 208 | 82 | 45 | 29 | 12 | 0 |
| 5 | 225 | 122 | 75 | 49 | 21 | 0 |
| 8 | 236 | 156 | 112 | 78 | 34 | 0 |

Pair it with `throwLength` for distance. The field is not clamped, and the demo
slider's top is a convenience rather than a limit.

**`SpotLight::colorTemp`** (default 5600 K)
Colour temperature, baked to linear RGB on the CPU from an eight-anchor
blackbody table and shipped per vertex. Below 1800 K and above 8000 K it
clamps to the end anchors.

| K | Reads as |
|---|---|
| 1800 | deep amber, candle / sodium |
| 2700 - 3200 | warm tungsten |
| 4000 | neutral warm |
| 5600 | daylight (the default) |
| 6500 | white |
| 8000 | cool blue-white |

**`SpotLight::tint`** (default `(1, 1, 1)`)
Linear RGB multiplied onto the colour `colorTemp` bakes. This is the only way
to reach a saturated lamp: the blackbody curve runs amber to white to
blue-white and cannot produce green, cyan or magenta at any Kelvin value.

Multiplies rather than replaces on purpose - a gel in front of a tungsten lamp
and the same gel in front of a daylight lamp are different colours, and keeping
both controls preserves that. White leaves `colorTemp`'s result untouched, so
the default costs nothing and changes nothing.

**Not clamped, and values above 1 are legal.** They brighten the lamp, and the
geometry follows: the renderer folds `max(r, g, b)` of the final colour into
the value it solves the strip bound against (`LampSolve::solveIntensity`), so a
boosted tint grows the lit region rather than being clipped by a strip that was
sized without it. The fold runs the other way too - a dim tint shrinks the
strip, measured at 51% of the untinted area for a tint peaking at 0.2.

An all-zero tint switches the lamp off exactly as `intensity` 0 does, and costs
the same: `DeriveLamp` bails before the solve, so no geometry is emitted.

### 4.5 Aperture bloom

**`SpotLight::bloom`** (default 0.4)
Strength of the inverse-square glow at the lamp itself - the bright spill
around the fixture, layered on top of the cone. `0` zeroes the term and, more
usefully, shrinks the geometry: with no bloom the strip is bounded by the cone
alone.

**`SpotLight::bloomRadius`** (default 20 px)
Size of that glow. Floored at 1 px.

**This is the pathological case for cost, not `throwLength`.** An
inverse-square term has no natural end, so the renderer windows it off at
`bloomRadius * SPOT_BLOOM_WINDOW_OUTER` and the strip has to contain that disc. At
the default 20 px the window closes at 160 px; at `bloomRadius` 90 it closes at
720 px, which fills a 1280x720 frame on its own. If one lamp is unexpectedly
expensive, look here first.

(The strip actually uses the *tighter* of that window and the distance at which
the inverse-square core drops below a visible level, so a faint bloom with a
large radius does not pay the full disc.)

### 4.6 The clip area

One rounded-rectangle region, in the same app coordinates as everything else in
section 3, that cuts light off. Its type is **`ClipArea`** (with `ClipMode`
beside it), a *shared* shape declared with `Cutoff` and `BlendSpace` at the top
of [`config.h`](../lib/include/core/config.h) rather than inside
`SpotlightConfig` - nothing about a region and which side of it to keep is
specific to this layer, so any renderer that wants an explicit bound can take
one. The spotlight is currently the only consumer. **The area is per layer; honouring it is per
lamp** - which is the split that matters, because the two kinds of lamp in a rig
want opposite answers. A lamp washing one panel should stop at its edge; a lamp
lighting the scene around it should cross the same boundary untouched.

**`SpotLight::clipped`** (default `false`)
Whether *this* lamp is cut off, and **the only gate there is** - the area
carries no enable of its own, so this bit alone decides whether the clip
touches a lamp. Off by default, so configuring a clip area changes nothing
until a lamp asks for it.

Setting it on any enabled lamp also holds `spotlight.resolutionScale` at 1.0
for the whole layer - see the end of this section.

> **Set the rectangle before setting this.** The area defaults to 0 x 0, and a
> zero-size `KEEP_INSIDE` area correctly keeps nothing - so a lamp opted in
> before the rect is configured goes dark, with nothing in the log. Nothing
> guards against that ordering now; the only protection is that `clipped`
> defaults off, so it can only happen to a caller already reaching for the
> clip. Both demo UIs say so when a lamp is opted in against a zero-size area.

**`spotlight.clipArea.position` / `.width` / `.height`** (default `(0,0)`, 0, 0)
The area: TOP-LEFT corner plus size, app coordinates. Zero size is a meaningful
value, not "unset" - see the warning above.

**`spotlight.clipArea.cornerRadius`** (default 0)
Corner rounding in px, clamped at draw time to half the shorter side. 0 is a
sharp rectangle.

**`spotlight.clipArea.edgeSoftness`** (default 1.0 px)
Width of the fade across the cut. **0 is a hard edge, and on this layer a hard
edge reads as an aliased one** - the cut runs through a smooth gradient, so
there is no contrast to hide the staircase the way a cut through a sharp feature
would. 1.0 is a single pixel of feather: enough to antialias the boundary and
nothing more. Larger values are a look, not a fix.

**Two names, one word apart, different scopes** - worth reading once:
`spotlight.clipArea` is the *region*, `SpotLight::clipped` is a *lamp's state*
and the only switch.
Both were called `clip` at first, one field apart, which made `light.clip` read
like an area and `spotlight.clip` read like a flag. The same split runs through
the C ABI (`..._clip_<noun>` addresses the area and takes no index;
`..._clipped` addresses a lamp and takes one) and into the shader, where the
per-lamp value is `aClipWeight` / `vClipWeight` - a lerp weight, named so it
cannot be confused with the *other* clip a vertex shader already has, clip
space.

**`spotlight.clipArea.mode`** (default `KEEP_INSIDE`)

| Mode | Keeps | Typical use |
|---|---|---|
| `KEEP_INSIDE` | light inside the area | confine a wash to one panel or region |
| `KEEP_OUTSIDE` | light outside the area | keep spill off a video surface or a screen the rect frames |

**It is a coverage multiply, not a change to the falloff.** A clipped lamp is
the same lamp with part of it missing: moving the area cannot make the light
that survives brighter, dimmer or differently shaped, and the coverage alpha is
read after the cut, so removed light stops claiming the surface as well as
stops colouring it. That last part matters on a surface something else
composites - see the alpha note in section 1.

**The area is deliberately NOT `Config::geometry`.** This renderer reads nothing
but `Config::spotlight`, and that is what keeps a rect move off its rebuild
path. Both demo UIs have a **Match Rect** button that copies the four numbers
across once; there is no binding, so a later rect move does not follow. Over the
C ABI that button is a one-liner, because `el_effect_set_spotlight_clip_rect`
takes the same five parameters in the same order as `el_effect_set_geometry`.

**Cost.** Under `KEEP_INSIDE` the solve intersects each clipped lamp's strip
with the area's box *in that lamp's frame*, so the clip buys fragments back
rather than only hiding them - a lamp whose beam mostly leaves the area draws a
correspondingly shorter strip, and one that misses it entirely draws nothing at
all. `KEEP_OUTSIDE` gets none of that: what survives there is the complement of
a bounded region, which is unbounded, so the strip stands as solved and the cut
is purely a fragment-stage multiply.

Verified offscreen at 640x360 over six clip scenes and four structural checks
(both modes, sharp and rounded areas, feathered and not, rotated lamps, an area
covering the whole viewport, a tiny area the beams barely reach, a zero-size
area, a mixed rig with one lamp clipped and one not, a configured area with no
lamp opted in, and `resolutionScale` 0.5):
no lit pixel survives where the mask is zero, and with the dither disabled every
pixel where the mask is exactly 1 is **byte-identical** to the unclipped render -
which is the evidence that the narrowed geometry loses nothing. With the dither
on, the same region differs on a handful of pixels by at most 2 LSB, because the
strip's corners moved and `spotDither` reads the interpolated lamp-frame
position.

**A clipped lamp holds `resolutionScale` at 1.0.** The mask is evaluated per
fragment, so on the scaled path its boundary is resolved at the *reduced
buffer's* texel pitch and the blit then smears that back to full resolution.
The mask's geometry is in app coordinates and survives untouched; its edge does
not. Measured across a `KEEP_INSIDE` boundary crossing bright light, in 1/255,
by destination pixel from the boundary:

| offset | scale 1.0 | scale 0.5 | scale 0.25 |
| ------ | --------- | --------- | ---------- |
| -1 | 255 | 255 | 191 |
| 0 | 183 | 128 | 128 |
| +1 | 0 | 0 | 64 |
| +2 (softness 4) | 0 | 28 | 0 |

Three things go wrong at once: the boundary moves by up to a destination pixel,
light leaks up to 64/255 *outside* an area whose whole job is to stop it, and
`edgeSoftness` stops meaning anything below one buffer texel - at 0.25, softness
1 and softness 4 render identically. This is the failure
[`neon-blit.frag`](../lib/shaders/neon-blit.frag) records at length, and the
neon fixed it by moving its one-sided cut into the full-res blit. **That fix
does not transfer here**, because `clipped` is per lamp while the blitted buffer
holds every lamp's light summed together: a mask applied at blit time would cut
the lamps that opted out along with the ones that opted in. Separating them
costs a second buffer and a second blit, and the blit is already the fixed cost
that makes this scale marginal (section 6) - so there would be nothing left to
win.

`GetClampedSpotScale` therefore returns 1.0 whenever a drawn lamp is clipped.
The configured value is kept, not rewritten, so dropping the clip brings it back,
and both transitions are logged. Verified on-device at 640x360 on a two-lamp rig
with one lamp clipped: requesting 0.5 or 0.25 is now byte-identical to 1.0, where
before the pin they differed by up to **65** and **101** of 255 respectively -
against a max deviation of 11 for the same scale change with no lamp clipped,
which is what ordinary resolution loss looks like. Both demo UIs say so beside
the slider, since a knob that moves and changes nothing otherwise reads as
broken.

---

## 5. Animating a lamp

Every spotlight scalar is per-lamp, so there is no spotlight block in
`AnimatableField`. `SpotlightField`
([`field-bound-animation.h`](../lib/include/animation/field-bound-animation.h))
is the whole animatable surface: `POSITION_X`, `POSITION_Y`, `ANGLE`,
`BEAM_ANGLE`, `THROW_LENGTH`, `APERTURE_WIDTH`, `SOFTNESS`, `INTENSITY`,
`BLOOM`, `BLOOM_RADIUS`, `COLOR_TEMP`, `TINT_R`, `TINT_G`, `TINT_B`,
`SPREAD_FALLOFF`. Bind one with

```cpp
anim->AddSpotlightField(lampIndex, SpotlightField::ANGLE, modulator);
```

No auto-grow: `spotlight.lamps` must already hold `lampIndex`, and an
out-of-range index at apply time is a logged no-op.

**One cost note.** The strip's vertices are solved on the CPU and hold app
coordinates, so driving *any* of these fields makes the renderer rebuild its
vertex buffer on every frame the value moves. Most of them earn it - `POSITION_*`
and `ANGLE` move the strip, and `BEAM_ANGLE`, `THROW_LENGTH`, `APERTURE_WIDTH`,
`SOFTNESS`, `INTENSITY`, `BLOOM`, `BLOOM_RADIUS`, `SPREAD_FALLOFF` and the three
`TINT_*` all feed the solve that sizes it - the tint through the brightest-channel fold
described in 4.4. `COLOR_TEMP` is the only one that can change the bound
without obviously looking like it does, for the same reason.

The rebuild is bounded and small - tens of microseconds for a full eight-lamp
rig, sub-data into a buffer allocated once at its ceiling - but it is not free
the way animating a pure shader uniform is.

---

## 6. Resolution and cost

`SpotlightConfig::resolutionScale` behaves exactly as the neon's and the
flare's do - same two paths, same blit - and **not one uniform differs between
them**, so a scaled frame is the same picture at lower resolution rather than a
differently shaped one.

One exception, and it is a hard gate rather than a caveat: **an enabled clipped
lamp holds the scale at 1.0**, so everything in this section describes an
unclipped rig. Section 4.6 has the measurements and the reason.

**But the bargain is not the same, and that is why the default is 1.0.** Neon
and the flare shade the whole viewport, so quartering their fragments always
beats a blit. This layer already bounds its geometry to what the lamps light,
so the blit's full viewport of fragments is a **fixed cost it may never earn
back**.

Measured with an occlusion query at 1280x720 (fragments, and as a percentage of
the 921,600-fragment viewport):

| scene | scale 1.0 | scale 0.5 | scale 0.25 |
| ----- | --------- | --------- | ---------- |
| one lamp | 183,868 (20%) | 967,599 (105%) | 933,076 (101%) |
| five-lamp fan | 1,131,073 (123%) | 1,204,301 (131%) | 992,285 (108%) |
| eight lamps | 1,900,287 (206%) | **1,396,661 (152%)** | **1,040,340 (113%)** |

A single lamp at 0.5 costs **5.3x more** than at 1.0. The crossover is where
the full-res strips exceed `blit / (1 - scale^2)`: at 0.5 that is about 1.23M
fragments, which the five-lamp fan sits just under and the eight-lamp rig
clears - winning 1.36x at 0.5 and 1.83x at 0.25. At 0.75 nothing in range wins
at all.

**Lower it only for a large rig, and measure.** Both demo UIs warn inline once
the scale drops below 1.0.

Two other cost facts worth having:

- Against a fullscreen pass looping over the lamps, the bounded strips are a
  **4.1x saving at five lamps**. Flattening the taper into an oriented bounding
  box of the same solve costs **1.16x to 1.50x** more for an identical image.
- A large `bloomRadius` dominates everything else (section 4.5).

---

## 7. C ABI

The lamp surface is parameterised by index and grouped by concern rather than
one function per scalar, mirroring the arc family:

| Function | Covers |
|---|---|
| `el_effect_set_spotlight_renderer_enabled` | `spotlight.enable` |
| `el_effect_set_spotlight_resolution_scale` | `spotlight.resolutionScale` |
| `el_effect_set_spotlight_count` | list length (resizes from the end) |
| `el_effect_clear_spotlights` | empties the list |
| `el_effect_set_spotlight_placement` | `position.x`, `position.y`, `angle` |
| `el_effect_set_spotlight_beam` | `beamAngle`, `throwLength`, `apertureWidth`, `softness` |
| `el_effect_set_spotlight_look` | `intensity`, `bloom`, `bloomRadius`, `colorTemp` |
| `el_effect_set_spotlight_spread_falloff` | `spreadFalloff` |
| `el_effect_set_spotlight_tint` | `tint.r`, `tint.g`, `tint.b` |
| `el_effect_set_spotlight_enabled` | per-lamp `enable` |
| `el_effect_set_spotlight_clipped` | per-lamp `clipped` |
| `el_effect_set_spotlight_clip_rect` | `clipArea.width`, `.height`, `.position.x`, `.position.y`, `.cornerRadius` |
| `el_effect_set_spotlight_clip_softness` | `clipArea.edgeSoftness` |
| `el_effect_set_spotlight_clip_mode` | `clipArea.mode` (`el_clip_mode_e`) |

`el_effect_set_spotlight_clip_rect` takes the **same five parameters in the same
order as `el_effect_set_geometry`** - width, height, x, y, cornerRadius - so
lining the clip up with the rect is a straight forward of one call's output into
the other. There is still no binding between them; a later rect move does not
follow. `edgeSoftness` is a scalar setter of its own rather than a sixth
parameter: it is not part of the shape, and retuning it should not re-send four
numbers that did not change.

Each has a matching `el_effect_get_*`. The tint and `spreadFalloff` are calls of
their own rather than extra parameters on `el_effect_set_spotlight_look` and
`el_effect_set_spotlight_beam`, both already published with four - this ABI
parameterises rather than re-signs. None of them grows the list, so
`el_effect_set_spotlight_count` comes first; an out-of-range index is a logged
`EL_ERROR_INVALID_PARAMETER`. The layer is registered by
`EL_RENDERER_SPOTLIGHT` (and by `EL_RENDERER_ALL`, which is what
`el_effect_init` uses). Animation bindings go through
`el_animation_add_spotlight_field` with an `el_spotlight_field_e`.

Remember that every `el_effect_set_*` writes the handle's **staging** config
and only `el_effect_update` hands it to the effect - a host that sets lamps and
then calls only `el_effect_render` renders the previous frame's rig.

---

## 8. Interaction cheatsheet

| To change... | ...touch this |
|---|---|
| Where the lamp is, where it points | `position`, `angle` |
| How wide the cone opens | `beamAngle` |
| How far down the throw it reads | `throwLength` (spent by ~3000 px) |
| Reach further than that | `spreadFalloff` below 1 - the term that limits reach at range |
| How tight the bright core is | `apertureWidth` (small = hot core) |
| How soft the beam edge is | `softness` (there is no hard edge at any value) |
| Overall brightness of one lamp | `intensity` |
| The glow at the fixture | `bloom`, `bloomRadius` |
| Warm vs cool | `colorTemp` |
| A saturated or gelled colour | `tint` (multiplies `colorTemp`; above 1 brightens) |
| Turn one lamp off, keep its index | `SpotLight::enable` |
| Cut one lamp off at an area's edge | `SpotLight::clipped` + `spotlight.clipArea.*` |
| Keep light out of a region (a video surface, say) | `clipArea.mode = KEEP_OUTSIDE` |
| Confine light to a region | `clipArea.mode = KEEP_INSIDE` (the default) |
| Soften or sharpen the cut | `clipArea.edgeSoftness` (0 aliases; 1.0 is one pixel) |
| Turn the whole layer off | `spotlight.enable` |
| Trade quality for speed (large rigs only) | `spotlight.resolutionScale` |

Known rough edges in this layer are recorded in
[`review-findings.md`](review-findings.md#sixth-pass-the-spotlight-renderer-review).
