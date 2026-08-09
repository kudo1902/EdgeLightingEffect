# Neon Edge Lighting - Effect Reference

Reference for the neon renderer's visual model. Covers what the effect is, what
you get with the default `Config()`, and how each field in the config affects
the rendered pixels.

This document is a reader-facing complement to
[`architecture-design.md`](architecture-design.md) (which explains *how* the
code is put together) - here we focus on *what the parameters do to the
picture*.

Field defaults come from [`lib/include/core/config.h`](../lib/include/core/config.h);
tuning constants from
[`lib/include/renderer/neon-tuning.h`](../lib/include/renderer/neon-tuning.h).
When a doc value drifts from those headers, the headers win.

---

## 1. What the effect is

A single fragment shader that draws an animated neon-style glow along the
perimeter of a rounded rectangle. The image is the sum of three coloured
layers, all evaluated per pixel in HDR and tone-mapped together at the end:

- **Filament** - the sharp bright line right on the perimeter. A generalised
  Gaussian falloff perpendicular to the edge. Peak brightness is always 1.0
  on-axis; you shape only the width and the sides.
- **Halo** - a coloured glow that spreads from the line. Sampled from a set
  of pre-baked positions around the perimeter (a "gather loop"), each sample
  weighted by an inverse-square kernel. This is where the *colour* of the
  effect is picked up.
- **Bloom** - a wider, softer background spill layered on top of the halo.
  Same gather loop, wider kernel, gentler falloff. Adds ambient bleed.

On top of that, two independent perimeter-space features can further shape
what you see:

- **Arcs** - one or more slices of the perimeter that are "on". Everything
  outside an arc is dark; where arcs overlap, the winner-take-all rule picks
  the one with the largest mask * intensity at that sample. Default: one arc
  covering the whole perimeter.
- **Segments** - travelling additive lights, each a Gaussian bump on the
  perimeter with its own colour. They're summed on top of the arc emission
  and are independent of the master intensity, so a segment can shine on an
  otherwise-dark arc.

The neon renderer is one of three renderers that can be enabled independently
in the same effect: `NeonRenderer`, `NeonOptimizedRenderer` (half-res variant
of the same shader), and `DebugRenderer` (every diagnostic overlay - bounding
box, gradient LUT strip, colour-stop markers - in one droppable layer).

---

## 2. What the default `Config()` renders

If you construct a fresh `Config` and enable the neon renderer, this is what
comes out. Every value is the compiler-visible default from `config.h`.

| Group | Field | Default |
|---|---|---|
| Geometry | width x height | 800 x 600 pixels |
| Geometry | position | (0, 0) - top-left of the framebuffer |
| Geometry | cornerRadius | 40 px |
| Geometry | winding | CCW (top-left -> left -> bottom -> right -> top) |
| Neon | enable | **false** - the renderer is off by default; the host opts in |
| Neon | opaque | false (premultiplied blend onto the framebuffer) |
| Neon | lineWidth | 4 px |
| Neon | filamentFalloff | 1.0 (pure Gaussian) |
| Neon | intensity | 1.0 |
| Neon | glowRadius | 5 px |
| Neon | bloomStrength | 0.3 |
| Neon | glowSide | BOTH |
| Neon | glowSideSoftness | 0 (only used when glowSide != BOTH) |
| Neon | blendSpace | RGB |
| Neon | colorStops | red(0.00), green(0.25), blue(0.50), yellow(0.75) |
| Neon | hueRotationRate | 0.5 revolutions / second |
| Neon | arcs | one arc {start=0, length=1, intensity=1, no stops} - full perimeter lit |
| Neon | segmentBoosts | empty - no travelling lights |
| Neon | colorTransitionDuration | 0.3 s |
| Optimized | enable | false |
| Debug | enable | true |
| Debug | showWireframe | true |
| Debug | wireframeColor | opaque green |
| Debug | showGradientLUT / showColorStops | false |
| Debug | showArcMarkers / showSegmentMarkers | false |

