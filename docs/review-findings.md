# Review findings

Open defects and rough edges found in a full read of the tree at
`improve_perf_by_emission_prepass` (`bbdba62`), with the visual ones
reproduced offscreen rather than argued from the source.

Nothing here is fixed yet. This document is the backlog; when an item lands,
strike it and note the commit.

## How the visual items were reproduced

A throwaway harness linked against `build/lib/libedge-lighting.a`: a hidden
GLFW window, an `OffscreenCapture` at 1000x800, one `SetConfig` / `Update` /
`Render` per case, `CaptureUtil::WritePNG` out. Every case starts from the
same base config:

```
geometry: 600 x 400 at (200, 200), cornerRadius 40, winding CCW (the default)
neon.enable = true, everything else at its Config default
neon.colorTransitionDuration = 0   // no cross-fade, so one frame settles
neon.hueRotationRate = 0           // except where the case is about rotation
```

Two notes for anyone re-running this. `hueRotationRate` defaults to `0.5`, so
leaving it on makes every capture time-dependent and A/B comparisons drift by a
few LSB per frame. And `colorTransitionDuration` defaults above zero, so the
first frame after a colour-stop change still shows the **old** ring - a
single-frame capture of a new gradient is the cross-fade's start, not its end.

Checked and found correct, for the record: stop sorting (unsorted and sorted
inputs render bit-identical), CW/CCW agreement between
`GeometryUtils::GetPointOnRectangle` and the shader's `perimeterPosition`
across all eight perimeter spans, colour-stop alpha gating filament, halo and
bloom together at the right position, `operator==` coverage on every `Config`
sub-struct, `NeonRenderer` vs `NeonOptimizedRenderer` agreement (mean
|diff| 0.16/255, max 7 at `glowRadius` 30), and no beading at
`optimizedNeon.numSamples = 16`.

---

## Visual

### V1. `cornerRadius` is never clamped against the rect's half-extent

**Confirmed.** `cornerRadius` 260 and 500 on a 600x400 rect (valid maximum:
200).

| `cornerRadius` 260 | `cornerRadius` 500 |
| ------------------ | ------------------ |
| ![](images/review-findings/corner-radius-overflow-mild.png) | ![](images/review-findings/corner-radius-overflow.png) |

Three of the four places that consume the radius clamp it and one does not:

| consumer | clamps? |
| -------- | ------- |
| `perimeterPosition` ([`neon.frag:171`](../lib/shaders/neon.frag)) | yes, to `min(halfW, halfH)` |
| `peri` (perimeter length, same shader) | yes |
| `GeometryUtils::GetPointOnRectangle` | yes |
| `sdRoundBox(vPos, halfSize, uCornerRadius)` ([`neon.frag:351`](../lib/shaders/neon.frag), [`neon-optimized.frag:342`](../lib/shaders/neon-optimized.frag), [`black-rect.frag:85`](../lib/shaders/black-rect.frag)) | **no** |

Past the half-extent the unclamped SDF stops describing a rounded box - it
becomes a lens with cusps - while the gather samples still sit on the correctly
clamped stadium. So the filament draws the wrong shape *and* the colour ring
smears, because the samples it gathers from are nowhere near the fragments
being lit.

Reachable from the demo in one drag: the Corner Radius slider runs to 1080
([`demo/src/debug-ui.cpp:543`](../demo/src/debug-ui.cpp)) against a default
800x600 rect.

Fix: clamp once at the top of each `main()` and pass that value to every
consumer, and clamp again in `RectGeometry` or at the UI so the config never
carries an impossible radius.

### V2. Abutting arcs render a hard dark notch at every seam

**Confirmed.** Two arcs, `{start 0, length 0.5}` and `{start 0.5, length 0.5}`,
each with its own solid colour stops.

![](images/review-findings/arc-seam-notch.png)

`arcCoverContinuous`
([`neon.frag:325-345`](../lib/shaders/neon.frag)) feathers **inward**:
`tailIn` is 0 at `rel = 0` and `headIn` is 0 at `rel = length`. `emitCover`
combines arcs with `max` ([`neon.frag:587`](../lib/shaders/neon.frag)). At a
shared endpoint that is `max(0, 0) = 0` - not a dip, a hole - and it stays
below full brightness for `TAIL_FEATHER_PX + HEAD_FEATHER_PX` = 28 px. Because
`emitCover` also scales the halo and the bloom, the hole is punched radially
outward as a dark wedge, which is what makes it so visible.

The hue does *not* notch: the pre-pass's `arcInside`
([`neon-emission.frag`](../lib/shaders/neon-emission.frag)) uses an **outward**
feather, so colour crosses the seam smoothly. The two halves of the same arc
disagree by construction.

This is the documented multi-arc recipe, not an exotic case - see the `arcs`
examples in [`config.h:426-432`](../lib/include/core/config.h).

Fix: a soft union (`1 - (1-a)(1-b)`) instead of `max`, or overlap the two ramps
so they sum to 1 across the seam.

