# Config Reference

Complete field-by-field reference for `lib/include/core/config.h`.
Headers are the source of truth; when this document drifts from them, the headers win.

Related docs:

* `effect-reference.md` - neon visual model and recipes (neon-focused).
* `spotlight-renderer.md` - spotlight layer reference, cost measurements.
* `implementation.md` - orchestrator, per-frame flow, C ABI staging.
* `renderer/*-tuning.h` - shared CPU/GPU constants (caps, array sizes).

## Conventions used in this document

* **Units** are stated per field. `px` means framebuffer pixels at
  `resolutionScale = 1.0`. Fractions in `[0, 1)` are perimeter positions.
* **App coordinates** (rect `position`, lamp `position`, `ClipArea.position`):
  origin at viewport top-left, `+x` right, `+y` DOWN, in px.
* **Normalized screen coords** (`LensFlareConfig.flareCenter`): `[0, 1]`,
  origin top-left, y-down.
* **Color**: linear RGB(A) in `[0, 1]`. `ColorStop.color.a` is an emission
  scale, not a blend opacity (see below).
* **Change detection**: `Config` and every sub-struct define `operator==`.
  `EdgeLightingEffect::SetConfig` and `refreshActiveConfig` skip
  `OnConfigChanged` when the composited config is equal. Adding a field
  without adding it to `operator==` silently breaks rebuilds.
* **Caps**: `MAX_SEGMENT_BOOSTS = 8`, `MAX_ARCS = 8` (`neon-tuning.h`),
  `SPOT_MAX_LAMPS = 8` (`spotlight-tuning.h`). Entries past the cap are
  ignored at draw time (neon logs a one-shot overflow warning).

## Shared types

### Winding

Traversal direction for the `[0, 1)` perimeter parameter.

* `COUNTER_CLOCKWISE` (default): `s = 0` at top-left, then
  left -> bottom -> right -> top.
* `CLOCKWISE`: `s = 0` at top-left, then top -> right -> bottom -> left.

Affects `ColorStop.position`, `SegmentBoost.position`, `Arc.start`,
`LensFlareConfig.perimeterPosition`, hue rotation direction.

### GlowSide

Which side of the rect edge the neon glow emits from.

* `BOTH` (default): symmetric glow.
* `INSIDE`: interior only, exterior dark.
* `OUTSIDE`: exterior only, interior dark.

Consumed by `NeonRenderer` (halo/bloom/filament coverage) and by
`DropletsRenderer` (which side the rain band occupies).

### Cutoff

Per-side hard geometric limit for the neon glow.

| Field | Unit | Meaning |
|---|---|---|
| `enable` | bool | `true` = clamp at `size`; `false` = uncapped, natural halo/bloom decay bounds it |
| `size` | px, positive | Distance from rect edge to cutoff boundary along this side |
| `softness` | px, total width centred on boundary | Feather over the boundary. `0` = pixel-tight AA edge (floored at 1 destination px), larger = smooth fade |

Coverage applied to the graded output (not multiplied into linear emission
ahead of the tone map). A cutoff on the side `glowSide` already culls is
ignored (subsumed): `OUTSIDE` subsumes `insideCutoff`, `INSIDE` subsumes
`outsideCutoff`. Still bounds the opaque fill, which has no glow-side notion.

### OpaqueMode

Where the opaque fill covers. All modes fill `NeonConfig.opaqueColor`;
neon emission composites on top inside the glow band. Cutoff distances come
from `NeonConfig.insideCutoff / outsideCutoff`.

* `NONE` (default): no fill, transparent composite.
* `OUTSIDE`: `0 <= d <= outsideCutoff`.
* `INSIDE`: `-insideCutoff <= d <= 0`.
* `BOTH`: `-insideCutoff <= d <= +outsideCutoff`.
* `ALL`: whole viewport (old `opaque = true + glowSide = BOTH` behaviour).

Here `d` is signed SDF distance to the edge in px (negative inside).

### BlendSpace

Interpolation space between color stops.

* `RGB` (default): linear per-channel. Complementary pairs pass through grey.
* `HSV`: preserves saturation across the blend.
* `HSL`: smoother mid-tones through neutral gray.

