# Progress log

Dated entries, newest first: what changed, what was measured, what was decided,
and what is open. Each entry names the commits it covers; the headers under
`lib/include/` stay the source of truth, and the topic documents linked here
hold the detail.

## 2026-10-09 (late night) - I63: a uniform the field bake compiles out

`1d9f509`. I63 in [`review-findings.md`](review-findings.md) has the detail.

- **Reported.** On Tizen: `uniform 'uGlowSideSoftness' not found` on
  `NeonRenderer.Field`.
- **Cause.** The field bake returns at the grade, before the one-sided cut,
  which is the uniform's only reader, so an optimising compiler drops it; the
  shared upload still set it. Harmless, and as old as the bake; Apple's GL
  keeps the uniform, so it never showed on the Macs.
- **What changed.** `UploadEdgeMaskUniforms(..., cutSoftness)`: the bake alone
  is not sent it.
- **Measured.** Byte-identical frames, `check` identical, nothing logged on the
  AMD. Every neon program's uploads audited against what its variant reads; no
  other uniform is dead anywhere. Open: confirm on the device.

## 2026-10-09 (late night) - V26: one corner of a one-sided glow

On top of V25, uncommitted. AMD Radeon Pro 5300M. V26 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** With `GlowSide::INSIDE`, the top-left corner was not sharp
  while the other three were - for some rects, not all.
- **Cause.** Every edge ramp was sized by `fwidth(d)`. On a sharp corner's
  vertex pixel the 2x2 quad can hold both outside neighbours, so `fwidth` reads
  2 and the ramp doubles: that one pixel came out at 0.84 (the opaque fill's
  at 0.75). The quad's parity decides which corners, so it moved with the
  rect's position.
- **What changed.** `sdRoundBoxFwidth` in `neon-sdf.glsl` - the SDF's analytic
  gradient against the position's derivatives - in `neon.frag`,
  `neon-field.frag`, `neon-blit.frag` and `black-rect.frag`.
- **Measured.** Random rects' four corners agree within the dither (up to 40
  levels before; the fill 64 -> 0) for an inside glow, an outside glow, an
  inside cutoff at softness 0 and the fill, at 0.25-1.0. Two-sided scenes
  byte-identical; `check` identical, `partition` passes; five guide figures
  move by 1 level. Cost +0.005-0.008 ms on still frames of the one-sided
  scenes at 0.5 (`glow_inside` 0.090 -> 0.098), nothing measurable elsewhere.

## 2026-10-09 (night) - V25: an arc's end on a sharp corner

On top of `fb6f06f`, uncommitted. AMD Radeon Pro 5300M. V25 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** The arc wipe, started from position 0: the tail parked on the
  top-left corner was asymmetric, with a hard cut.
- **Cause.** The filament read one coverage per fragment, at its NEAREST
  perimeter point, and inside a sharp corner that map jumps across the
  diagonal - fragments nearer the unlit edge dropped the lit edge's light, so
  the end ramp's inside half was cut along the diagonal. The same at every end
  passing within the filament's reach of a corner tighter than that reach.
- **What changed.** The filament is a max over the outline's pieces (four
  straights, four corner arcs), each at the fragment's distance from it and the
  coverage at its own nearest point (`filamentPieceDistance`, `perimeterAt`,
  `filamentCover` in `neon.frag`); `perimeterPosition` is gone.
- **Measured.** The corner ends symmetric and soft; changes confined to the
  corners' boxes (up to 132 levels there). Uniformly lit rings, a ring with a
  translucent stop and a segment on a corner byte-identical; `check` identical
  to the build before, `partition` passes, all 69 guide figures
  byte-identical; direct against field within 1; an end swept through a
  corner in 0.5 px steps never pops. No measurable cost: `time` geometric
  means 0.94-0.99 after / before on still, arc-wipe and segment-travel at 1.0
  and 0.5, inside the 1.13x round-to-round spread.
- **Open.** On a rect smaller than twice the filament's reach a lit edge's
  filament now crosses the interior rather than stopping at the spine -
  correct, but a visible change on tiny rects with a fat soft line. Nothing
  timed on the M2 Pro. The onboarding guide and the reference pages still
  describe the nearest-point read.

## 2026-10-09 (evening) - The glow coverage table at the resolution each light needs