Turning `neon.enable = true` on top of these defaults gives you a full-loop
rainbow (red -> green -> blue -> yellow, blending back to red) marching slowly
around an 800x600 rectangle at 0.5 rev/s, with a modest 5 px halo and a light
bloom bleed. That's the effect's "hello world" - the shader and defaults are
tuned so this looks like a real neon strip out of the box.

---

## 3. Field-by-field visual reference

Each section below lists the config field, its default, and *what pushing it
up or down does to the rendered image*. Where a field interacts with others,
that's called out.

### 3.1 Geometry

**`geometry.width`, `geometry.height`** (default 800 x 600 px)
Rectangle bounding size in framebuffer pixels. Directly sets the perimeter
length used for arc / segment / colour-stop positions. Doubling one axis
stretches the neon over a longer path but does not change its brightness
per-pixel.

**`geometry.position`** (default (0, 0))
Top-left corner of the rectangle in framebuffer pixels. y grows downward.

**`geometry.cornerRadius`** (default 40 px)
Radius of the four rounded corners. `0` = sharp corners; large radii round
the rectangle toward a stadium / pill shape. Constrained by the shader to
at most half the shorter axis (values above are clamped internally).

**`geometry.winding`** (default `COUNTER_CLOCKWISE`)
Direction the perimeter is traversed to build the parameter `s in [0, 1)`
used for colour stops, arcs, segments, and hue rotation.
- `COUNTER_CLOCKWISE`: `s=0` at top-left, then left -> bottom -> right -> top.
- `CLOCKWISE`: `s=0` at top-left, then top -> right -> bottom -> left.

Flipping winding mirrors every position-based effect (colour stop positions,
arc starts, segment positions) around the top-left corner.

### 3.2 Neon renderer - top-level

**`neon.enable`** (default `false`)
Master switch for the single-pass neon renderer. Nothing renders when
`false` - not the filament, not the halo, not the arcs, not the segments.
The host must set this to `true` to see any neon at all; the demo does this
in its startup code.

**`neon.opaque`** (default `false`)
- `false`: premultiplied "over" blend - dark surround is transparent, the
  effect composites onto whatever was in the framebuffer.
- `true`: the surround is filled with `neon.opaqueColor` first (occluding
  the background), and the neon glow is composited on top. Useful when you
  want the effect to *replace* the background inside its draw region.

**`neon.opaqueColor`** (default black `(0, 0, 0, 1)`)
RGBA colour used to fill the surround when `neon.opaque = true`. Only the
`.rgb` is used today; `.a` is reserved.

### 3.3 Filament (the bright line)

**`neon.lineWidth`** (default 4 px)
Width of the bright filament in pixels. Peak brightness on the axis stays
**exactly 1.0 regardless of lineWidth** - only the width changes, so a wider
line reads as "more bright pixels" rather than "a hotter line".

**`neon.filamentFalloff`** (default 1.0)
Shape of the brightness falloff perpendicular to the axis. The shader uses
`exp(-ln(2) * (ad/sigma)^N)` where `N = 2 * filamentFalloff`.

| Value | N | Look |
|---|---|---|
| 0.5 | 1 | Laplace-like - heavy tails, very smooth peak |
| 1.0 | 2 | Pure Gaussian - clean roll-off (default) |
| 2.0 | 4 | Platykurtic - flatter top, sharper shoulder |
| >= 3.0 | >= 6 | Near-rectangular plateau, crisp edges |

Higher values give a "hard neon rod" feel; lower values a "diffuse plasma"
feel. Peak brightness stays 1.0 for every value.

### 3.4 Halo + Bloom + Intensity

**`neon.intensity`** (default 1.0)
Master multiplier applied to the arc emission (filament + halo + bloom
together). Uniform across the whole effect. To dim one slice of the
perimeter without affecting the others, use `Arc::intensity` instead - those
are per-slice. Travelling segments *deliberately bypass this multiplier* so
a segment stays lit even when the master goes to 0.