`.rgb` only; stop alpha always interpolates linearly.

### ClipMode / ClipArea

Shared rounded-rect mask shape. Today only `SpotlightConfig.clipArea` holds
one, but nothing about the shape is spotlight-specific.

| Field | Unit | Meaning |
|---|---|---|
| `position` | px, app coords | Top-left corner of the area |
| `width, height` | px | Area size. `0 x 0 + KEEP_INSIDE` keeps nothing (deliberate, not "unset") |
| `cornerRadius` | px | Clamped to half the shorter side. `0` = sharp |
| `edgeSoftness` | px | Fade across the cut. `0` = hard edge, `1.0` (default) = 1 px AA |
| `mode` | `KEEP_INSIDE / KEEP_OUTSIDE` | Which side survives |

No enable flag. The consumer opt-in (`SpotLight.clipped`) is the only gate.
Expected as coverage on shaded output, not folded into shading.

### ColorStop

| Field | Unit | Meaning |
|---|---|---|
| `position` | `[0, 1)` | Perimeter fraction (base ring) or head-to-tail span position (arc/segment row) |
| `color` | linear `vec4` | `.rgb` color, `.a` emission scale at this stop |

`a = 1` full brightness, `a = 0` dark with background showing through
(attenuates filament + halo + bloom together). Interpolates linearly in every
`BlendSpace`. Base ring is circular (`SampleRing`, `GL_REPEAT`); arc/segment
rows are head-to-tail (`SampleSpan`, `GL_CLAMP_TO_EDGE`, holds end colors).

### SegmentBoost

Travelling additive light. Composites additively, independent of
`NeonConfig.intensity`, so a segment shines on a dark arc.

| Field | Unit | Meaning |
|---|---|---|
| `position` | `[0, 1)` | Centre on perimeter |
| `length` | perimeter fraction | Visible span (~2 sigma). Default `0.15` |
| `boost` | absolute brightness | Peak amplitude added on top of base arc. Default `0.0` |
| `colorStops` | head-to-tail stops | Across the segment span. Empty = inherit base gradient at touched samples |
| `blendSpace` | enum | For `colorStops`. Ignored when empty |

### PreservedSegment

`{ id: uint32, segment: SegmentBoost }`. Id-addressed pool
(`NeonConfig.preservedSegmentBoosts`) that survives transient-pool bulk
overrides (clear/resize/rebuild of `segmentBoosts`). Ids from
`SegmentUtils::AcquireSegment`, stable for the entry lifetime, `0` = invalid,
reusable after release. Same renderer effect as `SegmentBoost`.

### Arc

Lit slice of the perimeter. Overlap resolves winner-take-all
(largest `mask * intensity` owns the sample).

| Field | Unit | Meaning |
|---|---|---|
| `start` | `[0, 1)` | Arc start. Wraps over `0/1` |
| `length` | perimeter fraction | `0` = off, `1` (default) = full ring |
| `intensity` | multiplier | Per-arc brightness, independent of `NeonConfig.intensity` |
| `colorStops` | head-to-tail stops | Across the arc span. Empty = inherit base gradient |
| `blendSpace` | enum | For `colorStops`. Ignored when empty |

Examples: `{0, 1, 1}` full ring (default `arcs = {Arc{}}`), `{0, 0.5, 1}`
first half, `{0.8, 0.4, 1}` wraps `0.8 -> 0.2`.

## RectGeometry

Geometry of the target rectangle. Read by every renderer.

| Field | Unit | Default | Renderer effect |
|---|---|---|---|
| `width, height` | px | `800 x 600` | Rect size. Sets perimeter length for arc/segment/stop walks |
| `position` | px, app coords | `(0, 0)` | Top-left corner |
| `cornerRadius` | px | `40` | Corner rounding. `0` = sharp. Clamped internally to `[0, min(w,h)/2]` at every consumer; `Config` value itself is never rewritten |
| `winding` | enum | `COUNTER_CLOCKWISE` | Perimeter traversal direction |

## NeonConfig -> NeonRenderer

