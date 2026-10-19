# Progress log

Dated entries, newest first: what changed, what was measured, what was decided,
and what is open. Each entry names the commits it covers; the headers under
`lib/include/` stay the source of truth, and the topic documents linked here
hold the detail.

## 2026-10-19 - The clock is a double; shaders get bounded phases

Uncommitted. `Clock`, `BaseRenderer` and every renderer's time path, four
shaders, `droplets-tuning.h`, and a `neon-scale-check clock` mode.

- **The defect.** `Clock` accumulated `float mTime += deltaTime`, and that
  float went to the shaders as `uTime`: the neon's hue (`si - uTime *
  uHueRotationRate` in the emission pre-pass, the same in `neon.frag`'s
  gradient alpha read and the debug strip), the droplets (`uTime * 0.2 *
  uSpeed`) and the flare's ray spin (`time * rotationRate * 2 pi`). At 60 fps,
  10 s of frames moved the clock 9.375 s after 0.5-2 days of uptime, 18.75 s
  after 3.1-4 days, and 0 s past 2^19 s (~6.07 days): the hue, the drops and
  the rays ran at the wrong speed, then stopped. Edge devices run the effect
  for days. Per-animation elapsed time was never affected (LOOP wraps it).
- **The clock.** `Clock::mTime`, `GetTime` and `SetTime` are `double`, and so
  is the `time` every renderer is handed (`BaseRenderer::Update`,
  `RenderOffscreen`, `Render`) - double rather than a wrapped float because the
  wrap a phase needs depends on its rate, which only the renderer knows. A
  host's own renderer has to change its overrides (`docs/upgrade-notes.md`
  section 6); the C ABI has no clock-time function, so no export changed.
- **The phases.** No shader is handed an absolute time any more; each renderer
  reduces it in double on the CPU (`GetCyclePhase`, `WrapPeriod` in
  `animation/clock.h`):
  - neon and debug strip: `uHuePhase = fract(time * hueRotationRate)` replaces
    `uTime` and `uHueRotationRate` in `neon-emission.frag`, `neon.frag` and
    `neon-lut-debug.frag`. The emission table's key is now the phase itself
    (`EmissionBake::huePhase`; `EmissionInputs` lost `hueRotationRate`, which
    the pass no longer uploads) - still exact, and at rate 0 the phase is 0 at
    every time, as the old key's rate test was. `RenderOffscreen` computes it
    once and hands it to `Render`'s ring pass through `FrameState`.
  - flare: `uRotation = GetCyclePhase(time, rotationRate) * 2 pi`. The shader
    reads it only through `mod(a, 2 pi)` and `sin(a * N / 2)` with N whole, so
    the wrap is seamless.
  - droplets: `uDropTime = WrapPeriod(time * 0.2 * speed, 2048)`. The field is
    not periodic on its own - the trickling layers scroll their grid and hash
    each cell's row - so the shader now hashes the row modulo
    `DROPLET_HASH_ROWS` (3072), the number of rows a 2048 wrap scrolls in both
    layers. The wrap then lands every cell on one with the same hash and
    sub-cell offset, and every `fract(t + n.z)` lines up: no seam. The modulo
    is an identity until a row on screen reaches 3072, late in the first ~2.8 h
    period at speed 1. Rows would repeat on screen only past ~3072 rows of the
    fine layer, ~10,000 px at a 1 px cell (`droplets-tuning.h`).
- **Verified** (Apple M2 Pro, Release). `neon-scale-check check`, `partition`
  (1000 configs, 0 overlap, 0 gap) and `shared-buffer` pass; `generate
  --images` against the build before is byte-identical on all 210 page images
  (they draw at time 0). The new `neon-scale-check clock` sets the clock to
  870400 s (~10.07 days, a whole number of cycles of every phase involved):
  600 frames of 1/60 s advance it 10.000000522 s, exactly their sum (a float
  accumulator: 0 s); the hue phase steps within 5.1e-8 of a cycle of rate / 60;
  30 frames of neon (hue rotating), droplets, flare (rays spinning) and the
  debug strip match the same frames from 0 byte for byte, and the first and
  last differ by up to 154 levels. The same mode built against the library
  before fails all three (clock 0 s, frames up to 174 levels off the run from
  0, first and last within 1 level - frozen). At an ordinary uptime the phases'
  rounding moved from the GPU to the CPU: 120 frames from 0 differ from the
  old build by at most 2 levels on 0.015% of pixels (`clock --dump` + `diff`).
- **Open.** The other time inputs are deltas (`Update`'s `deltaTime`, the
  gradient ring's cross-fade, the glow coverage table's release timer) and
  per-animation elapsed times, which stay float and stay bounded. A
  `Modulator` evaluated on the absolute clock by a host would narrow it to a
  float again; `modulator.h` now says to use an animation's own elapsed time.

## 2026-10-19 - C ABI: enum inputs validated, `OpaqueMode` parity pinned

Uncommitted. `lib/capi` and `FieldBoundAnimation` only; no exported signature
changed.

- **Asked for.** A review found the C boundary sound (no exception escapes,
  handles and indices are checked) with two gaps: `OpaqueMode` was the one
  mirrored enum without parity `static_assert`s, and enum-typed setters stored
  any integer a foreign caller passed and returned `EL_SUCCESS`.
- **Parity.** `capi-internal.h` now asserts all five `OpaqueMode` values against
  `el_opaque_mode_e`. `EndAction` and `AnimationState` convert through switches
  that name every enumerator on both sides (the default arm only catches
  foreign integers), so they stay assert-free, as the comment there says.
- **Validation.** One `isValid(el_*_e)` predicate per mirrored enum a setter
  stores through `static_cast`, each a switch with no default (so `-Wswitch`
  flags a new enumerator), behind `VALIDATE_ENUM`. Out-of-range values now log
  and return `EL_ERROR_INVALID_PARAMETER`, leaving the config untouched, in
  `el_effect_set_winding`, `_opaque_mode`, `_glow_side`, `_blend_space`,
  `_segment_blend_space`, `_preserved_segment_blend_space`, `_arc` (blend
  space), `_spotlight_clip_mode`, and every `el_animation_add_*field` binder
  (config, segment, preserved segment and its stop, arc, spotlight, arc stop,
  segment stop). `el_animation_from_modulator` returns null on a bad field.
- **Negative indices.** `VALIDATE_INDEX` added to `add_segment_field`,
  `add_arc_field`, `add_spotlight_field`, which cast to `size_t` unchecked
  (a negative became ~2^64 and bound a slot that never exists), and to
  `add_arc_stop_field` / `add_segment_stop_field`, which lacked it too.
- **Log once.** `FieldBoundAnimation`'s write helpers logged an error on every
  `Apply` while a binding's index was out of range, i.e. every frame after a
  host shrank a pool. Each binding that can go out of range now carries a
  `mutable outOfRangeLogged`: the first miss logs, later misses are silent, and
  the next successful write re-arms it. Binding structs grew one field; the
  `push_back({...})` aggregate inits are unchanged.
- **Verified.** Both demos and the C library build. Through `ctypes` against
  the built dylib: out-of-range winding / opaque mode (5, -1) / glow side /
  blend space / clip mode / field enums and negative indices return -4
  (`EL_ERROR_INVALID_PARAMETER`); valid values still return 0. No renderer
  output is touched.
- **Open.** `el_animation_set_end_action` and the playback setters still
  coerce an unknown value to the default through their helpers instead of
  rejecting it.

## 2026-10-19 - The neon's reduced buffer shared with the other renderers

`17046b2` - `b4fe8c2`; the review's follow-up (the "Reviewed" item below)
uncommitted. AMD Radeon Pro 5300M.

- **Asked for.** The neon's scaled buffer - line and glow, no opaque fill -
  shared with the library's other renderers, with no extra draw: the buffer
  the neon already renders, handed out as a `Framebuffer`.
- **What changed.** A core registry, `SharedBufferRegistry`
  (`core/shared-buffer-registry.h`), owned by `EdgeLightingEffect` and handed to every
  renderer it registers (`BaseRenderer::GetSharedBufferRegistry`). The neon publishes
  `SharedBufferId::NEON` on every frame that draws the glow - its own
  `mScaledBuffer`, the uv map from `gl_FragCoord`, the viewport and a version
  that moves when it redraws - and a reader takes it with `Get` in its Render.
  Internal only: no C ABI, no host accessor. A first version published only
  while a reader requested it; made unconditional the same day, which removed
  the request counting, the demand-change invalidation and a uniform, for no
  measurable cost.
- **The one thing in the shader.** I62 reverted: pass 1b and its field bake
  shade the filament again (`filamentLit` no longer reads `uBlitOwnsCut`), so
  the buffer holds the line; the blit still never reads those texels. The cut
  and the cutoffs stay out of the buffer - only the blit can apply them at full
  resolution - and a reader applies them itself if it wants them.
- **Measured.** Every final frame byte-identical against the build before:
  the `cover` set (306 frames, 1080p and 4K) and all 210 page images. Frame
  time after / before, geometric mean over the twelve scenes: 1.006 / 1.007
  (arc-wipe, 0.5 / 0.25), 0.949 / 1.001 (intensity), 0.983 / 1.000
  (segment-travel) - noise. No memory. `neon-scale-check shared-buffer` (new):
  a probe renderer is handed the buffer on every scene and scale, still and
  through the field's composite (66 of 72 hue cells), with 0.94-1.24 of the
  frame's light on the line, against 0.58-0.89 for a scratch build without the
  filament, which the gate fails. `check` and `partition` pass. Four guide
  figures of intermediate buffers have the line again; re-rendered on the AMD,
  the other 65 left as committed (rendering them here moves nearly all of them
  by the GPU alone).
- **The whole viewport.** The reduced buffer, and so the shared buffer, now
  covers the whole viewport at the scale (`GetViewportRegion`), so a reader's
  uv is `gl_FragCoord.xy / viewport`; it used to be a region round the rect,
  capped at exactly this. The glow's field keeps a region of its own - the
  glow quad's box as a window of the same grid (`GetFieldRegion`,
  `GetSubWindow`) - so it is used where it was (66 of 72 hue cells, as before),
  and a write clears only that window and the last write's (`mScaledDirty`),
  every other texel being 0 already. 4.1 MB at 1080p and 0.5 whatever the rect;
  `mScaledOuter` and `GetBufferRegion`'s viewport cap are gone, unused.
  Measured on the AMD at 4K against the region build: hue frames 1.00-1.01x,
  still frames 1.00-1.03x (12 rounds on the worst scene), arc-wipe frames
  1.01x over the twelve scenes, intensity frames 1.03-1.13x with identical GPU time (the scissored clear
  costs the driver CPU time, and those frames are CPU-bound - with no clear at
  all they ran 0.93x, the old region's full clear being ~7%), a full clear of
  the viewport-sized buffer 1.2-1.65x, which is why the clear is windowed.
  Frames within 1 level of the region build: the blit's uv map and pass 1b's
  projection take other constants, so a few bilinear weights and texel
  positions round differently (306 `cover` frames pass `diff`, max 1; 97 of
  144 buffer dumps texel-identical, the rest within 1 but 2 texels of
  `bounded_band` at 0.75 that sit on a discard boundary past the outside
  cutoff, where the blit masks them to 0). The windowed clear is
  byte-identical to a full clear.
- **Cleaner architecture.** The buffer is now a core object, `SharedBuffer`
  (`core/shared-buffer.h`): it owns the GL buffer, its format tier, the
  windowed clear, the version and a ready flag, where `NeonRenderer` held four
  loose members and three methods for them. `SharedBufferRegistry` is two calls -
  `Register` (once, from `Initialize`; a second producer is refused) and `Find`
  (a `shared_ptr<const SharedBuffer>`, which never dangles: a reader holding it
  past the effect's life finds a released, not-ready shell) - with no
  per-frame publish or withdraw (five withdraw sites and a `published` flag
  gone). The registry reaches renderers privately (`friend
  EdgeLightingEffect`), and the frame has two phases: the effect runs every
  layer's `RenderOffscreen` before any layer's `Render`, so a reader sees this
  frame's buffer wherever it sits in the layer order (the gate now checks a
  reader before the neon against one after it), and every layer's offscreen
  work lands before the first draw to the caller's target. `NeonRenderer`'s
  `Render` split at its old phase boundary, with `FrameState mFrame` between
  the two. Byte-identical to the build before (306 `cover` frames, 210 page
  images, 144 buffer dumps); timing within noise (0.99-1.07x at 4K); `check`,
  `partition` and `shared-buffer` pass. The debug inset lost its outline,
  which only ever traced the viewport now.
- **Names.** One noun for one concept: the object is `SharedBuffer`, its slot
  key `SharedBufferId`, and the directory of them `SharedBufferRegistry` (it
  allocates and draws nothing, so not "the buffers"), with `Register` /
  `Unregister` / `Find` and `BaseRenderer::GetSharedBufferRegistry`; files
  `core/shared-buffer.h` and `core/shared-buffer-registry.h`. They were
  `SharedTarget`, `SharedBuffers`, `Produce` / `Read` - two nouns, "buffer"
  and "target", for the one thing, and the only plural class name in `lib/`.
- **Rechecked.** `core/edge-lighting.h` had started pulling in glad and the
  log macros (through `base-renderer.h` -> `shared-buffer-registry.h` ->
  `gl/framebuffer.h`), which collides with a host's own GL headers; the
  registry now only declares `SharedBuffer`. A probe walking one
  effect through a moved, shrunk, rescaled, resized, off-screen and one-sided
  rect found no stale texel left by the windowed clear (every step within
  0.25/255 of a fresh effect - the gather's reuse, I55), and a reader drawn
  before the neon seeing the previous frame's version, which the two-phase
  frame (above) has since ended.
- **Debug view.** `DebugConfig::showSharedBuffer` (with `sharedBufferInsetSize`):
  `DebugRenderer` draws the buffer as an inset in the bottom-left corner, one
  block per texel (`texelFetch`, `shared-buffer-debug.frag`), and a frame round
  where its map puts it - read through `GetSharedBufferRegistry()`, so it is the first
  internal reader. Toggled from the C++ demo's Debug section; no C ABI setter,
  so the C ABI demo has no toggle.
- **Reviewed.** A review of the branch found two claims the code fell short
  of. No gate exercised the windowed clear's second half, the last write's
  window: every `shared-buffer` cell changes the scale, which reallocates the
  buffer and clears it whole, and no frame shows a stale texel. A scratch build
  that cleared only this write's window left 2790-218880 stale texels (scales
  0.125-1.0, up to 47/255) and passed `check`, `partition` and
  `shared-buffer`. The gate now moves and shrinks the rect at every scale and
  compares the buffer with a fresh effect's texel for texel: 0 here, and the
  scratch build fails at every scale. And "every layer's offscreen work lands
  before the first draw to the caller's target" held for the neon only: the
  lens flare and the spotlight still drew their scaled buffers inside
  `Render`, 4 offscreen clears and draws after the first composite on a frame
  with every layer at 0.5 (counted by hooking `glDrawArrays` /
  `glClearBufferfv`). Both now draw in `RenderOffscreen` and blit in `Render`
  (a `FrameState` between them, and a `Render` with no `RenderOffscreen`
  before it runs that phase itself, as the neon's does): 0 such draws on every
  scene, and 48 captures - flare, spotlight and all-layer scenes at scales
  1.0-0.25, an odd viewport, a host scissor, a reordered stack, a host calling
  `Render` directly, a rotating hue - byte-identical to the build before.
  The spotlight's screen blend, which all three of its draws take, is set in
  one place (`UseScreenBlend`). Plus the stale names it found in comments and
  docs (`GetSubRegion`, `mScaledBuffer` for the neon's buffer).
- **Open.** No renderer but the debug view reads the buffer yet. The buffer is
  the viewport's size whatever the rect's, so a window drag reallocates it on
  every frame, and at scale 1.0 a 4K viewport holds 66 MB of RGBA16F. A reader
  that has to process the buffer offscreen (a blur, a downsample) has no slot
  for it: in its own `RenderOffscreen` it sees this frame's buffer only when
  it sits after the neon in the order, and in its `Render` it splits the
  caller's run. The four figures should be re-rendered on the M2 with the rest
  at the next full `neon-guide-figures` run.

## 2026-10-15 - V27: the spotlight's dark tail below scale 1.0

Uncommitted. AMD Radeon Pro 5300M. V27 in
[`review-findings.md`](review-findings.md) has the detail.

- **Reported.** The spotlight "is not good" at a low `resolutionScale`.
- **Cause.** The neon's R7 again, on the spotlight's own reduced buffer: it was
  RGBA8 and dithered per texel, so the bilinear blit read a tail already
  rounded to one-level plateaus and averaged the noise into texel-wide mottle -
  blocky, diamond-shaped contours at 0.25 and 0.125 where 1.0 is smooth.
- **What changed.** The buffer is RGBA16F (RGBA8 fallback, which keeps the
  in-pass dither); `spotlight.frag` writes it undithered (`uDither`); a new
  `spotlight-blit.frag` dithers once per destination pixel after the read, with
  the noise shared through a new chunk, `spotlight-dither.glsl`.
  `blit.frag` is now the lens flare's alone.
- **Measured.** Tail pixels off 1.0 by over half a level after a 9 x 9 box
  filter, at 0.25: 3,586 -> 49 (wide soft lamp), 6,472 -> 0 (five-lamp fan).
  1.0 byte-identical. Scaled frames 1.04-1.08x at 1080p; the first blit cost
  1.26-1.33x, almost all of it from reading `gl_FragCoord` in a full-viewport
  pass and writing noise over empty pixels - both removed. Buffer memory
  doubles (4.1 MB at 1080p and 0.5).
- **Open.** Unchanged by this and inherent to the scale: small bright features
  (a narrow beam's head) are under-sampled - 24 levels off at 0.25. Only
  measured on the AMD; the device should get one look at 0.25 over a dark
  background.

## 2026-10-13 - Stale documents cleared; V16 re-measured on the M2 Pro

`abd2ece` for the documents. Apple M2 Pro.

- **Documents.** Six records of finished work removed (`architecture-design.md`,
  `multiple-arcs-design.md` and the four comparison documents, with their
  images); four of them were already recorded as removed on 2026-10-09 but
  never deleted. The pages that still described the direct path at 1.0, the
  inline gather, the "optimized" renderers or a field that refuses segments
  were brought up to the code: `emission-prepass.md`'s pass table, the
  guide's Parts 2.1, 5.5, 6 and 7.4, the three neon pages, `implementation.md`.
  `CLAUDE.md` had lost the droplets, lens flare and debug renderers' entries
  in `2bfd098`; restored.
- **V16 re-measured.** Not fixed, but explained. Steady frames are within 1
  level since V24 (`f54372a`, bisected); frames that shade the ring directly -
  every frame of an animation - still read up to 5 levels off at the corners
  (8 at `cornerRadius` 120). The cause is the ring's mesh, which moves with the
  scale: pinning `GetRingWidth` makes the reduced scales match 1.0 exactly.
  Addendum 2 of V16 in [`review-findings.md`](review-findings.md) has the
  numbers and two candidate fixes; neither is built.

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

`d07140f`, on top of V25. AMD Radeon Pro 5300M. V26 in
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

`4b8e699` (the header comments in `f809b8f`), on top of `fb6f06f`. AMD Radeon
Pro 5300M. V25 in
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
built in `498ed6f`, its documents in `fb6f06f`, on top of `14ef6c7`. AMD Radeon
Pro 5300M; I59-I62 in
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

`f54372a`, on top of `063603f`. Measured on an AMD Radeon Pro 5300M. V24 in
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

`063603f`, with the two entries below. Apple M2 Pro.

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

`063603f`, on top of V23. Measured on an Apple M2 Pro. R7 in
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

`063603f`, on top of `457ec75`. Measured on an Apple M2 Pro. V23 in
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