`0` = arcs go black (segments still visible if any). `2 - 3` = HDR overdrive
that pushes even the mid-gradient colours toward the tone-map knee.

**`neon.glowRadius`** (default 5 px)
Reach of the sharp coloured halo around the filament, in pixels. Also seeds
the wider bloom kernel (`bloomReach = glowRadius * BLOOM_REACH_TO_GLOW`,
tuning constant = 6). Larger radii bleed more colour into the surround;
small radii keep the effect a tight ring around the line.

Ranges: 1 - 10 = subtle, 10 - 30 = classic neon, 30 - 80 = heavy wash.

**`neon.bloomStrength`** (default 0.3)
How much of the wide background spill is layered on top of the halo.
- `0` = halo only, no ambient bleed
- `0.3` = subtle atmospheric wash (default)
- `1.0+` = strong glow into the background, the effect starts to dominate

**`neon.glowSide`** (default `BOTH`)
Which side of the perimeter line the halo + bloom are allowed to spill onto.
- `BOTH`: symmetric neon (default look).
- `INSIDE`: only the interior of the rectangle receives glow; outside stays
  dark. Reads like a "backlit panel".
- `OUTSIDE`: only the exterior receives glow; interior stays dark. Reads
  like a "sign lit from behind the frame".

**`neon.glowSideSoftness`** (default 0)
Softness of the one-sided cut in pixels. `0` = hard edge along the axis;
`~2` = a subtle feather to hide the aliased transition. Ignored when
`glowSide == BOTH`.

### 3.5 Colour and hue rotation

**`neon.blendSpace`** (default `RGB`)
How the shader interpolates *between* consecutive colour stops.
- `RGB`: linear per-channel blend. Two complementary colours pass through a
  muddy mid-grey (red-cyan -> muddy grey mid-point).
- `HSV`: hue-space blend. Preserves saturation across the interpolation -
  red -> cyan passes through yellow / green.
- `HSL`: like HSV but the L component gives smoother mid-tones through
  perceptual grey - a middle-ground choice.

The debug UI exposes this per-arc and per-segment too (each of those
inherits the base if empty).

**`neon.colorStops`** (default the R/G/B/Y quartet at 0, 0.25, 0.5, 0.75)
Ordered list of `{position, RGBA}` stops laid around the perimeter, where
`position in [0, 1)` maps to the perimeter parameter `s`. The renderer bakes
these into a 256-texel LUT (REPEAT-wrapped, so the gradient loops seamlessly
at `s=1 -> 0`) and the shader reads the LUT once per gather-loop sample.

- 1 stop = solid colour.
- 2 stops = linear gradient (subject to `blendSpace`).
- 3 or more = multi-stop circular gradient.

Cap: `MAX_COLOR_STOPS = 128` (see `neon-renderer.h`). The debug UI caps its
adder at the same value.

**`neon.hueRotationRate`** (default 0.5 rev / s)
How fast the whole gradient scrolls around the perimeter, in revolutions per
second. Positive values scroll *with* the winding; negative values scroll
against.
- `0` = static gradient pinned to the perimeter.
- `0.5` = one full lap of the perimeter every 2 seconds (default).
- `~2+` = clearly kinetic; useful for arcade / marching-lights vibes.

Also drives the per-arc gradient sample when arcs have their own stops:
inside an arc, the arc's LUT scrolls through the arc's window at the same
rate (see [`multiple-arcs-design.md`](multiple-arcs-design.md) for the
sampling model).

**`neon.colorTransitionDuration`** (default 0.3 s)
Seconds to cross-fade the *baked LUT* when the colour stops or blend space
change. Because the fade blends the whole 256-texel LUT rather than pairing
individual stops, it works even when the two stop sets differ in count or
position. `0` = instant snap (the old behaviour).

### 3.6 Arcs (perimeter gating)

Default: `arcs = { Arc{} }` - one arc, `{start = 0, length = 1, intensity =
1, no stops}`, i.e. the full perimeter is lit at full intensity with colour
inherited from the base gradient. That's why the effect "just works" out of
the box: the single default arc covers everything.