Filament (bright line) + halo (sharp colored glow) + bloom (wide spill),
summed in HDR and tone-mapped together. Also owns the opaque fill pass
(`black-rect.frag`).

| Field | Unit | Default | Effect |
|---|---|---|---|
| `enable` | bool | `false` | Master switch. Nothing draws when false |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | `1.0` = direct to caller framebuffer. Below = offscreen buffer at that fraction + bilinear blit (`neon-blit.frag`). Buys fragment work (glow quad dominates). Clamped at draw time |
| `numSamples` | int | `128` (`NEON_MAX_LOOP_SAMPLES`) | Gather samples per fragment. Capped at 128, clamped `>= 1`. Lower = faster + grainier halo |
| `gradientLutSize` | texels `32-256` | `256` | Baked ring LUT width. Size change snaps, never cross-fades |
| `opaqueMode` | enum | `NONE` | Fill geometry (see `OpaqueMode`) |
| `opaqueColor` | linear `vec4` | `(0,0,0,1)` | Fill color. `.rgb` used today, `.a` reserved |
| `opaqueSoftness` | px total, centred | `0.0` | Fill feather at cutoff boundaries. `0` = 1 px AA edge. Independent of `Cutoff.softness` |
| `lineWidth` | px | `4.0` | Filament width. Peak always `1.0`, only width changes |
| `filamentFalloff` | shape `N = 2*falloff` | `1.0` | `0.5` Laplace / `1.0` Gaussian / `2.0` flat-top / `>3` near-rectangular. Sides only, peak fixed |
| `intensity` | multiplier | `1.0` | Master arc emission multiplier (filament + halo + bloom). Segments bypass it |
| `glowRadius` | px | `5.0` | Halo reach. Also seeds bloom and corner cross-fade widths. `0` = filament only (halo/bloom gated by `GLOW_GATE_FADE_PX`) |
| `bloomStrength` | unitless | `0.30` | Wide spill on top of halo. `0` = halo only, `1+` = strong wash |
| `glowSide` | enum | `BOTH` | Which side emits |
| `glowSideSoftness` | px total, into lit side | `0.0` | One-sided cut feather. `0` = pixel-tight AA at `d = 0`, registered with the fill edge. Ignored when `BOTH`. Coverage on graded output |
| `insideCutoff` | `Cutoff` | `{false, 0, 0}` | Interior cap. Also caps `INSIDE/BOTH` fills |
| `outsideCutoff` | `Cutoff` | `{false, 0, 0}` | Exterior cap. Also caps `OUTSIDE/BOTH` fills and sizes the draw quad (far-exterior rasteriser cull) |
| `blendSpace` | enum | `RGB` | Base-ring interpolation space |
| `colorStops` | ring stops | R/G/B/Y at `0/.25/.5/.75` | Base gradient. 1 = solid, 2 = gradient, 3+ = multi-stop circular |
| `hueRotationRate` | rev/sec | `0.5` | Gradient scroll around perimeter. `0` = static. `+` with winding |
| `segmentBoosts` | transient pool | empty | Index-addressed hotspots, freely overwritten |
| `preservedSegmentBoosts` | id pool | empty | Id-addressed hotspots, override-proof. Merged with transient via `FillEffectiveSegments`, share `MAX_SEGMENT_BOOSTS = 8` slots |
| `arcs` | slices | `{Arc{}}` full ring | Gating. Cap `MAX_ARCS = 8` |
| `colorTransitionDuration` | sec | `0.3` | Whole-LUT cross-fade on stop/blend change. `0` = snap. Works across count/position changes |

### Brightness model: intensity vs arc intensity vs segment boost

Three knobs, three compose rules (`neon.frag:1444-1457`):

