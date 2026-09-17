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
  freely. There is no shadow model and no rect gating.
- **Light only adds.** The pass is fully additive - `glBlendFuncSeparate(GL_ONE,
  GL_ONE, GL_ONE, GL_ONE)` - in the alpha channel as well as in colour, which
  also makes the layer order-independent. The order of
  `SpotlightConfig::lights` cannot change the image.
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

---

## 2. What the default `Config()` renders

**Nothing.** `spotlight.enable` defaults to `false` and `spotlight.lights`
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

**`spotlight.lights`** (default empty)
The lamps. **At most `SPOT_MAX_LIGHTS` (8) are drawn**, and the clamp is by
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
flare scales, this one usually costs more than it saves.

### 4.2 Placing and aiming

**`SpotLight::position`** (default `(0, 0)`)
Where the lamp is, in app coordinates (section 3). Not rect-relative.

**`SpotLight::angle`** (default 90 degrees)
Where it points (section 3).

**`SpotLight::enable`** (default `true`)
Skips the lamp without removing it from the list, so a host can keep indices
stable. A disabled lamp costs nothing but its slot - no geometry is emitted for
it - but see `spotlight.lights` above for what "its slot" means.

### 4.3 Beam shape

**`SpotLight::beamAngle`** (default 26 degrees)
Full field angle of the cone. Clamped internally to `[0, 170]` so the half
angle stays under 90 and its tangent stays finite - a "cone" at 180 degrees is
a half-plane with no axis left to speak of.

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
so raising it grows the geometry as well as the brightness.

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
`bloomRadius * SPOT_BLOOM_SUPPORT` and the strip has to contain that disc. At
the default 20 px the window closes at 160 px; at `bloomRadius` 90 it closes at
720 px, which fills a 1280x720 frame on its own. If one lamp is unexpectedly
expensive, look here first.

(The strip actually uses the *tighter* of that window and the distance at which
the inverse-square core drops below a visible level, so a faint bloom with a
large radius does not pay the full disc.)

---

## 5. Animating a lamp

Every spotlight scalar is per-lamp, so there is no spotlight block in
`AnimatableField`. `SpotlightField`
([`field-bound-animation.h`](../lib/include/animation/field-bound-animation.h))
is the whole animatable surface: `POSITION_X`, `POSITION_Y`, `ANGLE`,
`BEAM_ANGLE`, `THROW_LENGTH`, `APERTURE_WIDTH`, `SOFTNESS`, `INTENSITY`,
`BLOOM`, `BLOOM_RADIUS`, `COLOR_TEMP`, `TINT_R`, `TINT_G`, `TINT_B`. Bind one
with

```cpp
anim->AddSpotlightField(lampIndex, SpotlightField::ANGLE, modulator);
```

No auto-grow: `spotlight.lights` must already hold `lampIndex`, and an
out-of-range index at apply time is a logged no-op.

**One cost note.** The strip's vertices are solved on the CPU and hold app
coordinates, so driving *any* of these fields makes the renderer rebuild its
vertex buffer on every frame the value moves. Most of them earn it - `POSITION_*`
and `ANGLE` move the strip, and `BEAM_ANGLE`, `THROW_LENGTH`, `APERTURE_WIDTH`,
`SOFTNESS`, `INTENSITY`, `BLOOM`, `BLOOM_RADIUS` and the three `TINT_*` all
feed the solve that sizes it - the tint through the brightest-channel fold
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
| `el_effect_set_spotlight_tint` | `tint.r`, `tint.g`, `tint.b` |
| `el_effect_set_spotlight_enabled` | per-lamp `enable` |

Each has a matching `el_effect_get_*`. The tint is its own call rather than two
more parameters on `el_effect_set_spotlight_look`, which is already published
with four - this ABI parameterises rather than re-signs. None of them grows the list, so
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
| How far down the throw it reads | `throwLength` |
| How tight the bright core is | `apertureWidth` (small = hot core) |
| How soft the beam edge is | `softness` (there is no hard edge at any value) |
| Overall brightness of one lamp | `intensity` |
| The glow at the fixture | `bloom`, `bloomRadius` |
| Warm vs cool | `colorTemp` |
| A saturated or gelled colour | `tint` (multiplies `colorTemp`; above 1 brightens) |
| Turn one lamp off, keep its index | `SpotLight::enable` |
| Turn the whole layer off | `spotlight.enable` |
| Trade quality for speed (large rigs only) | `spotlight.resolutionScale` |

Known rough edges in this layer are recorded in
[`review-findings.md`](review-findings.md#sixth-pass-the-spotlight-renderer-review).