### V3. Arc-local gradients wrap under hue rotation, producing a travelling seam

**Confirmed.** One full-perimeter arc with stops white (head) to red (tail),
`hueRotationRate = 0.5`.

| t = 0 | after ~0.6 s of rotation |
| ----- | ------------------------ |
| ![](images/review-findings/arc-lut-wrap-seam-t0.png) | ![](images/review-findings/arc-lut-wrap-seam.png) |

All three shaders subtract `uTime * uHueRotationRate` from `uArc`
([`neon.frag:582`](../lib/shaders/neon.frag),
[`neon-optimized.frag:516`](../lib/shaders/neon-optimized.frag),
[`neon-emission.frag:137`](../lib/shaders/neon-emission.frag)), but `uArc` is an
**arc-local** coordinate on a head-to-tail gradient, not a cyclic perimeter
coordinate. The arc atlas is `GL_REPEAT` on U
([`neon-renderer.cpp:861`](../lib/src/renderer/neon-renderer.cpp),
[`neon-optimized-renderer.cpp:790`](../lib/src/renderer/neon-optimized-renderer.cpp)),
so the scroll eventually wraps and the tail colour butts straight into the head
colour, mid-edge, with no geometric feature to hide it.

Segments do not have this: their atlas is `CLAMP` and their LUT coordinate
carries no time term.

Fix: either drop the time term when the arc has its own stops, or bind the arc
atlas `CLAMP_TO_EDGE` on U so the scroll saturates instead of wrapping. The
`hasStops = false` path reads the base gradient in perimeter space and must
keep the time term either way.

### V4. The interior glow has visible medial-axis creases

**Confirmed.** `glowRadius` 60, `bloomStrength` 1.0.

![](images/review-findings/medial-axis-creases.png)

`halo` and `bloom` are closed forms of `ad = abs(d)`
([`neon.frag:648-649`](../lib/shaders/neon.frag)). Inside the rect the
rounded-box SDF's *gradient* is discontinuous along the medial axis (the
diagonals from each corner plus the central spine), so both terms inherit a C1
crease there. The gather this replaced summed over perimeter samples and was
smooth; a nearest-distance profile cannot be.

Subtle at the default `glowRadius` 5, unmistakable at 30 and above, and it is
what produces the "mitred picture frame" look in the interior of most captures
in this document.

The trade that bought it (geometry-independent glow width, no beading, no
sample-spacing floor) is documented and worth keeping. What is missing is
either a note in [`neon-tuning.h`](../lib/include/renderer/neon-tuning.h)
admitting the artifact, or a softened `ad` near the axis.

### V5. An arc's lit span is inset, but its colour is not

The magnitude comes from `arcCoverContinuous`: inward feather, pixel-based,
read pointwise at the fragment's own perimeter position. The hue comes from the
pre-pass's `arcInside`: outward feather, one sample wide, quantised to the
gather points. So an arc lights up at `start + TAIL_FEATHER_PX` and ends at
`start + length - HEAD_FEATHER_PX` while its hue leaks slightly past both ends.

The inset itself is a deliberate trade
([`neon-tuning.h:29-40`](../lib/include/renderer/neon-tuning.h)) and
`ARC_FEATHER_MAX_SHARE` keeps short arcs from losing their peak. The unlogged
part is that the two halves use *different* feather shapes, which is also the
root of V2.

### V6. `SampleStops` is cyclic, but the segment and arc atlases are head-to-tail

`ColorUtils::SampleStops` wraps from the last stop back to the first
([`color-utils.h:273-285`](../lib/include/util/color-utils.h)). That is right
for the base ring, which is genuinely circular and sampled `REPEAT`.

`rebuildSegmentLUT` and `rebuildArcLUT` bake the same function over
`t = x / (W - 1)` into a row that the shader samples as a head-to-tail span.
For stops that do not reach both 0 and 1 - say 0.2 and 0.8 - the head of the
span (`t = 0`) lands inside the wrap interval and shows a blend of the *last*
and *first* colours rather than the first stop, and the tail ramps back toward
the head colour instead of holding.

[`config.h:106`](../lib/include/core/config.h) calls this layout
"head-to-tail", so the bake should clamp at the ends. A `SampleStopsClamped`
alongside the cyclic one, used by the two atlas bakes, is the minimal change.

### V7. Arcs and segments past the caps are dropped silently

![](images/review-findings/arc-cap-truncation.png)

Twelve arcs authored, eight rendered. `packLightBlocks` does
`std::min(size(), MAX_ARCS)` with no diagnostic on either renderer. The demo UI
enforces `MAX_ARCS_CAP` / `MAX_SEGMENT_BOOSTS_CAP` so it never bites there, but
a library or C-ABI host gets no signal at all - not a log line, not a result
code.

---

## Implementation

### I1. Shader sources contain non-ASCII, against the project's own rule