| Knob | Scope | Rule | Default | Notes |
|---|---|---|---|---|
| `NeonConfig.intensity` | all arcs | multiply | `1.0` | `arcCol = col * uIntensity`, then `arcCol * emitCover` (filament) and `arcCol * emitCoverGathered` (glow). Scales filament + halo + bloom together. Segments bypass it, so `intensity = 0` kills arcs but leaves segments lit |
| `Arc.intensity` | one slice | multiply + compete | `1.0` | Folded into the mask: `mask = arcInside * arc.z`, winner-take-all picks largest mask, `emitCover = max(emitCover, c * arc.z * aA)`. `0` darkens only that slice. Decides overlap seams |
| `SegmentBoost.boost` | one hotspot | add + stack | `0.0` | `bell = boost * exp(-e*e)`, `segCoverPt += bell * alpha`, `segCol = hue * cover`, `emitFil = arcCol*emitCover + segCol*filamentGate`. Absolute peak on top of base arc, unclamped so `>1` still brightens. Gathered with raw proximity `g`, not arc-gated, so it lights where no arc covers. Overlapping segments sum; overlapping arcs max |

Practical reads: `intensity = 0 + boost = 4` = dark ring with a bright comet.
`arcs[1].intensity = 0.5` = one dim slice. `boost = 0` = entry is a no-op
(why the default is `0.0`; visible values come from the host or from
`SegmentTravel`/`SegmentBounce`, which default to `4.0`).

#### How they interact

They meet at two sums - filament (sharp line) and glow (halo + bloom):

```
arcCol  = col * uIntensity
emitFil  = arcCol * emitCover         + segCol     * filamentGate
emitGlow = arcCol * emitCoverGathered + segColGlow * glowCoverAll
```

`col / segColHue` are gated-normalised pure hues, so all brightness lives in
the four coverage terms:

* **Master vs arc: multiply.** Arc light is
  `col * uIntensity * Arc.intensity * coverage * alpha`, so `master 0.5` with
  `arc 0.5` renders `0.25` on that slice. Either zero kills that slice.
* **Master `= 0`:** both arc terms go zero, `segCol` untouched. Dark ring with
  a fully lit comet. The "solo the segment" switch.
* **One arc `= 0`:** its `mask = 0` never wins `max()`, so a co-covering arc
  takes the sample; with no coverer the stretch goes dark (`bestIdx = -1`,
  `arcW = 0`). Other arcs unaffected. Segments still emit there (raw-`g`
  gather, not arc-gated).
* **Arc + segment overlap: sum, each with its own gate.** `emitCover` /
  `emitCoverGathered` for arcs, `filamentGate` /
  `glowCoverAll` for segments. A shared gate let a segment lift the arc term
  on stretches no arc covers (blue half-arc + red segment rendered magenta at
  ~2x); separate gates fix it while leaving the lit-arc tracer case identical.
* **Segment + segment: sum and stack.** Two `boost = 2` comets crossing read
  `4` at the crossing, unclamped through the tone map.
* **Gates:** `filamentGate = max(smoothstep(0.5, 1, min(segCoverPt, 1)),
  emitCover)` is sharp and pointwise (lives on the line);
  `glowCoverAll = max(emitCoverGathered, min(segCoverGathered, 1))` is soft
  and gathered (integrals over the emitter). A faint segment (`cover 0.5`)
  opens no filament gate yet still feeds `0.25` into glow: halo without a core.
* **Shared shaping after the sum:** `core / halo / bloom` kernels,
  `glowRadius / bloomStrength`, `glowSide / cutoffs`, tone map + gamma apply
  to `emitFil / emitGlow` as a whole, so overdrive saturates jointly.
  `ColorStop.a = 0` kills both paths (`emitCover` via `aA`, `segCoverPt` via
  `sA`).

## DebugConfig -> DebugRenderer (+ NeonRenderer for one flag)

| Field | Unit | Default | Effect |
|---|---|---|---|
| `enable` | bool | `true` | Mute for the three overlays only |
| `showGradientLUT` | bool | `false` | Baked ring as centre strip (`neon-lut-debug.frag`). Own `GradientRingLUT` copy, gated by same predicate as draw so they cannot drift |
| `showColorStops` | bool | `false` | Disc per stop at perimeter pos (`neon-stop-marker.frag`) |
| `showWireframe` | bool | `true` | 1 px `GL_LINE_LOOP` sharp box (`wireframe.frag`). Extent, not rounded outline, so it compares against the neon trace. Draws over glow (layer is last) |
| `wireframeColor` | linear `vec4` | `(0,1,0,1)` | Box color |
| `opaqueOnly` | bool | `false` | Neon debug mode (read by `NeonRenderer`, not the overlay): fill only, skip filament/halo/bloom. Suppresses strip + markers (no glow to annotate), box survives. No-op when `opaqueMode == NONE`. Not in C API. Safe to toggle per-frame (no rebuild) |