**`Arc::start`** (default 0.0)
Where the arc starts on the perimeter, in `[0, 1)`. Wrapping is automatic:
`start = 0.8, length = 0.4` lights `[0.8, 1.0] + [0.0, 0.2]`.

**`Arc::length`** (default 1.0)
Fraction of the perimeter the arc covers. `0` = arc contributes nothing;
`1` = full perimeter (the default).

**`Arc::intensity`** (default 1.0)
Per-arc brightness multiplier, applied *inside* the arc's mask and
*independent* of `neon.intensity`. Two arcs at intensities 1.0 and 0.5 render
one bright and one dim slice; winner-take-all in overlap regions picks the
larger `mask * intensity`.

**`Arc::colorStops`** (default empty)
Colour stops laid across the arc's own span, head-to-tail:
- `position = 0` = start of the arc
- `position = 1` = end of the arc

The shader samples this in arc-local space, so an arc from `0.30 - 0.50`
with stops `red(0), blue(1)` renders red at s=0.30 and blue at s=0.50 - not
what would be at those positions in a perimeter-space gradient.

**Empty stops = inherit the base gradient in perimeter space**. That
keeps a plain "on / off gating" arc visually continuous with the rest of the
perimeter.

**`Arc::blendSpace`** (default `RGB`)
Interpolation space for the arc's own `colorStops`. Ignored when stops are
empty (the base gradient's blend space applies then).

**`NeonConfig::MAX_ARCS_CAP`** (constant = 8)
Hard ceiling on the number of concurrent arcs, matching the shader UBO layout.

**Overlap resolution** - winner-take-all: for each perimeter sample, the arc
with the largest effective mask (`arcInside * intensity`) owns the emission
there. Because `arcInside` is smoothstepped one-sample-wide at each end,
adjacent arcs of different colours crossfade smoothly at the seam without
any special blend logic.

### 3.7 Segments (travelling additive lights)

Default: empty vector - no segments, no cost beyond one skipped inner
iteration per sample.

**`SegmentBoost::position`** (default 0.0)
Centre of the segment on the perimeter, `[0, 1)`. Wraps naturally at the
`0/1` seam.

**`SegmentBoost::length`** (default 0.15)
Segment's visible span as a fraction of the perimeter (~2σ of the Gaussian).
`0.05` = tight comet spot, `0.2 - 0.3` = wide traveller.

**`SegmentBoost::boost`** (default 0.0)
Peak brightness of the segment - added on top of the base arc emission, so
segments **stay lit even when arc / master intensity is 0**. That's the key
difference from arcs: segments compose additively, arcs multiply.

**`SegmentBoost::colorStops`** (default empty)
Same convention as arcs - stops laid across the segment's own span, in
head-to-tail order (segment-local space, so `position=0` = the leading edge
of the moving spot). Empty = inherit the base gradient at each sample the
segment touches (matches the old "just a bright spot in the base colour"
feel).

**`SegmentBoost::blendSpace`** (default `RGB`)
Interpolation space for the segment's own stops; ignored when empty.

**`NeonConfig::MAX_SEGMENT_BOOSTS_CAP`** (constant = 8)
Hard ceiling on concurrent segments (shared with the shader UBO layout).

Typical animation patterns:
- `SegmentTravel(duration, length, boost)` - one comet-like spot that
  circulates the perimeter every `duration` seconds.
- `SegmentBounce(...)` - swings back and forth via a triangle-wave modulator.
- `Comet(...)` = a tight fast `SegmentTravel`.

### 3.8 Debug visualisations

All of these are drawn by `DebugRenderer`, the pipeline's single diagnostic
layer, and live under `Config::debug`. None of them is part of the effect:
leave the renderer unregistered in production and they cost nothing at all -
no shader compiles, no geometry, no draws.

**`debug.enable`** (default `true`)
Master switch for the layer. Off hides every overlay below regardless of
their own flags.

**`debug.showWireframe`** (default `true`)
Draws a 1px `GL_LINE_LOOP` around the rectangle.

**`debug.wireframeColor`** (default opaque green `(0, 1, 0, 1)`)
Colour of that line.

**`debug.showGradientLUT`** (default false)
Draws the baked 256-texel gradient LUT as a horizontal strip across the
centre of the rectangle. Handy to eyeball what colours the shader is
actually sampling from. The strip bakes its own copy of the ring from
`neon.colorStops`, cross-fade included, so it works even with both neon
renderers disabled.

**`debug.showColorStops`** (default false)
Draws a coloured dot at each colour-stop position on the perimeter so you
can verify the `position -> colour` mapping against the LUT strip and the
rendered halo.

**`debug.showArcMarkers`** (default false)
Draws a chevron at each arc's start and end, just outside the perimeter.
Both chevrons of a pair point *into* the lit span - the start one along the
direction of travel, the end one against it - and are coloured green for
start, red for end. So an arc's extent, and which way it runs under the
current `geometry.winding`, are readable without converting perimeter
fractions to pixels. An arc covering the whole perimeter has no boundary,
so it gets a start marker only.

**`debug.showSegmentMarkers`** (default false)
Draws a diamond at each segment's centre position, just inside the
perimeter, in that segment's own first stop colour (white when it has no
stops and inherits the base gradient). Covers the merged pool
(`neon.segmentBoosts` + `neon.preservedSegmentBoosts`) that the renderers
actually draw, so a travelling segment can be tracked while it animates.

