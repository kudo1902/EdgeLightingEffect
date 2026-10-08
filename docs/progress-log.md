# Progress log

Dated entries, newest first: what changed, what was measured, what was decided,
and what is open. Each entry names the commits it covers; the headers under
`lib/include/` stay the source of truth, and the topic documents linked here
hold the detail.

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

- **A two-channel field for segment configs** (the reduced-scale plan's D3,
  declined earlier for its memory): segment configs pay ~1.5 ms more than arc
  configs on every rotating-hue frame (2.44 vs 0.90 ms) and ~0.3 ms more on
  every still one; estimated 2.44 -> ~1.0 ms for ~1.1 MB. An owner decision.
- **A lower `resolutionScale` for animated content** (no code): 8 arcs moving
  3.07 -> 2.29 ms at 0.25, <= 3 levels.
- **Measure on the target**: which frames production draws, half-float render
  targets, fill rate, the CPU.
- The onboarding guide's direct-path / scaled-path passages still predate I57;
  they need a proper rewrite rather than a wording fix.