[`neon-glow-cover-resolution-plan.md`](neon-glow-cover-resolution-plan.md)
built, uncommitted on top of `14ef6c7`. AMD Radeon Pro 5300M; I59-I62 in
[`review-findings.md`](review-findings.md) and the plan's section 11 have the
detail.

- **Tooling (step 0).** `neon-scale-check` gained the frames of the original
  report: `time --mode lights --arcs N --segments M`, `--mode resize`,
  `--set cover` / `--scene` / `--scales`, `--gpu` / `--passes` (per-pass timer
  queries through `PassRecorder`) and the first frame's time; `generate --set
  cover` (17 animated scenes, the plan's prototypes among them) and `diff`,
  the plan's criterion as an exit code.
- **Found on the way.** `GL_TIME_ELAPSED` on this GPU reads 3.6-4.0x the
  wall-clock time of the same frames, steadily: every timer figure measured
  here before (`neon-animation-perf-analysis.md`, the plan's sections 2 and 6)
  is a ratio, not milliseconds. The band's "4.0 ms" eight-segment frame is
  1.05 ms.
- **What changed.** The table's layout is a parameter stored with it (I59,
  byte-identical); its width follows the rect and the halo width, 256-1024
  columns (I59); the segments are baked into a narrow table of their own and
  copied in by a fill pass, `neon.frag` unchanged (I60); each bell is cut at
  4.24 sigma instead of 5 (I61); pass 1b no longer computes the filament the
  ring always redraws (I62, byte-identical).
- **Measured.** Wall-clock at 0.5 with every light changing length: the band
  with eight segments 1.05 -> 0.61 ms (1.72x), four arcs and four segments
  0.87 -> 0.63 (1.38x); the 960 x 540 rect with eight segments 1.44 -> 0.92
  (1.57x); arcs alone 1.05-1.10x; still, hue and resize frames unchanged. In
  timer units every row landed on the plan's estimate. Memory 1.0 MB -> 0.25-0.69
  MB for a default glow (1.0 still for a thin glow on a large rect), plus
  64-125 KB of segment table. Every step within 2 levels and 99.9% of lit
  pixels within 1 of the step before; `check` and `partition` pass throughout.
- **Decided.** The plan's 8-node rule was built and NOT taken: alone it read 3
  levels off a long, bright segment, under the criterion. Overhangs and the
  criterion itself (the plan's decisions 1 and 2) unchanged.
- **Open.** Nothing timed on the M2 Pro; the 8-bit fallbacks at the new widths
  unmeasured.

## 2026-10-09 (later) - V24: the gap between the edge ring and the blit

On top of `063603f`, uncommitted. Measured on an AMD Radeon Pro 5300M. V24 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** A gap between the ring band and the lit area outside it.
- **Cause.** Not the partition (`partition` passes) but the ring's field: its
  bake drew the ring mesh into the TRANSPOSED side strips, where a ring edge
  snapped onto a pixel centre goes to the other side of the fill rule, so the
  composite drew pixels whose texel was never baked - a 1 px dark seam the
  height of a side strip, on ~1% of configs, at every scale.
- **What changed.** `renderRingFieldPass` draws the layout's whole box per
  strip (`mRingField.box`), clipped by the strip's viewport, so every texel is
  baked whatever the rasteriser does with an edge.
- **Measured.** Direct-vs-field pixels over 8 levels 303 -> 2 on 400 random
  configs (the 2 are an unrelated corner pixel, unchanged); 1-level flips
  0.87% -> 0.91% of lit pixels; directly shaded frames byte-identical; `check`
  and `partition` pass; the bake frame no slower.
- **Open.** Two single pixels at a nearly square corner differ 10-17 levels
  between the ring shaded directly and its field, before and after this change.

## 2026-10-09 - The references follow V23 and R7

Uncommitted, on top of the two entries below. Apple M2 Pro.

- **The comparison page** (`docs/neon-resolution-scale-comparison.html`) was
  regenerated as `tools/neon-scale-check/README.md` describes: `542dad4` built
  in a scratch tree, `generate --mode hue` three times per build interleaved,
  one image run, `update-page.py`, the images copied over. `542dad4`'s metrics
  reproduced the page's exactly. `check` passes again, drift 0 on all twelve
  scenes.
- **What the page now shows.** Every reduced scale p99 1 against 1.0 (max 2,
  but `small_rect`: 3 at 0.25, 10 at 0.125). With the hue rotating every scale
  costs about the same, 0.13-0.18 ms, since the fields serve that frame below
  1.0 too; only `small_rect` gains clearly from a lower scale there (2.0x at
  0.25). Its notes, lede and two findings were rewritten - they described the
  2026-10-06 tree, before I47-I58.
- **The guide's figures** were rerun: 58 of 68 moved (the dither touches every
  final frame). `docs/neon-shader-outputs.html` names the reduced buffer's
  new format and the blit's dither.
- **Open.** The dither's cost on the target GPU; the guide's prose is still
  stale since I44.

## 2026-10-08 (night) - R7: the output is dithered

On top of V23, uncommitted. Measured on an Apple M2 Pro. R7 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** "Many banding circles" on the same capture: the glow's dark
  tail rounds into one-level plateaus up to 17 px wide outside and 32 inside,
  concentric round the corners.
- **What changed.** `neonDither` (`neon-grade.glsl`, `OUTPUT_DITHER_LSB`):
  +/- half a level of interleaved gradient noise on the colour of the blit, the
  edge ring and the ring field's composite - the three writes to the caller's
  framebuffer. And `mScaledBuffer` is RGBA16F (RGBA8 fallback,
  `SCALED_FORMATS`): with an 8-bit buffer the plateaus are baked in before the
  blit, and the dither alone left the rings (measured).
- **Image.** Widest plateau 17 -> 7 px outside, 32 -> 10 inside; block means
  unchanged (-0.01 levels). Reduced scales moved closer to 1.0. `partition`
  passes; the field composite within 1 level of direct.
- **Cost.** ~+0.01 ms a still frame at 1080p at every scale (timer query; the
  dither, most likely by defeating framebuffer compression), +0.02 at 0.5 and
  +0.13 at 1.0 on the reported 3600 x 2126 frame (the half-float read). The
  reduced buffer's memory doubles (1.6 -> 3.2 MB for 800 x 500 at 1080p at 0.5).
  Unmeasured on a Mali tiler.
- **Open.** Whether that cost is acceptable on the target is the owner's call;
  `OUTPUT_DITHER_LSB` 0 turns the dither off. (`check`'s references were
  regenerated on 2026-10-09.)

## 2026-10-08 (evening) - V23: the bloom fades out instead of stopping on a crease

On top of `457ec75`, uncommitted. Measured on an Apple M2 Pro. V23 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** A demo capture (1800 x 1063 rect, glowRadius 5, bloom 0.52,
  scale 0.125): no smooth transition where the glow ends - a hard-edged dark
  rectangle `reach` (340 px) inside the line, a rim the same distance out.
- **Cause.** The bloom's pedestal lands it on 0 at `reach` with its slope still
  on. Inside, nothing hid that crease; outside, the quad-edge fade did, but
  starting at 0.8 of the margin it steepened the tail before flattening it.
- **What changed.** Each piece's bloom (straights and corner arcs) is faded by
  its own distance, both sides, from `BLOOM_FADE_START_FRAC` (0.5) of `reach` -
  the latest start that does not steepen the tail - floored at a cutoff's end
  as the quad-edge fade is (`neon.frag`, `neon-tuning.h`).
- **Image.** At most 6 levels, in the bloom's outer half. Reduced scales against
  1.0 within their bounds (max moved by 1 in three cells); the field composite
  within 1 level of direct shading; `partition` passes (seeds 1, 7).
- **Cost.** Arc-wipe frames 0.967x at 1.0, 0.973x at 0.5; still frames
  unchanged.
- **Open.** `check` failed at 1.0 on five scenes (drift 3-6 against the
  committed images, bound 2) until the references were regenerated on
  2026-10-09 (above).

## 2026-10-08 (later) - D3: the fields take segment configs

On top of `d54465a`. Measured on an Apple M2 Pro, 1920 x 1080. I58 in
[`review-findings.md`](review-findings.md) has the detail.

- **What changed.** The hue-invariant fields (pass 1f / 1c, and the ring's 1r /
  2r) now serve configs with segments. `neon.frag` is linear in both hues, so
  the bake puts the arc hue on red and the segment hue on green and writes
  `Fa` and `Fs` in one draw into an RG16F field (R16F as before without
  segments, `GetFieldFormat`); the composite adds `segColHue * Fs` behind
  `uFieldSegments`. The intensity gain scales `Fa` only.
- **Measured.** `segments` hue frames 2.09x at 0.5 (3.20x at 1.0, 1.58x at
  0.25), still frames 1.5-2.1x; every other scene unchanged (geomean 1.00 hue,
  1.01 still).
- **Image.** Configs without segments byte-identical (all `neon-scale-check`
  images in five modes, the 68 guide figures). Segment configs within 1/255 on
  hue and still frames, within 2 while an intensity falls (I56's bound).
  `check` and `partition` pass.
- **Cost.** Memory on segment configs only: 0.84 -> 1.7 MB for the glow's
  field at 0.5 for a 960 x 540 rect, ~0.27 -> ~0.54 MB for the ring's.
- **Not covered.** A travelling segment still changes the config every frame,
  so neither field holds there (as for arcs); still measured on the M2 only.

## 2026-10-08

Commits `ae59629`, `61c3bda`, `51f8825`, `f4ab7df`, `daf0ff4` on
`improve_neon_perf`, on top of `6042866`. Measured on an AMD Radeon Pro 5300M,
1920 x 1080.

### NeonRenderer structure - no pixel moved

Verified after every step against the build before it: `neon-scale-check check`
(output identical), `partition` (seeds 1 and 7, 0 px overlap, 0 px gap) and the
68 guide figures (byte-identical).

- **One way to build a program.** Every program goes through `ensureProgram`
  (fragment source, optional define, `PROGRAM_*` failure bit, `BLOCK_*` mask of
  the uniform blocks to bind, the log line's consequence); a failed build is
  logged once and never retried. `setupShaders` and `buildNeonProgram` are gone;
  `Initialize` builds only the emission program. The opaque fill is built the
  first time a fill draws through its shader (`ensureFillProgram`), so a host
  with no fill, or one a clear stands in for, never compiles it.
- **One way to allocate a buffer in the best format.** `ResizeInBestFormat`
  replaces three copies of the format walk; `resizeEmissionBuffer` and
  `resizeGatherBuffer` are gone, with the emission walk's dead "resume from the
  buffer's own format" branch. The fallback path was checked by forcing an
  invalid first format in every list: the old and new code log the same
  messages and draw identical frames.
- **Names that say what the code does.** `ensurePathPrograms` ->
  `ensureGlowPrograms`, `renderNeonPass` -> `renderShadePass`, `renderOpaqueFill`
  -> `renderFillPass`, `mBlackRectShader` -> `mFillShader`, `mNeon*Shader` ->
  `mGatherShader` / `mShadeShader` / `mRingShader` / `mFieldBakeShader`,
  `currentEmissionInputs` / `currentGatherInputs` -> `getEmissionInputs` /
  `getGatherInputs`, and eight `.cpp` helpers to the `Get*` rule
  (`docs/naming-review.md` S8): `GetCircumscribedBox`, `GetInscribedBox`,
  `GetScaledExtent`, `GetRegionProjection`, `GetRegionUVMap`, `GetAnnulusArea`,
  `GetGlowCoverPieceStart`, `GetGlowCoverPiecesTouching`. Function casing was
  already right everywhere.
- **State grouped by resource.** 58 top-level members down to 41:
  `Mesh` (a vertex array and its count), `NeonDetail::Annulus` /
  `BufferRegion` / `UVMap` (shared with the `.cpp` helpers), `EmissionBake`,
  `GatherCache`, `ShadeLimits`, `LightBlocks`, `EmissionTable`,
  `GlowCoverTable`, `GatherTarget`, `GlowField`, `RingField`, `OffscreenState`
  (`mOffscreen.reusable`, `.viewport`). `mGlowArea` became `GetAnnulusArea`;
  `uploadNeonUniforms` takes a `ShadeLimits`.
- **Ordered by role.** The header groups methods (types, programs, buffers,
  CPU-side inputs, geometry, skip keys, uniform uploads, then the passes in the
  order `Render` runs them) and members (the same roles, then one group per
  offscreen buffer); the `.cpp` defines the methods in the header's order.
- **Doxygen everywhere.** Every type, method, member and `.cpp` helper carries
  a doc comment. Stale comments found on the way were fixed: `ArcBlockData`'s
  `.w` (the `PackArcFlags` bitmask, not `hasStops`), `mOffscreen`'s
  (it is set every frame, and is reused at every scale), the `emitCover`
  comment in `neon.frag` (it feeds the filament only), and a `gl-utils.h`
  comment naming a removed pass.

Two measurements made on the way: **`mOffscreen.reusable` is still needed**
after the gather got its own cache - without it a still frame is 4.2x slower
at 0.5 (3.1-5.5x over the twelve check scenes), 10.3x at 1.0 - and the
**offscreen phase is P0 -> P0b -> P1a -> P1f -> (P1b or P1c) -> P1r**, then
the fill, the blit and (P2c or P2r).

### Docs

- `docs/neon-shader-outputs.html`: the dataflow figure redrawn as one lane in
  frame order, with the two per-frame choices (P1b or P1c, P2c or P2r) side by
  side; the sections reordered to match.
- `docs/neon-renderer-reference.html`: Fig 4 (one frame's draw sequence)
  redrawn as one column of today's schedule - the old chart compared the
  removed 1.0 path with the reduced one and named `NEON_READS_GATHER`.
- Stale "two resolution paths" wording fixed in CLAUDE.md, `implementation.md`
  and the reference page; `implementation.md`'s "where to change what" rows
  pointed the gather loop at `neon-common.glsl` (it is `neon-gather.frag`'s).
- Every rename above followed in CLAUDE.md, `implementation.md`, the onboarding
  guide, the outputs page and the reference page. The history documents (plans,
  reviews, findings) keep the names of their time.

### Performance - frame rate while arcs or segments change length

Reported: the frame rate drops when several arcs or segments change length every
frame. Attributed per pass (scale 0.5, a 960 x 540 rect): a config change
re-runs the coverage bake and re-shades P1b and the ring; the CPU is under
0.25 ms. Two exact changes, both byte-identical (the three gates above, plus 33
animated captures aimed at the changed code):

| change | measured |
| ------ | -------- |
| `filamentLit` (`f4ab7df`): `neon.frag` skips the filament's pointwise inputs - `sPos`, the stop alpha, `emitCover`, `segCoverPt` - wherever the filament is exactly 0 | P1b no longer grows with the light count: 8 arcs 1.90 -> 1.20 ms, frame 3.82 -> 3.07 ms; 8 segments on a rotating hue 2.74 -> 2.44 ms |
| per-type coverage re-bake (`daf0ff4`): `dirtyArcPieces` / `dirtySegmentPieces`; a piece only one light type dirtied integrates that type and writes its channels under `glColorMask` | arcs over still segments: bake 1.35 -> 0.35 ms, frame 3.96 -> 2.79 ms (1.42x) |

`neon-scale-check time` (`still`, `arc-wipe`, `segment-travel`, three
interleaved rounds against the build before): no regression.

The deeper study is [`neon-animation-perf-analysis.md`](neon-animation-perf-analysis.md):
cost by frame type (still 0.42 ms, rotating hue 0.9, 8 arcs moving 3.1, 8
segments 5.4), fragments and visible share per pass (no area is wasted - the
blit's 1.64M pixels are 96% visible), where the time goes inside the shading
(the analytic pieces are over half of it), a ranked list of what is left, and
what was ruled out.

### Decided

- **Keep the name `Annulus`** for `NeonDetail::Annulus` (offered `HollowBox`).
- **Approximate options not taken** for light animation, offered with numbers:
  narrower bell support + 8-node bake (8 segments 5.40 -> 3.91 ms, <= 2 levels
  on ~3% of pixels), 8 nodes alone (<= 3 levels on 9.4%), half-rate soft glow,
  cached per-piece re-shade. Kept on record in the analysis document.

### Open

- ~~**A two-channel field for segment configs**~~ - built the same day (I58,
  the entry above).
- **A lower `resolutionScale` for animated content** (no code): 8 arcs moving
  3.07 -> 2.29 ms at 0.25, <= 3 levels.
- **Measure on the target**: which frames production draws, half-float render
  targets, fill rate, the CPU.
- The onboarding guide's direct-path / scaled-path passages still predate I57;
  they need a proper rewrite rather than a wording fix.