The three marker families use different shapes and sit on / outside /
inside the perimeter respectively, so they stay legible together.

Register `DebugRenderer` last - its overlays are meant to sit on top of the
finished frame.

### 3.9 Optimized neon (half-res variant)

The optimized renderer runs the same shader into a **downscaled FBO** then
bilinear-blits it back to full resolution. All visual params (line width,
intensity, colour stops, arcs, segments, hue rotation, ...) are read from
`Config::neon` - the optimized renderer only adds its own perf-shaping
fields:

**`optimizedNeon.enable`** (default false)
Master switch for the half-res path. Can run concurrently with the full-res
neon; the two composite additively, which is useful for A/B testing but
typically only one of the two is on at a time.

**`optimizedNeon.resolutionScale`** (default 0.5)
Fraction of the framebuffer resolution used for the internal FBO. `0.5`
halves each axis (quarter-pixel work); `0.25` quarters each axis (16x
speed-up). Below ~0.35 the bilinear upscale starts showing.

**`optimizedNeon.numSamples`** (default 64)
Number of gather-loop samples per fragment. The full-res `NeonRenderer`
always runs 128 (compile-time fixed to `NEON_MAX_LOOP_SAMPLES`); this
renderer uses the slider value up to that ceiling. Fewer samples ->
faster, but the halo can develop visible "beading" at low counts.

**`optimizedNeon.gradientLutSize`** (default 256)
Width of the precomputed gradient LUT (32 - 256). At the default 256 the
gradient is smooth enough that dithering hides all seams; below ~64 you can
see quantisation on smooth pans.

---

## 4. Common recipes

Quick config sketches for looks that come up a lot. Each starts from the
default `Config()` and only sets the fields listed - everything else stays at
its default.

### 4.1 The default "hello neon"

```cpp
cfg.neon.enable = true;
```

Full-perimeter rainbow, 4 px filament, 5 px halo, hue rotating at 0.5 rev/s.
This is the effect's out-of-box preset.

### 4.2 Solid single-colour ring

```cpp
cfg.neon.enable = true;
cfg.neon.colorStops = { {0.0f, glm::vec4(0.0f, 0.8f, 1.0f, 1.0f)} };
cfg.neon.hueRotationRate = 0.0f;
```

Static cyan neon ring. One stop = solid colour; hue rotation off since
there's nothing to rotate.

### 4.3 Backlit panel