| file | lines with non-ASCII |
| ---- | -------------------- |
| `lib/shaders/neon.frag` | 11 |
| `lib/shaders/neon-optimized.frag` | 10 |
| `lib/shaders/neon-stop-marker.frag` | 3 |
| `lib/shaders/shaders.h.in` | 2 |

All in comments (`-> x sum ...` written as arrows, multiplication signs, a
summation sign and an ellipsis). `AGENTS.md` and `CLAUDE.md` require ASCII-only
shaders precisely because these strings are handed verbatim to the GLSL ES
compiler on Mali / Tizen, where non-ASCII bytes can be rejected even inside a
comment. `neon-emission.frag` and `neon-tuning.h` are clean, so the rule is
being followed unevenly rather than dropped.

### I2. The emission pre-pass re-bakes unconditionally every frame

The table is a pure function of `(si, uTime, config)` - the pass's own stated
invariant. When `hueRotationRate` is 0 and no animation is attached, `uTime`
does not change the result, yet `renderEmissionPass` still resizes, binds,
uploads two UBOs and draws, in both renderers, every frame.

A dirty flag over (the config fields the pass reads, plus the effective time
term) would skip the pass outright in the static case, which is the common one
for a settled UI.

### I3. Registering both neon renderers doubles the CPU-side work

Each owns its own gradient / segment / arc LUTs, its own three UBOs and its own
128x2 emission FBO, and `OnConfigChanged` bakes both regardless of `enable`. On
top of that `SegmentUtils::FillEffectiveSegments` runs three times per frame
per renderer: once in `OnConfigChanged`, once in `packLightBlocks`, once in
`rebuildSegmentLUT`.

The demo registers both, so this is the default path, not a corner case.

### I4. `WireframeRenderer` does not follow the conventions the others do

Two deviations in [`wireframe-renderer.cpp`](../lib/src/renderer/wireframe-renderer.cpp):

- `OnConfigChanged` re-uploads its VBO on **any** config change, which with an
  animation attached is every frame. The neon renderers gate the equivalent
  rebuild on `geometry` alone.
- `Render` re-enables `GL_BLEND` but never restores `glBlendFunc` to the
  `SRC_ALPHA` convention every other renderer hands back. Harmless only because
  it happens to be registered first.

It also ignores `cornerRadius` entirely, so the debug box is a sharp rectangle
around a rounded one. Defensible for a bounding box; worth a comment saying so.

### I5. `Framebuffer::GetBoundId()` is a `glGetIntegerv` on the hot path

Called once per frame per multi-pass renderer
([`neon-renderer.cpp:220`](../lib/src/renderer/neon-renderer.cpp),
[`neon-optimized-renderer.cpp:167,377`](../lib/src/renderer/neon-optimized-renderer.cpp),
[`lens-flare-optimized-renderer.cpp:40`](../lib/src/renderer/lens-flare-optimized-renderer.cpp)).
It is the *correct* fix for the `OffscreenCapture` case and should not be
reverted to `BindDefault`, but a GL state query can sync the driver on a tiler.
Threading the target through `BaseRenderer::Render` would give the same
guarantee with no query.

### I6. `AddRenderer` after `Initialize` yields a live, uninitialized renderer

[`EdgeLightingEffect::AddRenderer`](../lib/src/core/edge-lighting.cpp) calls
`OnConfigChanged` but never `Initialize`, and `Initialize()` walks the list
exactly once. A renderer registered later has no shaders, and its `Render` runs
with program 0.

Fix: track whether the effect has been initialized and initialize on add when
it has, or state in the header that every renderer must be registered before
`Initialize`.

### I7. The emission pass's total-failure path leaves the gather reading texture 0

If both the `GL_RGBA16F` and the `GL_RGBA8` `Resize` fail,
`renderEmissionPass` returns early
([`neon-renderer.cpp:239`](../lib/src/renderer/neon-renderer.cpp)) - but
`Framebuffer::Resize` has already called `destroy()` on the failure path, so
`mEmissionBuffer.BindTexture(3)` in `renderNeonPass` binds 0 and the gather
samples undefined data in core profile.

The comment there says the gather "reads a stale table". There is no stale
table left at that point. Either keep the old attachment alive across a failed
resize, or set a flag that makes `renderNeonPass` bail.

### I8. `demo/` and `demo-capi/` have drifted apart

Diffing the two `debug-ui.cpp` string tables shows the C-ABI demo missing the
droplets band controls and carrying a materially different animation panel.

`CLAUDE.md` states the fork exists to prove the C ABI is self-sufficient for a
real UI-shaped host. The further the two drift, the less that guarantee is
worth.

---

## Suggested order

V1 and V2 are user-visible with default-ish settings and both have small,
contained fixes; V3 is a one-line decision about a texture wrap mode. V6 and V7
are correctness gaps that only bite specific authoring patterns. V4 is a
documented-limitation call rather than a bug. On the implementation side I1 is
a portability risk against the stated target platform and costs nothing to fix,
I6 and I7 are latent crashes waiting for an unusual host, and the rest are
efficiency and consistency work.