Strip + markers annotate the glow: suppressed when neon off or `opaqueOnly`.
Box annotates geometry: survives both. Always full-res.

## DropletsConfig -> DropletsRenderer

Rain-on-glass in a band hugging the perimeter. Screen-space gravity,
self-lit drops (body + rim + specular), no framebuffer capture. Band-fitted
ring draw (4 strips), premultiplied blend. No resolution scale (rims/specs are
single-px features a blit would erase).

| Field | Unit | Default | Effect |
|---|---|---|---|
| `enable` | bool | `false` | Master switch |
| `amount` | `[0, 1]` | `0.7` | Density. Low = static condensation, high = +2 trickling layers with trails |
| `speed` | multiplier | `1.0` | Trickle pace. `0` freezes |
| `lanes` | int `>= 1` | `1` | Lanes across band. Cell width = `bandWidth / lanes` px |
| `bandWidth` | px | `24.0` | Band thickness and drop-size scale. Side from `neon.glowSide` (`OUTSIDE` outward, `INSIDE` inward, `BOTH` straddled) |
| `bandOffset` | px | `0.0` | Gap between rect edge and band inner boundary |
| `tint` | linear `vec4` | `(0.85,0.90,1.0,1.0)` | Body tint multiplier (`.rgb` used, `.a` reserved). Rim/spec stay white |

`droplets-tuning.h:DROPLET_BAND_GUARD = 0.25` band widths: shader/quad must
agree or the band clips to a straight line.

## LensFlareConfig -> LensFlareRenderer

Sun core + rays + hex-aperture chromatic ghosts, one fullscreen
premultiplied pass. Sun rides the perimeter in segment/arc parameter space.
One renderer, two resolution paths like neon (only `uResolution`/`uSunPos`
differ; shader is scale-invariant, low-frequency light).

| Field | Unit | Default | Effect |
|---|---|---|---|
| `enable` | bool | `false` | Master switch |
| `perimeterPosition` | `[0, 1)` | `0.0` | Sun pos along perimeter. `0` = top-left, follows `winding`. Via `GetPointOnRectangle` |
| `perimeterOffset` | px signed | `0.0` | Distance from edge. `+` outward, `-` inward. Rides constant-distance offset curve at constant arc-length speed. Below `-cornerRadius` clamps corners square |
| `size` | scale | `1.0` | Sun disc radius. Brightness separate (`intensity`; softening floor keeps peak finite). Ray extent (global envelope) and ghosts unaffected |
| `color` | linear `vec4` | `(1,0.92,0.75,1)` | Sun + ray tint. Ghosts procedural |
| `intensity` | multiplier | `1.0` | Master brightness |
| `spread` | strength | `1.0` | Ghost/hex strength. `0` = suppress |
| `ghostSpacing` | scale | `1.0` | Placement stretch along sun-to-centre axis. `>1` declumps edge-near suns |
| `ghostSize` | exponent/size | `2.2` | Shared ghost size/falloff (ref mean). Larger = bigger/softer |
| `ghostOffset` | axis units | `-1.5` | Cluster shift along axis (`0` = centre, `~-1` = on sun). Default toward border |
| `ghostColor` | linear `vec3` | `(1,1,1)` | Tint target |
| `ghostTint` | `[0, 1]` | `0.0` | `0` = procedural rainbow, `1` = all `ghostColor`. Hue only |
| `flareCenter` | normalized y-down | `(0.5,0.5)` | Ghost pivot/axis reference. Sun rays unaffected |
| `rayDensity` | `[0, 1]` | `0.25` | Ray angular density, quantised to slots. `0` = 1 broad ray. Per-slot random length, so slot count != visible spike count |
| `rotationRate` | rev/sec | `0.0` | Sun/ray rotation (`+` = CCW screen). Ghosts stay on axis |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | `1.0` = direct. Below = `scale^2` fragments + blit. Clamped; `>1` refused |