```cpp
cfg.neon.enable = true;
cfg.neon.glowSide = EdgeLighting::GlowSide::INSIDE;
cfg.neon.glowSideSoftness = 2.0f;
cfg.neon.glowRadius = 40.0f;
cfg.neon.bloomStrength = 0.6f;
```

Interior fills with a soft glow; exterior stays clean. Reads like a
back-illuminated sign.

### 4.4 Wide bloom, atmospheric

```cpp
cfg.neon.enable = true;
cfg.neon.glowRadius = 40.0f;
cfg.neon.bloomStrength = 1.2f;
cfg.neon.intensity = 0.9f;
```

Long spill, gentle master intensity - the neon reads as a diffuse ambient
light rather than a hard line. Pair with a dark background.

### 4.5 Marquee chase

```cpp
cfg.neon.enable = true;
cfg.neon.arcs = {
    { 0.00f, 0.25f, 1.0f },
    { 0.50f, 0.25f, 1.0f },
};
cfg.neon.hueRotationRate = 1.5f;
```

Two lit quarters on opposite sides of the perimeter, with the base gradient
scrolling through both. The other halves are dark. Reads as an alternating
chase pattern.

### 4.6 Segment comet

```cpp
cfg.neon.enable = true;
cfg.neon.segmentBoosts = { { 0.0f, 0.06f, 6.0f } };  // pos will be animated
// then attach a SegmentTravel animation to drive segmentBoosts[0].position:
manager.Attach(std::make_shared<SegmentTravel>(1.5f, 0.06f, 6.0f));
```

A tight bright comet running around the perimeter every 1.5 seconds on top
of the base neon. The additive compose means the comet stays visible even
if the arc intensity is dropped low.

### 4.7 Outline draw-on

```cpp
cfg.neon.enable = true;
cfg.neon.arcs = { { 0.0f, 0.0f, 1.0f } };  // start empty
manager.Attach(std::make_shared<OutlineTracer>(2.0f));
```

`arcs[0].length` animates from `0 -> 1` over 2 s; the rectangle "draws
itself on" from the top-left corner, ends fully lit. Great for reveal moments.

### 4.8 Arc wipe (chase around and off)

```cpp
cfg.neon.enable = true;
manager.Attach(std::make_shared<ArcWipe>(3.0f, /*startPos=*/0.1f,
                                          /*endPos=*/0.1f, /*maxLength=*/0.5f));
```

A 0.5-long arc races around the perimeter from `0.1` back to `0.1` (full
loop), growing / chasing / shrinking through three phases so both ends
move at constant speed. Fire-and-forget one-shot.

---

## 5. Interaction cheatsheet

Which control affects which pixels:

| To change... | ...touch this |
|---|---|
| How wide the bright line is | `neon.lineWidth` |
| How hard vs soft the line reads | `neon.filamentFalloff` |
| How far the coloured glow reaches | `neon.glowRadius` |
| How much soft background wash | `neon.bloomStrength` |
| Overall brightness | `neon.intensity` (arcs), segment `boost` (segments) |
| Which side the glow spills to | `neon.glowSide` (+ `glowSideSoftness`) |
| The colours around the perimeter | `neon.colorStops` (+ `neon.blendSpace`) |
| How fast colours move | `neon.hueRotationRate` |
| Which slices of the perimeter are on | `neon.arcs` |
| Per-slice colour override | `Arc::colorStops` (+ `Arc::blendSpace`) |
| Per-slice intensity | `Arc::intensity` |
| A moving highlight on top | `neon.segmentBoosts` (+ `SegmentTravel` / `SegmentBounce`) |
| Cross-fade when swapping palettes | `neon.colorTransitionDuration` |

If you need to *animate* one of these fields rather than set it, the
`FieldBoundAnimation` + `Modulator` family lets you plug an oscillator, an
ease, or a sequence into any of the animatable fields listed in
[`field-bound-animation.h`](../lib/include/animation/field-bound-animation.h).
See [`architecture-design.md`](architecture-design.md) for the animation
model.
