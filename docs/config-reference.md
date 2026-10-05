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

## Reading the Range column

Every numeric field carries a **Range**: the span over which the value
actually changes what is drawn. Outside it the renderer draws the same
pixels, so a slider there is dead travel. Three notations:

| Notation | Meaning |
|---|---|
| `[a, b]` | Hard clamp. The library rewrites anything outside; `b` and `10b` render byte-identically |
| `>= a` | Floored at `a`. Below it (including negatives) renders exactly as `a` |
| bold warning | Overrides the notation beside it. The value is neither clamped nor safe - see [Out-of-range values that are NOT clamped](#out-of-range-values-that-are-not-clamped) |
| `no cap` | Keeps changing as far as was measured. Pick by look, not by limit |

A clamp is applied at draw time - `Config` keeps whatever was written and
`GetConfig` reads it back unchanged, so a clamped field does NOT round-trip
to the clamped value.

Every bound in the Range columns was verified by rendering: one offscreen
900x640 frame per value, compared byte-for-byte. Section
[Measured limits](#measured-limits) has the evidence, the values that have no
ceiling at all, and the four fields where an out-of-range value is not
clamped but breaks the output instead.

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

Per-side hard geometric limit. Two layers take a pair each: the neon glow
(`NeonConfig.insideCutoff / outsideCutoff`) and the opaque fill
(`NeonConfig.opaqueInsideCutoff / opaqueOutsideCutoff`). Where a field reads
differently for the two, the row says so.

| Field | Unit | Range | Meaning |
|---|---|---|---|
| `enable` | bool | - | `true` = the layer ends at `size`, faded out over the next `softness` px. `false` = uncapped: for the glow, natural halo/bloom decay bounds it; for the fill nothing does - an `INSIDE` fill covers the whole rect, an `OUTSIDE` fill runs to the viewport edge, and `BOTH` with both off covers the viewport |
| `size` | px, positive | `>= 0`; glow: no-op past the glow's own reach; fill: outside never a no-op, inside a no-op past `min(width, height) / 2` | Distance from rect edge to where the feather STARTS along this side; the layer is untouched up to here whenever `softness` is at or above the 1 px floor (below it, see `softness`). For the glow, once `size` exceeds `glowRadius * 48 * (1 + bloomStrength * intensity)` px (the quad margin, `GLOW_REACH_RADIUS_FACTOR`) there is no emission left to cut - 312 px at the stock glow. The fill has no reach of its own, so a larger outside `size` always grows it; an inside `size` stops mattering once it reaches `min(width, height) / 2`, where the fill already covers the whole interior |
| `softness` | px, running outward from `size` | `>= 0`; width floored at 1 px | Feather width, same meaning for both layers: full strength at `size`, 50% at `size + softness/2`, gone at `size + softness`. At or above the floor it never eats into the first `size` px; below it the floored ramp is laid symmetrically about `size + softness/2` and so starts up to half a floor inside `size` - 0.5 px at softness 0 on a straight edge, ~0.7 px on a diagonal. All of it holds at every `resolutionScale`: the glow's floor is 1 destination px on both paths, and below `1.0` the edge is still drawn at destination resolution (by the blit, or by the full-res edge ring near the line), never into the reduced buffer - the pixels either side of `size` read the same at 0.5 and 0.25 as at 1.0 (20 px cutoff, either side, softness 0 / 4 / 16). A reduced scale changes the glow inside the band, not its edges; it used to blur them (56-63% just inside `size` at 0.25, softness 0) until `neon-resolution-scale-plan.md` step 2. The WIDTH is floored at 1 destination px so `0` is a pixel-tight AA edge at `size`, but the POSITION is not floored: every softness moves the edge out by `softness/2`, so small values are not interchangeable (glow, 420x300 rect, cutoff 12: `0` vs `0.1` differ by up to 8/255, `0.5` vs `1.0` by 39/255, while `1.0` vs `1.01` differ by 1/255). |

Coverage applied to the graded output (not multiplied into linear emission
ahead of the tone map). A cutoff on the side `glowSide` already culls is
ignored (subsumed): `OUTSIDE` subsumes `insideCutoff`, `INSIDE` subsumes
`outsideCutoff`. The glow's pair never touches the opaque fill: the fill has
its own pair, `NeonConfig.opaqueInsideCutoff / opaqueOutsideCutoff`, which
reads `softness` the same way and to which the subsumption rule does not
apply (the fill has no glow-side notion).

### OpaqueMode

Where the opaque fill covers. All modes fill `NeonConfig.opaqueColor`;
neon emission composites on top inside the glow band. Cutoff distances come
from the fill's own `NeonConfig.opaqueInsideCutoff / opaqueOutsideCutoff`,
not from the glow's `insideCutoff / outsideCutoff`.

* `NONE` (default): no fill, transparent composite.
* `OUTSIDE`: `0 <= d <= opaqueOutsideCutoff`.
* `INSIDE`: `-opaqueInsideCutoff <= d <= 0`.
* `BOTH`: `-opaqueInsideCutoff <= d <= +opaqueOutsideCutoff`. With both
  disabled (their default) this covers the whole viewport.
* `ALL`: whole viewport (old `opaque = true + glowSide = BOTH` behaviour).

Here `d` is signed SDF distance to the edge in px (negative inside), each
cutoff stands for its `size`, and each range is where the fill is SOLID: a
non-zero `softness` adds a fade of that many px beyond it, never inside it.

### BlendSpace

Interpolation space between color stops.

* `RGB` (default): linear per-channel. Complementary pairs pass through grey.
* `HSV`: preserves saturation across the blend.
* `HSL`: smoother mid-tones through neutral gray.

`.rgb` only; stop alpha always interpolates linearly.

### ClipMode / ClipArea

Shared rounded-rect mask shape. Today only `SpotlightConfig.clipArea` holds
one, but nothing about the shape is spotlight-specific.

| Field | Unit | Range | Meaning |
|---|---|---|---|
| `position` | px, app coords | no cap | Top-left corner of the area |
| `width, height` | px | `>= 0` | Area size. `0 x 0 + KEEP_INSIDE` keeps nothing (deliberate, not "unset") |
| `cornerRadius` | px | `[0, min(w,h)/2]` | Clamped to half the shorter side - on a 500x400 area, `200`, `201` and `900` are identical. `0` = sharp |
| `edgeSoftness` | px | `>= 0`, sub-pixel invisible | Fade across the cut. `0` = hard edge, `1.0` (default) = 1 px AA. `0` through `1.1` stay within 1/255 of each other |
| `mode` | `KEEP_INSIDE / KEEP_OUTSIDE` | - | Which side survives |

No enable flag. The consumer opt-in (`SpotLight.clipped`) is the only gate.
Expected as coverage on shaded output, not folded into shading.

### ColorStop

| Field | Unit | Range | Meaning |
|---|---|---|---|
| `position` | `[0, 1)` | wraps | Perimeter fraction (base ring) or head-to-tail span position (arc/segment row) |
| `color` | linear `vec4` | `.a` is `[0, 1]` | `.rgb` color, `.a` emission scale at this stop. Alpha saturates at `1` - `1.5` and `4.0` render identically to `1.0`, so it cannot overdrive |

`a = 1` full brightness, `a = 0` dark with background showing through
(attenuates filament + halo + bloom together). Interpolates linearly in every
`BlendSpace`. Base ring is circular (`SampleRing`, `GL_REPEAT`); arc/segment
rows are head-to-tail (`SampleSpan`, `GL_CLAMP_TO_EDGE`, holds end colors).

### SegmentBoost

Travelling additive light. Composites additively, independent of
`NeonConfig.intensity`, so a segment shines on a dark arc.

| Field | Unit | Range | Meaning |
|---|---|---|---|
| `position` | `[0, 1)` | wraps | Centre on perimeter |
| `length` | perimeter fraction | `>= 0`, no cap | Visible span (~2 sigma). Default `0.15`. Keeps widening past `1.0` (sigma is `length/2`, it is not a wrap) |
| `boost` | absolute brightness | `>= 0`, no cap | Peak amplitude added on top of base arc. Default `0.0`. Negatives render as `0`; unclamped upward, so overlapping segments sum past `1` |
| `colorStops` | head-to-tail stops | - | Across the segment span. Empty = inherit base gradient at touched samples |
| `blendSpace` | enum | - | For `colorStops`. Ignored when empty |

### PreservedSegment

`{ id: uint32, segment: SegmentBoost }`. Id-addressed pool
(`NeonConfig.preservedSegmentBoosts`) that survives transient-pool bulk
overrides (clear/resize/rebuild of `segmentBoosts`). Ids from
`SegmentUtils::AcquireSegment`, stable for the entry lifetime, `0` = invalid,
reusable after release. Same renderer effect as `SegmentBoost`.

### Arc

Lit slice of the perimeter. Overlap resolves winner-take-all
(largest `mask * intensity` owns the sample).

| Field | Unit | Range | Meaning |
|---|---|---|---|
| `start` | `[0, 1)` | wraps | Arc start. Wraps over `0/1` |
| `length` | perimeter fraction | `[0, 1]` | `0` = off, `1` (default) = full ring. Saturates at `1` - `1.5` and `2.0` are identical, they do not wrap a second lap |
| `intensity` | multiplier | `>= 0`, no cap | Per-arc brightness, independent of `NeonConfig.intensity`. Negatives render as `0` (the slice goes dark) |
| `colorStops` | head-to-tail stops | - | Across the arc span. Empty = inherit base gradient |
| `blendSpace` | enum | - | For `colorStops`. Ignored when empty |

Examples: `{0, 1, 1}` full ring (default `arcs = {Arc{}}`), `{0, 0.5, 1}`
first half, `{0.8, 0.4, 1}` wraps `0.8 -> 0.2`.

## RectGeometry

Geometry of the target rectangle. Read by every renderer.

| Field | Unit | Default | Range | Renderer effect |
|---|---|---|---|---|
| `width, height` | px | `800 x 600` | `> 0`, no cap | Rect size. Sets perimeter length for arc/segment/stop walks |
| `position` | px, app coords | `(0, 0)` | no cap | Top-left corner |
| `cornerRadius` | px | `40` | `[0, min(w,h)/2]` | Corner rounding. `0` = sharp. Clamped internally at every consumer; `Config` value itself is never rewritten. On a 420x300 rect, `150`, `151` and `400` render identically |
| `winding` | enum | `COUNTER_CLOCKWISE` | - | Perimeter traversal direction |

## NeonConfig -> NeonRenderer

Filament (bright line) + halo (sharp colored glow) + bloom (wide spill),
summed in HDR and tone-mapped together. Also owns the opaque fill pass
(`black-rect.frag`).

| Field | Unit | Default | Range | Effect |
|---|---|---|---|---|
| `enable` | bool | `false` | - | Master switch. Nothing draws when false |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | `[0.001, 1.0]`, useful `0.25-1.0` | `1.0` = direct to caller framebuffer. Below = the gather runs alone into a small buffer at its own coarse scale (about 2 texels per colour kernel, `GetGatherScale`), the glow is shaded from it into an offscreen buffer at that fraction, then bilinear-blitted (`neon-blit.frag`) everywhere except a thin ring around the edge, which is re-shaded at full res from the same gather result (`neon.frag` built with `NEON_READS_GATHER`). Buys the gather loop (~95% of the cost) almost entirely, and the rest of the shading on (1 - scale^2) of the glow. Clamped at draw time; `>= 1.0` all identical, `<= 0.001` all identical (a 1 px buffer). Within 2/255 of `1.0` down to `0.125` on the comparison page's scenes, except a 160 x 96 rect (4 at `0.25`, 10 at `0.125`) - the error grows as the rect shrinks (20 x 17: 18 / 53 / 93 at `0.5` / `0.25` / `0.125`), so keep small rects at `1.0`; before the edge ring the defaults sat 7/255 away at `0.5` and 25 at `0.25`. NOT a guaranteed saving: the ring and the blit are a fixed cost, so a tight cutoff band rendered ~2x SLOWER below `1.0` on an AMD 5300M (before the ring and blit were bounded to the lit band, which took 44% off that scene at `0.25` on llvmpipe), and a soft filament (`filamentFalloff` < ~0.3) widens the ring. Memory below `1.0`: an RGBA8 buffer over what the blit reads, never more than the whole viewport at that fraction, plus an RGBA16F gather buffer (two attachments with segments) over the glow - at 1920 x 1080 and `0.5`, 2.15 MB for a full-screen rect, 1.8 MB for a 900 x 540 one, against 4.1 MB before `neon-resolution-scale-plan.md` section 13. See `neon-resolution-scale-comparison.html` |
| `decoupledGather` | bool | `false` | - | At `resolutionScale` `1.0` only: the gather runs alone into the gather buffer at `GetGatherScale` (as below `1.0`) and the glow quad is shaded at full res from it by the ring's program, straight onto the caller's framebuffer - no reduced buffer, no blit, no ring. No effect below `1.0`. Off by default because it gives up `1.0`'s exactness: max 1/255 (p99 1) against the exact render on the comparison page's scenes, 2 on a few dozen pixels of a partly lit ring with cutoffs. 3.6x faster over those scenes (2.0-5.4x each), 4.6 -> 1.1 ms for a full-screen rect at 1080p, Apple M2 Pro. Gather buffer 0.08 MB at that size (0.16 with segments), up to ~1 MB for a 160 x 96 rect. A rect so small its gather grid would be the pixel grid (`GetGatherScale(config, 1.0) >= 1`) takes the exact path regardless (`DecouplesAtFullScale`): 24 x 18 gained 1.09x for a 3.4 MB buffer. `neon-scale-check check` bounds it (`dg1000`, max 2) |
| `numSamples` | int | `128` (`NEON_MAX_LOOP_SAMPLES`) | `[1, 128]`, converged `>= 96` | Gather samples per fragment. Lower = faster + grainier halo. `128`, `129` and `512` are identical; so are `0` and `1`. Against `128`: `96` is 1/255, `64` is 3/255, `32` is 12/255, `16` is 29/255 |
| `gradientLutSize` | texels | `256` | `4-4096`; converged `>= 128` | Baked ring LUT width. Size change snaps, never cross-fades. Clamped when baked: floored at 4 (`1` through `4` identical, `5` is the first that differs) and capped at 4096, or at the driver's `GL_MAX_TEXTURE_SIZE` where lower, with one warning per oversized value; the stored value round-trips unchanged. The demo's `32-256` slider is a UI choice, and `512`/`4096` do render (3/255 and 5/255 past `256`). Before the cap a width past the texture limit drew from an incomplete texture, and the CPU ring cost 52 bytes a texel (4M texels: +208 MB, 18 ms per fade frame) |
| `opaqueMode` | enum | `NONE` | - | Fill geometry (see `OpaqueMode`) |
| `opaqueColor` | linear `vec4` | `(0,0,0,1)` | `.rgb` `[0, 1]` | Fill color. `.rgb` used today, `.a` reserved |
| `opaqueInsideCutoff` | `Cutoff` | `{false, 0, 0}` | see `Cutoff` | The FILL's interior cap: `INSIDE/BOTH` fills are solid down to `d = -size` and gone at `d = -(size + softness)`. Independent of the glow's `insideCutoff`. `enable = false` lets an `INSIDE` fill cover the whole rect. `softness` is the fill feather on this side, width floored at 1 destination px. It feathers the fill's **coverage alpha** as well as its colour, so with `opaqueColor` equal to the backdrop it is invisible on an opaque window and still the visible edge over a transparent surface |
| `opaqueOutsideCutoff` | `Cutoff` | `{false, 0, 0}` | see `Cutoff` | The FILL's exterior cap: `OUTSIDE/BOTH` fills are solid out to `d = +size` and gone at `d = +(size + softness)`. Independent of the glow's `outsideCutoff`. `enable = false` lets an `OUTSIDE` fill run to the viewport edge. `softness` as on `opaqueInsideCutoff`, set per side |
| `lineWidth` | px | `4.0` | `>= 0`, no cap | Filament width. Peak always `1.0`, only width changes. `0` = no line (negatives too); past `min(w,h)` the line has swallowed the rect and only its outer edge still moves |
| `filamentFalloff` | shape `N = 2*falloff` | `1.0` | `>= 0.001`, no cap | `0.5` Laplace / `1.0` Gaussian / `2.0` flat-top / `>3` near-rectangular. Sides only, peak fixed. Floored at `1e-3` (`0`, `0.0005`, `0.001` identical); above ~4 the profile is already rectangular and only its edge keeps sharpening |
| `intensity` | multiplier | `1.0` | `0` and up, no cap. **Negatives break the output** | Master arc emission multiplier (filament + halo + bloom). Segments bypass it. Never saturates - the tone map keeps moving the outer fringe at `4096`. Core white-out starts near `8` |
| `glowRadius` | px | `5.0` | `>= 0`, no cap | Halo reach. Also seeds bloom and corner cross-fade widths. `0` = filament only (halo/bloom gated by `GLOW_GATE_FADE_PX`; negatives read as `0`). Sizes the draw quad as `glowRadius * 48 * (1 + bloomStrength * intensity)` px, so it also sets the distance past which a cutoff is a no-op |
| `bloomStrength` | unitless | `0.30` | `0` and up, no cap. **Negatives break the output** | Wide spill on top of halo. `0` = halo only, `1+` = strong wash. Still changing at `1024` |
| `glowSide` | enum | `BOTH` | - | Which side emits |
| `glowSideSoftness` | px total, into lit side | `0.0` | `>= 1` | One-sided cut feather. Floored at 1 destination px: `0`, `0.5` and `1.0` are identical, `1.05` is the first that differs. Ignored when `BOTH`. Coverage on graded output |
| `insideCutoff` | `Cutoff` | `{false, 0, 0}` | see `Cutoff` | Glow interior cap. Does not shape the fill (see `opaqueInsideCutoff`). A complete no-op under `glowSide = OUTSIDE` - byte-identical at every size |
| `outsideCutoff` | `Cutoff` | `{false, 0, 0}` | see `Cutoff` | Glow exterior cap. Also sizes the draw quad (far-exterior rasteriser cull). Does not shape the fill (see `opaqueOutsideCutoff`). No-op under `glowSide = INSIDE` |
| `blendSpace` | enum | `RGB` | - | Base-ring interpolation space |
| `colorStops` | ring stops | R/G/B/Y at `0/.25/.5/.75` | `>= 1` stop, no cap | Base gradient. 1 = solid, 2 = gradient, 3+ = multi-stop circular. No cap measured - 64 stops still differ from 32, bounded in practice by `gradientLutSize` texels |
| `hueRotationRate` | rev/sec | `0.5` | no cap; sign = direction | Gradient scroll around perimeter. `0` = static (and drops `uTime` out of the emission pre-pass entirely). `+` with winding |
| `segmentBoosts` | transient pool | empty | first 8 entries | Index-addressed hotspots, freely overwritten |
| `preservedSegmentBoosts` | id pool | empty | shares the same 8 | Id-addressed hotspots, override-proof. Merged with transient via `FillEffectiveSegments`, share `MAX_SEGMENT_BOOSTS = 8` slots |
| `arcs` | slices | `{Arc{}}` full ring | first 8 entries | Gating. Cap `MAX_ARCS = 8` - entries 9+ are dropped byte-for-byte, with one logged warning |
| `colorTransitionDuration` | sec | `0.3` | `>= 0` | Whole-LUT cross-fade on stop/blend change. `0` = snap. Works across count/position changes |

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
emitGlow = arcCol * emitCoverGathered + segColHue  * gatheredSeg
glow     = emitGlow * (halo, bloom) + per-piece correction       // V19
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
  `gatheredSeg` for segments. A shared gate let a segment lift the arc term
  on stretches no arc covers (blue half-arc + red segment rendered magenta at
  ~2x); separate gates fix it while leaving the lit-arc tracer case identical.
* **Segment + segment: sum and stack.** Two `boost = 2` comets crossing read
  `4` at the crossing, unclamped through the tone map.
* **Gates:** `filamentGate = max(smoothstep(0.5, 1, min(segCoverPt, 1)),
  emitCover)` is sharp and pointwise (lives on the line);
  `gatheredSeg = segCoverGathered * max(emitCoverGathered, min(segCoverGathered, 1))`
  is soft and gathered (integrals over the emitter). A faint segment
  (`cover 0.5`) opens no filament gate yet still feeds `0.25` into glow: halo
  without a core.
* **Each piece's own coverage (V19, V20):** the halo and bloom are a sum over
  the outline's eight pieces, and each piece's light is scaled by how lit
  THAT piece is near its foot, under its own kernel - read from a table the
  renderer bakes per config change - rather than by the coverage gathered
  around the pixel. It is what keeps a stretch no arc covers from showing a
  thin line along it, even at a high `intensity`, and keeps lit edges' bloom
  from dimming where it reaches a dark one; 0 on a fully lit ring. Arcs and
  segments alike. Each piece's coverage stops at its own ends and follows its
  own corner (V21), so no light spills round a corner from the next piece.
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

| Field | Unit | Default | Range | Effect |
|---|---|---|---|---|
| `enable` | bool | `false` | - | Master switch |
| `amount` | `[0, 1]` | `0.7` | `[0, 1]` | Density. Low = static condensation, high = +2 trickling layers with trails. Saturates at `1` - `1.2`, `2.0`, `4.0` identical |
| `speed` | multiplier | `1.0` | no cap; time only. **No effect on a still frame** | Trickle pace. `0` freezes. Purely a time multiplier: at a fixed clock every value renders the same pixels, so it changes the animation rate and nothing else |
| `lanes` | int `>= 1` | `1` | `>= 1`, no cap | Lanes across band. Cell width = `bandWidth / lanes` px. Floored at 1 (`-4`, `0`, `1` identical). No convergence upward - each count re-tiles to a different drop layout, so `128` is as different from `96` as `2` is from `1` |
| `bandWidth` | px | `24.0` | `>= 1`, no cap | Band thickness and drop-size scale. Floored at 1 px (`0`, `0.5`, `1.0` identical). Side from `neon.glowSide` (`OUTSIDE` outward, `INSIDE` inward, `BOTH` straddled) |
| `bandOffset` | px | `0.0` | no cap, both signs | Gap between rect edge and band inner boundary |
| `tint` | linear `vec4` | `(0.85,0.90,1.0,1.0)` | `.rgb` `>= 0` | Body tint multiplier (`.rgb` used, `.a` reserved). Rim/spec stay white |

`droplets-tuning.h:DROPLET_BAND_GUARD = 0.25` band widths: shader/quad must
agree or the band clips to a straight line.

## LensFlareConfig -> LensFlareRenderer

Sun core + rays + hex-aperture chromatic ghosts, one fullscreen
premultiplied pass. Sun rides the perimeter in segment/arc parameter space.
One renderer, two resolution paths like neon (only `uResolution`/`uSunPos`
differ; shader is scale-invariant, low-frequency light).

| Field | Unit | Default | Range | Effect |
|---|---|---|---|---|
| `enable` | bool | `false` | - | Master switch |
| `perimeterPosition` | `[0, 1)` | `0.0` | wraps | Sun pos along perimeter. `0` = top-left, follows `winding`. Via `GetPointOnRectangle` |
| `perimeterOffset` | px signed | `0.0` | `>= -min(w,h)/2`, no outward cap | Distance from edge. `+` outward, `-` inward. Rides constant-distance offset curve at constant arc-length speed. Below `-cornerRadius` clamps corners square; at `-min(w,h)/2` the inward curve has collapsed to the rect's centre and everything further in is identical (measured `-150` = `-400` on a 420x300 rect) |
| `size` | scale | `1.0` | `>= 0`, no cap | Sun disc radius. Brightness separate (`intensity`; softening floor keeps peak finite). Ray extent (global envelope) and ghosts unaffected. Negatives render as `0` |
| `color` | linear `vec4` | `(1,0.92,0.75,1)` | `.rgb` `>= 0` | Sun + ray tint. Ghosts procedural |
| `intensity` | multiplier | `1.0` | `>= 0`, no cap | Master brightness. Never saturates (`32` still differs from `16` by half the range) |
| `spread` | strength | `1.0` | `0` and up, no cap. **Negatives break the output** | Ghost/hex strength. `0` = suppress (and skips the loop) |
| `ghostSpacing` | scale | `1.0` | `>= 0`, no cap | Placement stretch along sun-to-centre axis. `>1` declumps edge-near suns. Past ~6 the ghosts are leaving the frame and each step moves less |
| `ghostSize` | exponent/size | `2.2` | `> 0`, no cap | Shared ghost size/falloff (ref mean). Larger = bigger/softer. Past ~12 the ghosts overlap into one wash that keeps brightening |
| `ghostOffset` | axis units | `-1.5` | no cap; off-frame past ~`+4` | Cluster shift along axis (`0` = centre, `~-1` = on sun). Default toward border. The saturation point is where the cluster leaves the viewport, so it moves with `flareCenter` and frame size - not a clamp |
| `ghostColor` | linear `vec3` | `(1,1,1)` | `>= 0` | Tint target |
| `ghostTint` | `[0, 1]` | `0.0` | **not clamped** - see below | `0` = procedural rainbow, `1` = all `ghostColor`. Hue only. `[0, 1]` is the intended range but nothing enforces it: `-1` and `3.0` both render, differently and wrongly |
| `flareCenter` | normalized y-down | `(0.5,0.5)` | no cap | Ghost pivot/axis reference. Sun rays unaffected |
| `rayDensity` | `[0, 1]` | `0.25` | `[0, 1]`, 81 slots | Ray angular density, quantised to `round(density * 80)` slots, floored at 1. `0` = 1 broad ray. Saturates at `1` (`1.5`, `3`, `10` identical). Per-slot random length, so slot count != visible spike count, and a `0.0125` step is one slot |
| `rotationRate` | rev/sec | `0.0` | no cap; sign = direction | Sun/ray rotation (`+` = CCW screen). Ghosts stay on axis |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | `[0.001, 1.0]`, useful `0.25-1.0` | `1.0` = direct. Below = `scale^2` fragments + blit. Clamped; `>= 1` all identical, `<= 0.001` all identical. Costs more than neon's does: `0.5` sits 14/255 from `1.0` |

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

| Field | Unit | Default | Range | Effect |
|---|---|---|---|---|
| `position` | px, app coords | `(0,0)` | no cap | Lamp place. Rect moves do not move it |
| `angle` | deg, `0` = right, clockwise | `90` | wraps at 360 | Aim. `90` = down, `180` = left, `270` = up |
| `beamAngle` | deg full angle | `26.0` | `[0, 170]` | Gaussian width, not a hard cut. Hard-clamped at 170 deg - `171`, `180` and `360` all render as `170` |
| `throwLength` | px 1/e distance | `215.0` | `>= 1`, no cap | Axial decay, not end point. Floor `SPOT_MIN_THROW = 1.0` (`0`, `0.5`, `1.0` identical) |
| `spreadFalloff` | exponent `[0, 2]` | `1.0` | `[0, 2]` | `pow(aperture/halfW, x)`. `1` physical, `0` searchlight (throw only), `>1` tighter pool. Hard-clamped - `3.0` and `4.0` render as `2.0`. Grows strip; pair with `throwLength` |
| `apertureWidth` | px | `13.0` | `>= 1`, no cap | Width at lamp + core tightness. Floor `SPOT_MIN_APERTURE = 1.0` (`0`, `0.5`, `1.0` identical) |
| `softness` | `[0, 1]` | `0.55` | `[0, 1]` | Cross-section breadth (`0` tightest). Never a hard edge. Clamped both ends - `-1` renders as `0`, `1.5` and `3.0` as `1` |
| `intensity` | multiplier | `1.15` | `>= 0`, no cap | Lamp brightness. Never saturates; `40` still differs from `20` |
| `bloom` | strength | `0.4` | `>= 0`, no cap | Aperture spill at lamp. `0` removes term + cost |
| `bloomRadius` | px | `20.0` | `>= 1`, no cap | Aperture glow size. Floored at 1 px. Support `= radius * 8` (`SPOT_BLOOM_WINDOW_OUTER`); large values fill the frame - this, not a long throw, is the pathological case for strip area |
| `colorTemp` | Kelvin | `5600` | `[1800, 8000]` | Blackbody bake (2700 tungsten, 5600 daylight, 6500+ blue). A table lookup clamped at both ends: `500` and `1700` render as `1800`, `8100` and `10000` render as `8000` |
| `tint` | linear `vec3`, unclamped | `(1,1,1)` | `>= 0`, no cap | Gel multiplier on blackbody. `>1` legal (grows strip), `0` = lamp off (no geometry) |
| `enable` | bool | `true` | - | Skip without removing (stable indices/animations) |
| `clipped` | bool | `false` | - | Opt into `clipArea`. Only gate (area has no enable). Set area size first (`0x0 KEEP_INSIDE` blanks the lamp). Also pins `SpotlightConfig.resolutionScale` to `1.0` while any enabled lamp has it set |

Layer (`SpotlightConfig`):

| Field | Unit | Default | Range | Effect |
|---|---|---|---|---|
| `enable` | bool | `false` | - | Master switch |
| `lamps` | list | empty | first 8 entries | Past `SPOT_MAX_LAMPS = 8` ignored - entries 9+ dropped byte-for-byte, with one logged warning |
| `clipArea` | `ClipArea` | `0x0 KEEP_INSIDE` | see `ClipArea` | Cut region. `KEEP_INSIDE` also narrows strips (buys fragments back); `KEEP_OUTSIDE` unbounded, cannot |
| `resolutionScale` | fraction `(0, 1]` | `1.0` | `[0.125, 1.0]` | Same two-path shape as neon/flare, no uniform differs (scale in viewport transform only). Floor is `0.125`, not neon's `0.001` - `0.001` through `0.125` are identical and `0.126` is the first that differs. Fixed blit floor (~922k frags at 720p) so only large rigs win; default `1.0`. Held at `1.0` while any enabled lamp is `clipped` (value kept, transitions logged) |

Caller owns GL state the pass assumes and never sets: cull off (or
front-face surviving CCW strips), full color+alpha mask, `FUNC_ADD`, no
depth test/write.

## Measured limits

Every Range column above was checked by rendering, not read off the source.
One offscreen 900x640 RGBA8 frame per value, animations off and the clock
pinned at 0, compared byte-for-byte against its neighbour and against the
extremes of the sweep. **Identical** below means max per-channel difference
0 over all 576,000 pixels; **1/255** means the largest single channel moved
by one.

Scene: a 420x300 rect, `cornerRadius` 40, centred, with only the layer under
test enabled. Every boundary claim below was re-rendered in both orders and
re-rendered again, to rule out a value looking settled only because of what
was drawn before it; all were order-independent and repeatable to the byte.

Differences are quoted **in RGB**. That distinction is load-bearing for the
fill: the default `opaqueColor` is black, so against a black backdrop
the fill cutoffs' `softness` moves the coverage **alpha** and nothing else. The first
pass measured it that way and put its floor one step too high. The numbers
below come from a white fill, where the feather is visible in colour.

### Hard clamps

The library rewrites these at draw time. Two values on the same side of the
bound render the same pixels, so the slider past it is dead.

| Field | Clamp | Where it comes from | Evidence |
|---|---|---|---|
| `geometry.cornerRadius` | `[0, min(w,h)/2]` | every consumer | `150` = `151` = `400` on 420x300 |
| `ClipArea.cornerRadius` | `[0, min(w,h)/2]` | `spotlight-renderer.cpp` | `200` = `201` = `900` on a 500x400 area |
| `neon.resolutionScale` | `[0.001, 1.0]` | `MIN_RESOLUTION_SCALE` | `0.0005` = `0.001`; `1.0` = `1.5` = `4.0` |
| `lensFlare.resolutionScale` | `[0.001, 1.0]` | `MIN_FLARE_RESOLUTION_SCALE` | same shape |
| `spotlight.resolutionScale` | `[0.125, 1.0]` | `MIN_RESOLUTION_SCALE` | `0.001` = `0.0625` = `0.125`; `0.126` differs by 7/255 |
| `neon.numSamples` | `[1, 128]` | `NEON_MAX_LOOP_SAMPLES` | `0` = `1`; `128` = `129` = `512` |
| `neon.gradientLutSize` | `4-4096`, and `GL_MAX_TEXTURE_SIZE` | `gradient-ring-lut.h:85` (`clampSize`) | `1` = `2` = `3` = `4`; `5` differs. `512` differs from `256` by 3/255, `4096` from `1024` by 1/255 |
| `neon.filamentFalloff` | `>= 0.001` | `neon-renderer.cpp:887` | `0` = `0.0005` = `0.001` |
| `neon.lineWidth` | `>= 0` | shader gate | `-8` = `-1` = `0` (no filament) |
| `neon.glowRadius` | `>= 0` | shader gate | `-20` = `-1` = `0` |
| `neon.glowSideSoftness` | `>= 1 px` | one destination px (`fwidth` floor, in `neon.frag` or `neon-blit.frag`) | `0` through `1.0` identical; `1.05` differs |
| `ColorStop.color.a` | `[0, 1]` | LUT bake | `1.0` = `1.5` = `4.0` |
| `Arc.length` | `[0, 1]` | perimeter walk | `1.0` = `1.5` = `2.0` |
| `Arc.intensity` | `>= 0` | mask fold | `-4` = `-1` = `0` |
| `SegmentBoost.boost` | `>= 0` | bell amplitude | `-4` = `-1` = `0` |
| `droplets.amount` | `[0, 1]` | layer gates | `1.0` = `1.2` = `4.0` |
| `droplets.lanes` | `>= 1` | `droplets-renderer.cpp:140` | `-4` = `0` = `1` |
| `droplets.bandWidth` | `>= 1 px` | `droplets-renderer.cpp:47` | `0` = `0.5` = `1.0` |
| `lensFlare.rayDensity` | `[0, 1]` | `lens-flare-renderer.cpp:312` | `1.0` = `1.5` = `10.0` |
| `lensFlare.size` | `>= 0` | disc radius | `-2` = `-0.5` = `0` |
| `SpotLight.beamAngle` | `[0, 170]` deg | `spotlight-renderer.cpp:362` | `170` = `171` = `180` = `360` |
| `SpotLight.throwLength` | `>= 1 px` | `SPOT_MIN_THROW` | `0` = `0.5` = `1.0` |
| `SpotLight.apertureWidth` | `>= 1 px` | `SPOT_MIN_APERTURE` | `0` = `0.5` = `1.0` |
| `SpotLight.bloomRadius` | `>= 1 px` | `spotlight-renderer.cpp:375` | `0` = `0.5` = `1.0` |
| `SpotLight.softness` | `[0, 1]` | `spotlight-renderer.cpp:409` | `-1` = `0`; `1.0` = `1.5` = `3.0` |
| `SpotLight.spreadFalloff` | `[0, 2]` | `spotlight-renderer.cpp:371` | `2.0` = `3.0` = `4.0` |
| `SpotLight.colorTemp` | `[1800, 8000]` K | `KELVIN_TABLE` ends | `500` = `1700` = `1800`; `8000` = `8100` = `10000` |
| `arcs` / `segmentBoosts` / `lamps` | first 8 | the three `MAX_*` caps | holding the first 8 fixed, `8` = `9` = `24`, exactly |

### Clamped width, unclamped position

`Cutoff.softness` - the glow's `insideCutoff` / `outsideCutoff` and the fill's
`opaqueInsideCutoff` / `opaqueOutsideCutoff` alike - looks like a `>= 1 px`
clamp and is NOT one, which is why it is not in the table above. Only the
feather's WIDTH is floored at one destination pixel, by the shader's `fwidth`
floor on every path (below `resolutionScale` 1.0 the glow's cutoffs are drawn
by `neon-blit.frag`, at destination resolution). Its POSITION is
not: the fade's midpoint is always `size + softness/2`, and the floored width is
laid symmetrically about it. At or above the floor the fade therefore runs
exactly `size` to `size + softness`; below it, the one-pixel ramp straddles that
midpoint, so a softness-0 edge's 50% point is `size` at every resolution scale,
and every softness moves the edge out by half its value. Values under 1 px are
therefore not a dead zone - each one renders differently.

| Field | Evidence (420x300 rect, cutoff 12) |
|---|---|
| `Cutoff.softness` (glow) | `0` vs `0.1`: up to 8/255; `0.5` vs `1.0`: 39/255; `1.0` vs `1.01`: 1/255 |
| `opaqueInsideCutoff.softness`, `opaqueOutsideCutoff.softness` | `0` vs `0.1`: 17/255; `0.5` vs `1.0`: 89/255; `1.0` vs `1.01`: 2/255 |

### No ceiling at all

These keep changing as far as was swept. There is no "max useful value" to
quote - the HDR sum feeds a tone map that compresses but never closes, so
each doubling still moves the outer fringe even after the core is white.

| Field | Swept to | Still moving by | Where it stops being useful |
|---|---|---|---|
| `neon.intensity` | `4096` | 13/255 from `1024` | Core pins white around `8`; at `128` only 2.4% of the frame is actually saturated, the rest is still fringe |
| `neon.bloomStrength` | `1024` | 16/255 from `256` | Whole frame lit by `2`; past that it is a wash that keeps brightening |
| `neon.glowRadius` | `8000` | 24/255 from `4000` | Frame is fully lit by `60`; above that it is only flattening the gradient |
| `neon.lineWidth` | `2048` | 5/255 from `1024` | Line has swallowed a 420x300 rect by `256`; only the outer edge still moves |
| `lensFlare.size` / `intensity` / `spread` / `ghostSize` | 20-256 | 30-50/255 | No saturation point; pick by look |
| `SpotLight.intensity` / `bloom` / `tint` | 16-64 | 50-100/255 | Same |
| `droplets.lanes` | `128` | full range | Never converges: each count re-tiles to a different drop layout, so `96` -> `128` is as big a change as `1` -> `2` |

### Where reduced resolution actually costs something

`resolutionScale` against the same scene at `1.0`, max per-channel difference:

| Scale | Neon | Lens flare | Spotlight (no clip) |
|---|---|---|---|
| `0.75` | 5/255 | 14/255 | 2/255 |
| `0.5` | 7/255 | 14/255 | 2/255 |
| `0.25` | 28/255 | 20/255 | 4/255 |
| `0.125` | 85/255 | 35/255 | 10/255 |

The spotlight's near-immunity is why its own floor can be `0.125`, and why
the cost table in [`spotlight-renderer.md`](spotlight-renderer.md) - not
image quality - is what should decide that slider. With a lamp `clipped` the
picture reverses completely; see I21 in
[`review-findings.md`](review-findings.md).

### Convergence, not clamping

Two fields have a point past which more costs more and shows nothing.
Against the maximum, max per-channel difference:

| `numSamples` | vs `128` |
|---|---|
| `16` | 29/255 |
| `32` | 12/255 |
| `48` | 6/255 |
| `64` | 3/255 |
| `80` | 2/255 |
| `96` | 1/255 |
| `112` | 1/255 |

| `gradientLutSize` | vs `384` |
|---|---|
| `16` | 74/255 |
| `32` | 38/255 |
| `64` | 18/255 |
| `96` | 12/255 |
| `128` | 8/255 |
| `192` | 4/255 |
| `256` | 3/255 |

So `numSamples` is visually done at ~96 of its 128 ceiling, and
`gradientLutSize` at ~128-256. Both still cost what they cost - see section 8
of [`neon-perf-review.md`](neon-perf-review.md) for what `numSamples` buys
back.

### Bounds that depend on the scene

Not clamps. These are the point where a value pushes its effect out of the
drawn area, so they move with rect size, frame size and the other fields.

* **`Cutoff.size`** is a no-op once it exceeds the glow's own reach, which is
  the same expression that sizes the draw quad:
  `glowRadius * GLOW_REACH_RADIUS_FACTOR * (1 + bloomStrength * intensity)`,
  i.e. `glowRadius * 48 * (...)` px. At the stock glow (radius 5, bloom 0.30,
  intensity 1) that is 312 px, and the sweep confirms it: `400` px and
  "cutoff disabled" render identically.
* **`insideCutoff` under `glowSide = OUTSIDE`** (and `outsideCutoff` under
  `INSIDE`) is a no-op at *every* size - byte-identical from `5` px to
  `200` px to disabled. The subsumption documented under
  [Cutoff](#cutoff) is exact, not approximate.
* **`lensFlare.perimeterOffset`** stops moving inward at `-min(w,h)/2`, where
  the inward offset curve has collapsed to the rect's centre. Measured on
  420x300: `-150`, `-160`, `-200`, `-400` all identical.
* **`lensFlare.ghostOffset`** past ~`+4` pushed the whole cluster off a
  900x640 frame in this scene. That number moves with `flareCenter`, the sun
  position and the frame size.
* **`droplets.speed`** changes nothing in a single frame at all - it is a
  pure time multiplier. Every value from `0` to `8` rendered the same pixels
  on a pinned clock. It sets the animation rate and only that.

### Out-of-range values that are NOT clamped

Four fields take a value outside their documented range and render something
broken rather than clamping to the edge. Guard these host-side.

| Field | Documented | What actually happens |
|---|---|---|
| `neon.intensity` | multiplier | Negative values drive the tone map out of range: at `-1` the frame came back 80% lit and 75% of it pinned to 255 |
| `neon.bloomStrength` | unitless | Same - `-1` and `-2` both produce large pinned-white regions |
| `lensFlare.spread` | strength | Negative values render, differently at each value, with no sensible meaning |
| `lensFlare.ghostTint` | `[0, 1]` | The range is intent only, nothing enforces it. `-1` and `3.0` both render (and `3.0` pinned 16% of the frame) |

Compare `Arc.intensity` and `SegmentBoost.boost`, which sit in the same
brightness family and DO floor at 0 - so the inconsistency is in these four,
not in the ones that clamp.

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