Ghost distance/color baked on CPU into std140 `GhostBlock`
(`BakeGhostTable`, gated on `ghostOffset/ghostColor/ghostTint` only).
Bloom/ring terms gated on `GetGhostBloomRadius / GetGhostRingFloor`
(`lens-flare-tuning.h`); hex sprite not gated (`fwidth` in non-uniform flow
is undefined).

## SpotLight + SpotlightConfig -> SpotlightRenderer

Freely placed cones of light. Not a perimeter effect: reads only
`Config::spotlight`. Light only (no backdrop/fixtures/occlusion), one
additive pass (`GL_ONE, GL_ONE` color + alpha, coverage = max channel).
Per-lamp solved strips (`SPOT_STRIP_SEGMENTS = 12` quads) in one VBO, one
`glDrawArrays`. VBO holds app coords verbatim; y-flip in ortho.

Per lamp (`SpotLight`):

| Field | Unit | Default | Effect |
|---|---|---|---|
| `position` | px, app coords | `(0,0)` | Lamp place. Rect moves do not move it |
| `angle` | deg, `0` = right, clockwise | `90` | Aim. `90` = down, `180` = left, `270` = up |
| `beamAngle` | deg full angle | `26.0` | Gaussian width, not a hard cut |
| `throwLength` | px 1/e distance | `215.0` | Axial decay, not end point. Floor `1.0` |
| `spreadFalloff` | exponent `[0, 2]` | `1.0` | `pow(aperture/halfW, x)`. `1` physical, `0` searchlight (throw only), `>1` tighter pool. Grows strip; pair with `throwLength` |
| `apertureWidth` | px | `13.0` | Width at lamp + core tightness. Floor `1.0` |
| `softness` | `[0, 1]` | `0.55` | Cross-section breadth (`0` tightest). Never a hard edge |
| `intensity` | multiplier | `1.15` | Lamp brightness |
| `bloom` | strength | `0.4` | Aperture spill at lamp. `0` removes term + cost |
| `bloomRadius` | px | `20.0` | Aperture glow size. Support `= radius * 8` (`SPOT_BLOOM_WINDOW_OUTER`); large values fill the frame |
| `colorTemp` | Kelvin | `5600` | Blackbody bake (2700 tungsten, 5600 daylight, 6500+ blue) |
| `tint` | linear `vec3`, unclamped | `(1,1,1)` | Gel multiplier on blackbody. `>1` legal (grows strip), `0` = lamp off (no geometry) |
| `enable` | bool | `true` | Skip without removing (stable indices/animations) |
| `clipped` | bool | `false` | Opt into `clipArea`. Only gate (area has no enable). Set area size first (`0x0 KEEP_INSIDE` blanks the lamp) |

Layer (`SpotlightConfig`):

| Field | Unit | Default | Effect |
|---|---|---|---|
| `enable` | bool | `false` | Master switch |
| `lamps` | list | empty | Past `SPOT_MAX_LAMPS = 8` ignored |
| `clipArea` | `ClipArea` | `0x0 KEEP_INSIDE` | Cut region. `KEEP_INSIDE` also narrows strips (buys fragments back); `KEEP_OUTSIDE` unbounded, cannot |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | Same two-path shape as neon/flare, no uniform differs (scale in viewport transform only). Fixed blit floor (~922k frags at 720p) so only large rigs win; default `1.0`. Held at `1.0` while any enabled lamp is `clipped` (value kept, transitions logged) |

Caller owns GL state the pass assumes and never sets: cull off (or
front-face surviving CCW strips), full color+alpha mask, `FUNC_ADD`, no
depth test/write.

## Top-level Config

```cpp
typedef struct Config {
    RectGeometry geometry;
    NeonConfig neon;
    DebugConfig debug;
    DropletsConfig droplets;
    LensFlareConfig lensFlare;
    SpotlightConfig spotlight;
} Config;
```

Renderers composite by blending in registration order (demo default:
neon, droplets, flare, spotlight, debug last so annotations sit on top).
Core also supports `AddRenderer / GetLayerOrder / SetLayerOrder` by
`RendererLayer`. Enable any subset.
