# Progress log

Dated entries, newest first: what changed, what was measured, what was decided,
and what is open. Each entry names the commits it covers; the headers under
`lib/include/` stay the source of truth, and the topic documents linked here
hold the detail.

## 2026-10-17 - Step 4 of the shared-layer plan: tools and docs

- `partition` declares a host read of the ring atlas on every other config, so
  pass 2c is the atlas's copy there: 1000 configs, the ring copied in 477, no
  overlap, no gap. It fails if no ring was copied.
- `docs/neon-shared-buffer.md` rewritten around what was built, with
  `current.svg`; `per-frame.svg` gains the measured row.
- Not regenerated: `neon-guide-figures` (still frames moved by up to 1 level
  with the ring atlas; the guide's figures would churn for no visible change).

## 2026-10-17 - Step 3 of the shared-layer plan: the shared buffer assembled

- **Pass 1s no longer draws the neon a second time.** It draws the fill at the
  shared scale, then the light ASSEMBLED from the two parts
  (`neon-assemble.frag`): each texel the screen's light, read through
  `neonLight`, averaged over the texel's square of caller pixels. Gone: the
  shared ring and blit programs and meshes, `uDestScale` in `neon.frag`,
  `uDither` in `neon-blit.frag`, `drawBlit`, the destination-scale variants of
  `GetRingWidth` / `GetRingExtent` / `GetLitExtent` / `GetRingBlitPair`. The
  buffer covers only the region the light and the fill reach
  (`CompositeRegion`), not the whole viewport.
- **Verified** by the new `neon-scale-check shared`: twelve scenes, still and
  hue, at shared scale 1 without a fill, with one and with a whole-viewport
  fill, and at 0.5 against the screen averaged 2 x 2 - max 1 level everywhere,
  100% of lit pixels within 1. The old re-shaded buffer was up to 82 levels
  off on `hairline` at 0.5. `layer-check`: only the `shared/` scenes moved
  (their buffer views, and `+host-read`'s first frame, whose ring now goes
  through the atlas - within 1 level); every other frame identical. `check`,
  `partition` and `parts` pass.
- `PassRecorder` files the assemble pass and the shared buffer's fill as `P1S`.

## 2026-10-17 - Step 2 of the shared-layer plan: the parts, shared

- **`NeonRenderer.Ring` is PUBLIC** (`RING_KEY`, contract beside it): the ring
  atlas, the second part the screen is drawn from. A reader declares it, so it
  is drawn on moving frames too.
- **`neon-light.glsl`** (`ShaderSource::NEON_LIGHT_GLSL` for a host):
  `neonLight(fragPx, px)` rebuilds the light from the glow and the ring atlas -
  the ring's own pixels from the atlas (`neon-ring.glsl`'s `neonRingTexel`,
  now the one copy `neon-field.frag` uses too), every other pixel from the
  glow with the blit's edge mask. `neonEdgeMaskAt` / `sdRoundBoxPixelWidth`
  take the pixel width as an argument, so the mask can be evaluated off the
  fragment's own pixel. `NeonRenderer::GetSharedLight` / `UploadSharedLight`
  carry the rest (textures, the glow's placement, the ring's boxes and
  packing, the edge mask).
- **Verified** by the new `neon-scale-check parts`: twelve scenes x three
  scales x still and hue, a host rebuilding the light from the parts against
  the screen - max 1 level everywhere (the screen's dither), 100% of lit
  pixels within 1. With nothing reading the parts, `layer-check` is
  byte-identical to step 1 (all 300 frames), `check` and `partition` pass.

## 2026-10-17 - Step 1 of the shared-layer plan: the ring atlas

[`neon-shared-layer-plan.md`](neon-shared-layer-plan.md) step 1: the edge ring
drawn into a texture (pass 1t, `mRingAtlas`, `NeonRenderer.Ring`, DEBUG for
now), packed as the ring field is, and copied to the screen by pass 2c
(`mRingCopyShader`, `neon-field.frag` with `NEON_RING_COPY`).

**First version: the atlas on every frame - failed the step's gate.** Apple M2
Pro, Release, 1080p, the twelve scenes, medians of four interleaved rounds,
branch (`c08639a`) / atlas, above 1 = atlas faster:

| mode | 1.0 | 0.5 | 0.25 |
| ---- | --- | --- | ---- |
| still | 1.04 | 1.05 | 1.10 |
| hue | 0.91 | 0.88 | 0.86 |
| intensity | 0.87 | 0.77 | 0.74 |
| arc-wipe | 0.95 | 0.94 | 0.94 |
| segment-travel | 0.95 | 0.96 | 0.96 |

The atlas's clear, four strip draws and the copy cost 0.01-0.03 ms on every
moving frame, for a ring no later frame copied. The plan said to stop if
moving frames got slower by more than a few percent.

**Kept: the atlas only where it is copied again or read.** Drawn on the first
frame nothing moved (every still frame after copies it) and on any frame
something reads `NeonRenderer.Ring`; a moving frame nobody reads draws the
ring on the screen as before. Same measurement:

| mode | 1.0 | 0.5 | 0.25 |
| ---- | --- | --- | ---- |
| still | 1.07 (0.87-1.31) | 1.04 | 1.05 |
| hue | 1.00 | 1.00 | 1.00 |
| intensity | 1.00 | 1.00 | 1.00 |
| arc-wipe | 1.00 | 1.00 | 1.00 |
| segment-travel | 1.00 | 1.00 | 1.00 |

The first frame is about 1.4 ms slower (35 -> 36.4 ms): the copy's program.
The first version's numbers stay the price of a READER of the ring on a moving
frame (step 2): 0.01-0.03 ms.

- **Pixels.** The cover set (every frame moves): byte-identical to the branch.
  The page's still frames: within 1 level (the ring shaded rather than
  composited from its field, through half float). `check` and `partition`
  pass. `layer-check`: 144 of 300 frames differ, all still neon frames; +0.6 MB
  of textures where the atlas is held.
- **Tools.** `PassRecorder` files pass 1t as `P1T`, so `partition` and the
  guide's figures still find the screen's ring (the copy, `P2C`) and pass 1b.

## 2026-10-17 - The shared buffer's scale-1 copy: timed, and removed

At `compositeScale` 1 the main frame used to be one copy of the shared buffer
(pass 2s) instead of being drawn directly - "the layer drawn once, not twice".
It had grown a dozen special cases (its own program and mesh, pass 1r skipped
and its field released, the RGBA8 exclusion, a first-draw stall on the way
out) and had never been timed. Timed now, on an Apple M2 Pro, Release, 1920 x
1080, `resolutionScale` 0.5, the twelve `neon-scale-check` scenes, medians of
three rounds of `neon-scale-check time` (5 runs of 40 frames, best run):

- V0: no shared buffer.
- V1: `compositeScale` 1, the main frame the copy.
- V2: `compositeScale` 1 read by the host, the main frame drawn directly (the
  copy disabled in a scratch build) - what the product costs without the copy.

| mode | V0 median | V1 / V0 | V1 / V2 (copy vs direct) |
| ---- | --------- | ------- | ------------------------ |
| still | 0.062 ms | 0.78 (0.55-0.98) | 0.74 (0.49-1.01) |
| hue | 0.146 ms | 1.96 (1.28-2.64) | 1.15 (0.95-1.33) |
| intensity | 0.086 ms | 2.56 (1.51-3.67) | 1.18 (0.92-1.42) |
| arc-wipe | 0.376 ms | 1.26 (1.07-1.45) | 0.91 (0.68-1.07) |
| segment-travel | 0.459 ms | 1.22 (1.05-1.40) | 0.93 (0.73-1.08) |

Geomeans over the scenes, range in brackets. The copy wins where the direct
ring is shaded from scratch every frame (still frames reuse the buffer and
skip the ring; config-animated frames shade the ring once instead of twice)
and loses where the direct ring keeps its field (hue, intensity) - the copy
reads a full-resolution half-float buffer over the band. Mixed, and small in
absolute terms (-0.02 ms still, +0.05 ms hue at 1080p), against a dozen
special cases and a 16.6 MB buffer held whether anything read it or not.

**Decided: removed.** The main frame is drawn directly at every scale; the
shared buffer is a product only, made while something reads it, at 1 as at
any other scale. Gone: `mFramePlan.mainFromComposite`, `copyPlanned`,
`renderCompositeCopyPass`, `mCopyShader` and `NEON_BLIT_COPY`, the copy mesh
and its boxes, the ring field's release while copying. The main frame is now
byte-identical with the buffer on at every scale, 1 included.

- `layer-check`: every frame identical to the report before, but
  `shared/default@1`, which drew the copy and now hashes as
  `neon/default@0.5`. The copy-only scenes (`+opaque-only`, `+fill-only`) are
  gone; the scale-1 hue scene now reads the buffer (`+hue+view`). 300 frames
  over 75 scenes. `partition` passes.
- Not measured on the AMD Radeon Pro 5300M; on this evidence the decision
  would need the AMD to show a clear win to be revisited.

## 2026-10-16 - The shared neon buffer (`NeonRenderer.Composite`): built, in progress

On `design_system_sharing_resources_between_renderers`, uncommitted, on top of
the last commit. AMD Radeon Pro 5300M, Debug build. The whole neon layer -
fill, glow, line, the one-sided cut and the cutoffs - drawn a second time at a
scale `s` into an offscreen buffer any layer or the host can read.

- **Built.**
  - `NeonConfig::compositeScale`: 0 (the default) makes nothing; otherwise
    clamped to [`resolutionScale`, 1], so the buffer is never coarser than
    the reduced buffer it copies its glow from.
  - `NeonRenderer::COMPOSITE_KEY` (`NeonRenderer.Composite`), a PUBLIC record
    with its contract beside the key in `neon-renderer.h`: premultiplied,
    undithered, RGBA16F (RGBA8 fallback), over the whole viewport, placed at
    origin 0, size texels / s.
  - Pass 1s, in phase 1 after pass 1r: the main frame's three passes again -
    fill, blit, ring - at `s`, into the shared buffer, with program objects of
    their own (`mCompositeFillShader`, `mCompositeBlitShader`,
    `mCompositeRingShader`; links only, from the shader cache). The glow is
    the reduced buffer copied, not shaded again; only the ring band is shaded
    again, at `s`. Its ring and blit meshes come from one set of floats (the
    partition at `s`) and are clamped to the main pair's boxes, so the main
    frame's buffers do not grow with it.
  - Reused on a frame where nothing moved (`mOffscreen.reusable`); released
    when the scale goes back to 0 or the layer is disabled.
  - At `s` 1 the main frame is pass 2s, one dithered copy of the buffer, in
    place of the fill, the blit and the ring.
  - Two shader switches, byte-identical on every existing pass: `uDestScale`
    (the masks antialias over one pixel of the grid the output is seen at:
    `uResolutionScale / uDestScale`, an exact r / 1 on every pass but the
    shared ring) and `uDither` on the ring and the blit (0 into the shared
    buffer).
- **Verified.**
  - With the buffer off: all 256 `layer-check` frames identical to
    `bcf51e2`'s report, memory and allocation lines identical, `check`'s
    table unchanged, `partition` passes.
  - A probe over the twelve `neon-scale-check` scenes:
    - Turning the buffer on at 0.5 leaves the main frame byte-identical in
      all twelve.
    - The buffer at 0.5 against the main frame box-filtered 2 x 2: within 1
      level from 12 px out of the edge in all twelve - no seam. Nearer, the
      line and steep ramps are drawn at 0.5 rather than shrunk: up to 82
      levels on `hairline` (its 1 px line drawn 2 shared px wide, the
      filament's Nyquist floor), 13 on `bounded_band`'s cutoff ramps, 3-6 on
      the rest. Side-by-side crops show the same picture at half resolution.
    - At 1: the main frame drawn as the copy against the direct one, and the
      buffer against the direct frame, within 1 level (2 on 3-4 pixels of
      `hairline`).
    - A still frame reuses the buffer (its version holds); set back to 0, it
      is released and no longer current.
- **Review fixes (xhigh review of `01aca60`).**
  - `debug.opaqueOnly` drew the previous frame's shared buffer at scale 1:
    its early return left `mFramePlan.mainFromComposite` set, so the fill was
    skipped and the stale copy drawn. The flag is now reset with `glowReady`.
  - The shared pair is clamped to the main pair's boxes, whose lit band
    leaves 2 px past the masks' ends; the shared ramps reach 0.71 / s px on a
    corner's diagonal, so below s 0.355 the one-sided cut's back ramp (and
    below ~0.28 a cutoff's, below ~0.24 the fill's past its 3 px mesh margin)
    was cut off. The old comment's "down to 0.2" ignored the diagonal.
    `GetCompositeScale` now floors s at `NeonConfig::COMPOSITE_MIN_SCALE`
    (0.4).
  - At scale 1 in the RGBA8 fallback the copy dithered 8-bit plateaus (R7's
    contours); the main frame is copied only from a half-float buffer now.
  - Gated since: `layer-check` has eleven `shared/` scenes (see its README).
    Within one report, `shared/default@0.5` hashes as `neon/default@0.5`,
    `+opaque-only`'s frames 2-3 as `+fill-only`'s, and `outside@0.25` as
    `outside@0.4`. With the opaqueOnly fix undone, `+opaque-only` keeps the
    copy's hash on frames 2-3, so the scene catches it. The existing 256
    frames are identical to the report before the fixes; `partition` passes.
    Not covered: the RGBA8 fallback (nothing forces it), and whether the
    ramps at 0.4 are unclipped - the hashes pin the picture, they do not
    measure it.
- **Second review (the C++ as it stood after the fixes), all thirteen
  findings applied.**
  - Demand: pass 1s runs only while something reads `COMPOSITE_KEY`
    (`IsBufferWanted`) or the scale is 1. A C++ host declares its read with
    the new `EdgeLightingEffect::AddHostRead` / `RemoveHostRead` (a read with
    no reader, RENDER phase). An unread buffer is released by `Update` after
    `COMPOSITE_RELEASE_SECONDS`, and never reused across a frame it was not
    made (`mComposite.cache`, its one reuse key, now with the scale in it).
  - Scale 1: pass 1r is no longer decided or baked on a frame the main frame
    is the copy; the copy is its own program (`mCopyShader`, `neon-blit.frag`
    with `NEON_BLIT_COPY` - no mask, no fabricated `EdgeMask`) and draws only
    the blit's and the ring's meshes when there is no fill (the viewport
    with one).
  - One construction: `GetRingBlitPair(config, scale, destScale, ...)` builds
    both the main pair (destScale 1) and the shared one, replacing
    `GetCompositeRingExtent` and the hand copy; `GetRingWidth` and
    `GetRingExtent` take the destination scale. `setupCompositeGeometry`
    builds the shared meshes, so a change to `compositeScale` alone rebuilds
    those and nothing else - no field invalidation, no `setupGeometry`, the
    main frame's buffers kept.
  - Cleanups: neon.frag's mask scaling goes through `ScaleEdgeMask` too (a
    disabled cutoff now stays at its sentinel at every scale; it used to be
    multiplied, and read as a live cutoff 250 000 px away below 0.5);
    `drawFill` takes its program's builder rather than testing which program
    it was handed; pass 1s is declared and defined after pass 1r, 2s before
    2b; pass 1s hands blending back off; the demo prints the drawn scale.
- **Third review (`a6edc2c`, `de82aaf`), all eight findings applied.**
  - A host's read of a `DEBUG` buffer is logged like a layer's ("the host
    reads ..."); it used to pass silently, since host reads have no reader.
  - `NeonRenderer::GetCompositeScale(config)` is public, the one copy of the
    drawn-scale rule; both demos ask it instead of their own clamps.
  - `mComposite.wanted` is cleared at the top of `RenderOffscreen`, so a
    fill-only or disabled frame counts toward the idle release; the ring's
    field is released after `COMPOSITE_RELEASE_SECONDS` of copy frames, with
    its schedule. The shared buffer's delay is its own constant now.
  - The copy with a shaped fill draws one box round the fill's band and the
    blit (`mComposite.copyMesh`, from a scratch vector so an intensity
    animation allocates nothing more); the viewport only for a fill that
    covers it.
  - The contract no longer says the main frame IS the copy at 1 on every
    driver.
  - Not fixed, documented instead (beside `copyPlanned`): the direct path's
    programs are not drawn while the copy stands in, so the first frame
    back on it pays their first-draw stall (~125 ms on the AMD). Warming
    them would mean drawing on the caller's framebuffer on a copy frame.
  - New scene `shared/default@0.5+step-to-1+view` (0.5, 1, 0.5, 0.5 under a
    reader): every frame hashes as the matching still scene's. All 304
    earlier frames identical, allocation and memory lines unchanged but the
    copy mesh's 48 bytes; `partition` passes.
- **Not done.**
  - The C ABI: skipped by request. No `el_*` call sets the scale or exports
    the texture yet.
  - A control in the C++ demo.
  - Timing: what pass 1s costs on moving frames, and whether the scale-1
    copy beats drawing directly (it trades the ring field's cached composite
    for a full-res ring shaded on every frame the buffer is redrawn).
  - The probe is in scratch, not a repository gate; no `layer-check` scene
    turns the buffer on yet.
  - The buffer covers the whole viewport; sizing it to the neon's lit box
    would save memory (4.1 MB at 0.5, 16.6 at 1, at 1080p).

## 2026-10-16 - The branch against main: speed, startup, memory

The working tree (`bcf51e2` plus the third review's fixes) against `main`
(`7543a50`). AMD Radeon Pro 5300M, Release, 1920 x 1080, 12 rounds with the
build order alternating, medians of each figure's per-round minimum (5 runs
of 40 frames). The frames are byte-identical (previous entry).

- **Neon frames** (`neon-scale-check time`, six scenes x scales 1, 0.5,
  0.25), main / branch, above 1 = branch faster:

  | mode | geomean | range |
  | ---- | ------- | ----- |
  | still | 0.975 | 0.83-1.55 |
  | hue | 0.969 | 0.86-1.02 |
  | intensity | 0.985 | 0.86-1.10 |
  | arc-wipe | 0.997 | 0.96-1.03 |
  | segment-travel | 1.008 | 0.94-1.09 |

  The moving frames (0.5-2.2 ms) are flat. The still and hue frames
  (0.05-0.6 ms) lean a few percent slower, but their per-round values
  overlap almost entirely (the branch slower in ~70% of pairs), and still
  frames under 0.15 ms are bimodal here. The all-layer harness below reads
  the same neon frames the other way, 1.03-1.04. So the difference is under
  the noise, either way.
- **All layers** (five renderers, a larger rect): geomean 1.026, range
  1.00-1.10. The five-lamp fan at 0.5 is the one clear move, 0.235 -> 0.214
  ms (1.10x), and steadier: 0.20-0.23 across rounds against 0.20-0.27.
  Everything else is 1.00-1.04.
- **Startup** (`layer-check startup`, 5 rounds, the buffer view left out of
  the branch's first frame so both build the same programs): 36 shader
  compiles -> 21 (`neon.vert` 15 -> 1, `neon.frag` 2 -> 1). That saves almost
  no time: compiles 76 -> 75 ms, since `neon.vert` is tiny and this driver
  finishes a compile on the first draw. Initialize 24.1 -> 22.1 ms. Links
  are the same 18 but take 36.5 -> 41.8 ms. First frame 882 -> 896 ms, ranges
  862-931 and 887-918: no change.
- **Memory**: every scene holds 1 KB less texture and 144 bytes less buffer;
  allocations per frame are identical.
- **Not measured**: a tiler. The two-phase frame is for one, and only the M2
  or the target device can show what it saves. This GPU can only show that
  nothing got slower.

## 2026-10-16 - Third review: the branch against main

On `design_system_sharing_resources_between_renderers`, uncommitted, on top of
`bcf51e2`. A third deep review filed 7 findings; all are fixed. AMD Radeon Pro
5300M.

- **The whole branch moves no pixel.** `layer-check` now builds standalone
  against `main`, which has no buffer registry: it leaves out what a registry
  would list, its report says `# registry: none`, and `diff` skips the other
  report's `view/` frames. All 208 frames `main` renders are byte-identical to
  the branch's. Every scene holds 1 KB less texture (the debug layer's own
  colour ring, gone since S5b) and 144 bytes less buffer (the quads shared
  since S5a, the debug layer's included now); allocations are the same.
- **Fixed.**
  - A renderer shared with another effect that fails to initialise here -
    late, or in `Initialize` - goes back to the context it came from, not to
    none: that effect still draws it.
  - Registry slots live in a `std::deque`, so a record pointer from `Find`
    stays valid when another renderer registers later.
  - The debug layer draws its markers and buffer view with the shared
    fullscreen quad; `renderBlitPass` lost a parameter it no longer read.
  - The onboarding guide's renderer contract (Part 2.3), its shader
    assembly listing (now with `neon-mask.glsl`), and every comment that
    still named `WithDefine`.
- **Measured.** All 256 `layer-check` frames identical to `bcf51e2`'s report;
  `check`'s table unchanged; `partition` passes. Under ASan the probe's 29
  checks pass, among them the shared renderer handed back on both failure
  paths, and a `Find` pointer surviving 64 later registrations.

## 2026-10-16 - Review fixes on the sharing branch

On `design_system_sharing_resources_between_renderers`. Two deep reviews of
`main...HEAD` filed 15 findings, then 13 more against the fixed tree; all 28
are fixed. The first round is committed as `d1cd962` (its blit timing, below,
finished after that commit); the second is uncommitted on top of it. AMD
Radeon Pro 5300M.

- **Lifetimes.**
  - A renderer that fails `Initialize` leaves the effect's context with it,
    so a host that keeps it no longer pins its keys in the registry.
  - `AddRenderer` refuses a renderer already registered. Added twice, it ran
    its offscreen half three times a frame, once in the on-screen phase.
  - The effect's destructor closes the registry and clears the read list. A
    renderer that outlives the effect can still draw alone, but nothing it
    marks reads as current any more.
- **`Render` is non-virtual.** It runs `RenderOffscreen` when no `Prepare`
  came this frame, then the protected `RenderOnscreen` each renderer
  overrides. `EnsurePrepared` is gone, so an override can no longer skip
  phase 1 or leave the flag set for the next frame.
- **The edge mask** (`neon-mask.glsl`) is split into the cut's half and the
  cutoffs' half (`neonEdgeSide`, `neonEdgeCutoffs`). `neonEdgeMask` computes
  the cutoffs only where one binds, so a one-sided glow without cutoffs pays
  the cut alone again, as the blit did before S6a. `neon-field.frag` takes
  the mask chunk only in its `NEON_FIELD_RING` variant.
- **Tools.** `layer-check` counts allocations per thread (the counter was a
  data race), and its record ids no longer throw on a keyed record with an
  attachment above 0.
- **Cleanups.** One rect-centre formula (`GeometryUtils::GetRectCenter`) for
  the neon, the fill, the droplets and the debug overlays. One private
  `addOwned` behind the three `RegisterBuffer` overloads. Braced discards.
  Stale docs on the debug view, the layer order and the PUBLIC glow record.
- **The second review.**
  - The shader cache matched a stage by the caller's pointer and
    `memcmp`'d it later, so a host renderer building from a temporary string
    read freed memory. Under ASan, the committed cache hits a
    heap-use-after-free on exactly that. It now keys on the text's length and
    a 64-bit hash: no pointer kept, and no copy (the sources are ~700 KB).
    `layer-check startup` counts the same 22 compiles and 19 links as
    `b0c05e3`.
  - `Prepare` is private, the effect's alone. Public, it let a caller prepare
    a frame, skip its `Render`, and have the next lone `Render` composite
    stale buffers.
  - A record's `version` comes from one registry-wide counter, so a key
    registered again never repeats a value a consumer cached.
    `IsBufferWanted` is false for a buffer whose key the registry refused.
  - The read diagnostics warn about a DEBUG read once per reader, even when
    the debug view read the key first. They re-check on generation counters
    (renderer list, registry), not on addresses a new renderer can reuse.
  - The mask: the blit skips a cutoff side that does not bind (one cutoff
    no longer computes both distances, as before S6a). `neonEdgeSide` leaves
    the cutoffs neutral rather than undefined. The ring's scaled upload no
    longer builds a centre for a 0 px viewport.
  - Docs that still placed marks in `Render`, or described layer-order
    reads: `MarkBufferCurrent`, the debug layer, the spotlight and flare
    comments, `implementation.md`'s renderer contract.
- **Found while probing.** `BaseRenderer`'s inline default constructor made
  a host renderer that includes only `base-renderer.h` fail to compile, since
  S5a: it destroys a `unique_ptr<VertexArray>` of an incomplete type. The
  constructor is now defined in the .cpp.
- **Measured.**
  - All 256 `layer-check` frames are identical to S6b, and so are the
    allocation and memory lines. `check` and `partition` pass, and `check`'s
    table is unchanged.
  - Under ASan, a probe of 20 checks passes: the lifetime and phase cases,
    the cache with freed sources, `IsBufferWanted` on a refused key,
    versions across a re-registration, and the per-reader DEBUG warning. The
    glow product reader is still within 1 level on four configs.
  - After the second round, the 256 frames are identical to `d1cd962`'s
    report (and so to S6b's), as are the allocation and memory lines;
    `check`'s table is unchanged and `partition` passes.
  - The one-sided blit, timed against S5b (`02bfedc`, before S6a) and S6b
    (`b0c05e3`), with the first round's mask split.
    Release, 1920 x 1080, 12 rounds with the build order rotated, medians.
    The blit's own timer (`--passes`, a ratio on this GPU):

    | scene | scale | S5b | S6b | fixed |
    | ----- | ----- | --- | ---- | ----- |
    | `card_outside` | 0.5 | 0.348 | 0.459 | 0.349 |
    | `card_outside` | 0.25 | 0.341 | 0.457 | 0.341 |
    | `glow_inside` | 0.5 | 0.139 | 0.166 | 0.137 |
    | `glow_inside` | 0.25 | 0.138 | 0.168 | 0.137 |

    S6a had made the one-sided blit 1.2-1.3x slower; the split puts it back
    within 1% of S5b. At 1.0 the gap was smaller (1.07x on `card_outside`,
    none on `glow_inside`) and is gone too. The whole `card_outside` frame
    went from 0.200 to 0.167 ms at 0.5 (wall-clock). `default` and
    `bounded_band`, which have no mask or both cutoffs, did not move beyond
    noise.
- **Open.**
  - The single-cutoff blit's saving is unmeasured: no gate scene has one
    cutoff.
  - Nothing measured on the M2 or the target device.

## 2026-10-16 - Sharing plan S6c: the pilot, built and dropped

On `design_system_sharing_resources_between_renderers`, never committed.
AMD Radeon Pro 5300M. Section 7.10 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.** `DropletsConfig::glowLight` lit the drops with the neon's glow
  product, through a second droplets program with the neon's mask chunk
  injected.
- **Measured.**
  - Default frames identical to S6b; +5-12 us a frame with it on.
  - At 1, only 0.5% of the frame changed - drops 6-12 px across. Up to 72
    levels per pixel, but not visible at 1:1.
  - Over the tube it partly cancelled, because the light was added with
    coverage. And the product has no filament, so a drop on the line took
    the halo's colour.
- **Decided (owner).** Dropped. The code was removed before any commit. The
  glow product and its contract (S6b) stay, with no reader in the library.
- **Open.** S6 is done. S7, the render-target pool, waits for a temporary
  target - none exists yet. Nothing in S0-S6 is measured on the M2 or the
  target device.

## 2026-10-16 - Sharing plan S6b: the glow as a public product

Committed as `37b9716`, on `design_system_sharing_resources_between_renderers`, measured
against S6a (`b9a2781`). AMD Radeon Pro 5300M, Debug build. Section 7.9 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - `NeonRenderer.Glow` (`NeonRenderer::GLOW_KEY`) publishes the reduced
    buffer a second time, as a PUBLIC product. Its contract is in
    `neon-renderer.h` and `CLAUDE.md`: premultiplied, tone-mapped halo and
    bloom, before the edge mask, without the filament, over the placed box.
  - `NeonRenderer::GetEdgeMask` / `UploadEdgeMask` / `GetRectCenter` are the
    CPU side a reader needs, and the blit and the ring's scaled upload now
    take the same numbers from them.
  - `BaseRenderer::RegisterBuffer(buffer, key, visibility)` registers a
    product under its own key.
- **Measured.**
  - None of the 252 `layer-check` frames differs from S6a; one new scene is
    the view of the product.
  - `check` and `partition` pass; memory and allocations unchanged.
  - A host renderer reading the product with `neonEdgeMask` reproduced the
    neon's frame within 1 level (the blit's dither) away from the edge ring,
    on four configs including one-sided glows and cutoffs.
- **Next.** S6c, the pilot: droplets lit by the glow, behind a
  `DropletsConfig` field whose default leaves the frame identical.

## 2026-10-16 - Sharing plan S6a: one copy of the glow's edge mask

Committed as `b9a2781`, on `design_system_sharing_resources_between_renderers`, measured
against S5b (`02bfedc`). AMD Radeon Pro 5300M, Debug build. Section 7.8 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - `lib/shaders/neon-mask.glsl` holds the one-sided cut and the cutoffs as
    coverage. Three copies used to carry them by hand: the blit, the edge
    ring in `neon.frag`, and the ring field's composite.
  - It is injected into all three after `neon-sdf.glsl`.
  - The blit is one `neonEdgeMask` call. The ring and the field composite
    multiply its three factors one at a time, as before, so no float product
    reassociates.
  - It is the mask a reader of the glow product (S6b) will apply.
- **Measured.**
  - All 252 `layer-check` frames identical to S5b.
  - `check` and `partition` pass.
  - No "uniform not found" line from any program; memory and allocations
    unchanged.
- **Next.** S6b, `NeonRenderer.Glow` as a PUBLIC product with its contract.
  Then S6c, droplets lit by the glow.

## 2026-10-16 - Sharing plan S5b: the debug strip reads the neon's live ring

Committed as `02bfedc`, on `design_system_sharing_resources_between_renderers`, measured
against S5a (`186bc48`). AMD Radeon Pro 5300M, Debug build. Section 7.7 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - The registry can list a LUT.
  - The neon registers its colour ring as `NeonRenderer.GradientRing` and
    marks it current whenever the glow is on.
  - The debug layer's strip reads that ring through the registry, and
    declares the read.
  - The debug layer's own ring - bake, tick and snap-on-re-show - is gone.
- **Measured.**
  - None of the 248 `layer-check` frames differs from S5a. One new scene is
    the view of the ring.
  - 1 KB of textures less per scene; allocations unchanged.
  - `check` and `partition` pass.
  - The intended change: a strip switched on mid-fade now matches one
    visible throughout, byte for byte. Before, it snapped to the new stops:
    17,280 channels off, up to 115 levels.
- **Open.**
  - S5 is done.
  - Next is S6: the neon's glow as a public product (P-a), the shared mask
    chunk, and droplets lit by the glow.

## 2026-10-16 - Sharing plan S5a: one fullscreen quad

Committed as `186bc48`, on `design_system_sharing_resources_between_renderers`, measured
against S4 (`7584bdb`). AMD Radeon Pro 5300M, Debug build. Section 7.6 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - The effect's `ResourceContext` holds one fullscreen NDC quad, with a
    single definition, `BuildFullscreenQuad`.
  - The neon, the flare and the spotlight draw it through
    `BaseRenderer::GetFullscreenQuad`, which falls back to a quad of the
    renderer's own without an effect.
  - Their three copies are gone.
- **Measured.**
  - All 248 `layer-check` frames identical to S4.
  - 96 bytes of buffers less in every scene; allocations unchanged.
  - `check` and `partition` pass.
  - Without an effect, the neon, flare and spotlight each draw
    byte-identically to the same renderer inside one.
- **Open.** S5b: the debug strip reading the neon's live gradient ring. That
  needs the registry's texture kind, which S2 deferred.

## 2026-10-16 - Sharing plan S4: read declarations

Committed as `7584bdb`, on `design_system_sharing_resources_between_renderers`, measured
against S3 (`d9e1d95`). AMD Radeon Pro 5300M, Debug build. Section 7.5 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - Every renderer declares the buffers it will bind this frame
    (`CollectReads`), into the context's `ReadList`.
  - Producers can ask `IsBufferWanted`.
  - The effect calls `Prepare` in `PrepareSchedule`'s order: compositing
    order, except that a `Prepare` reading another layer's product runs after
    it. A cycle is refused, logged, and drawn in compositing order.
  - The debug view declares its selection.
  - The schedule logs a read of a key nothing registered, an overlong key,
    and a `DEBUG` buffer read outside the debug layer - once per read.
- **Measured.**
  - All 248 `layer-check` frames identical to S3; memory and allocation
    counts unchanged; no log line in any of the 62 scenes.
  - `check` and `partition` pass.
  - A probe with two host renderers covered the order under each kind of
    read and a cycle refused and logged once over 6 animated frames. Each
    diagnostic logged exactly once.
- **Departures.**
  - The diagnostics log when a read first appears, not once per config
    change: an animation changes the config every frame.
  - Marks stay ungated by demand, against I70's note: a UI shows what is
    current.
- **Open.** Next is S5: the shared quad, and the debug strip reading the
  neon's live gradient ring.

## 2026-10-16 - Sharing plan S3: the two-phase frame

Committed as `d9e1d95`, on `design_system_sharing_resources_between_renderers`, measured
against S2 (`d6d2175`). AMD Radeon Pro 5300M. Section 7.4 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - `EdgeLightingEffect::Render` calls `Prepare` on every renderer, then
    `Render` on every renderer.
  - `Prepare` is a non-virtual wrapper around the protected
    `RenderOffscreen`. `Render` calls `EnsurePrepared` first, so a renderer
    drawn alone still draws its whole frame.
  - The neon's passes 0 to 1r moved to `RenderOffscreen`; `mFramePlan`
    carries what passes 2a to 2r read.
  - The flare's and the spotlight's scaled draws moved there too. Their
    blits stayed in `Render` and set their own blend.
  - The neon's `Render` re-binds its segment and arc blocks.
  - Records go current in `Prepare`.
- **Measured.**
  - All 248 `layer-check` frames identical to S2.
  - `check` and `partition` pass; the ASan probe is clean.
  - No slower anywhere, Release at 1080p, S2/S3 geomean:
    - hue frames 0.990;
    - still frames 1.005, over 12 rounds;
    - flare, spotlight and all-layer frames 0.992.
  - Sub-0.1 ms still frames on the AMD are bimodal in either build, so they
    take 12 rounds, not 4. Four rounds read a false 0.56x on one figure.
- **Open.**
  - The point of the change is a tiler, which this GPU is not. The M2 and
    the target device are not measured.
  - Next is S4, read declarations.

## 2026-10-16 - Sharing plan S2: registry v2, and I64's open case and I65-I73 closed

Committed as `d6d2175`, on `design_system_sharing_resources_between_renderers`, measured
against S1 (`b03a9c3`). AMD Radeon Pro 5300M, Debug build. Section 7.3 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **Built.**
  - The `BufferRegistry` moved into the `ResourceContext`.
  - Records are owned: registered once, from the producer's constructor
    (`RegisterBuffer`), removed by `~BaseRenderer`, and moved by
    `SetResourceContext`.
  - One string key per record (`NeonRenderer.Gather[1]`), unique per
    registry.
  - Each record is stamped per frame (`BeginFrame`, `MarkBufferCurrent`,
    `IsCurrent`), with a `version`.
  - Each record is `PUBLIC` or `DEBUG`; everything today is `DEBUG`.
  - `SetBufferRegistry` and `DebugConfig::bufferViewAttachment` are gone.
  - The debug view, both demos, the C ABI and `layer-check` are ported. The
    C ABI adds `el_effect_get_buffer_current`; every other export keeps its
    arity.
- **Closed.**
  - I64's open case: a kept renderer's records leave with it.
  - I65 (one key per buffer) and I66 (no mark after a failed bake).
  - I67 (protected names) and I68 (one key).
  - I69 (the doc) and I70 (no search per mark).
  - I71 (keys capped at `EL_BUFFER_NAME_CAPACITY`).
  - I72 (whole-scale panels) and I73 (more formats).
- **Measured.**
  - 236 of 248 frames identical to S1. The 12 that differ are three table
    panels whose scale I72 changed.
  - Those panels are texel-exact: 0 of 193,038 pixels off.
  - Allocations and memory unchanged in every scene.
  - Four lifetime paths are clean under AddressSanitizer.
  - `neon-scale-check check` and `partition` pass.
- **Departures from the plan.**
  - The effect's clears are removed rather than kept as a backstop: with owned
    records they would drop a kept renderer's.
  - The texture kind waits for S5.
- **Open.** Not measured on the M2 or the target device. Next is S3, the
  two-phase frame.

## 2026-10-15 - Sharing plan S0 and S1: the five-layer baseline, and one compile per shader stage

Committed as `7d3e953` (docs `b03a9c3`), on
`design_system_sharing_resources_between_renderers`, measured against
`a213d55`. Apple M2 Pro, Debug build. Section 7 of
[`resource-sharing-plan.md`](resource-sharing-plan.md) has the detail.

- **S0.** `tools/layer-check`, built with the other tools. `report` hashes
  248 frames over 62 scenes through all five renderers, with GL memory and
  per-frame heap allocations; `diff` fails on any changed frame; `startup`
  counts and times every compile. Two runs of one binary agree exactly.
- **S1.** A `ResourceContext` per effect, handed to every renderer, holding a
  `ShaderCache`. Every program is linked from cached `ShaderObject`s
  (`BaseRenderer::BuildProgram`), one program object per pass as before.
- **Measured.**
  - All 248 frames are identical to S0.
  - Compiles 38 -> 22: `neon.vert` once instead of 16 times, `neon.frag`
    once instead of twice.
  - The all-programs first frame is ~9 ms faster (116.6-119.9 -> 108.3-113.8
    ms, interleaved runs).
  - A forced `neon.vert` failure is compiled and logged once instead of 6
    times.
  - A renderer drawn without an effect compiles privately and draws
    identically.
  - `neon-scale-check check` and `partition` pass.
- **Found, then traced** (I75, in a separate session). The tool's
  allocation counts are mostly Apple's GL driver, which the replaced
  `operator new` also catches: 24 B per draw, 40 B per render pass, and a
  burst per command buffer. On still frames the count equals the draw count.
  The library allocates only on three neon config-change paths (intensity
  and segment animations); I75 proposes the fixes.
- **Open.** Not measured on the AMD 5300M or the target device. Next is S2,
  the registry.

## 2026-10-15 - A plan for sharing resources between renderers

Not built yet. Written on `design_system_sharing_resources_between_renderers`;
see [`resource-sharing-plan.md`](resource-sharing-plan.md).

- **Goals (owner).** Cross-layer effects, memory pooling, and one copy of
  common objects. One owner writes each resource; everyone else only reads it.
- **Proposed.** A `ResourceContext` per effect. It holds a registry of records
  that are owned, typed and stamped per frame, a two-phase frame
  (`Prepare` / `Render`), read declarations, a shader-object cache, one
  fullscreen quad, the neon's gradient ring shared with the debug layer, and a
  render-target pool. Steps S0-S7; S2 closes I65-I73 along the way.
- **Found while writing it.** No buffer the neon owns holds its picture: the
  reduced buffer has no filament, cut or cutoffs. So a cross-layer read needs
  a public product with a contract. And pooling saves nothing across today's
  layers under a two-phase frame.
- **Decided (owner, the same day).** Every recommendation taken:
  - the glow product as P-a, the reduced buffer with a shared mask chunk;
  - droplets lit by the glow as the pilot;
  - the flare and the spotlight kept out of the pool;
  - one string key;
  - the debug strip following the live ring;
  - `SetResourceContext`;
  - the shader cache built first (S1).

  Host interop stays out of scope.

## 2026-10-15 - I64: registry records no longer outlive their producers

Uncommitted, on `design_system_sharing_resources_between_renderers`. Apple M2
Pro. I64 in [`review-findings.md`](review-findings.md) has the detail.

- **What changed.** `~EdgeLightingEffect` clears the `BufferRegistry`. So does
  an `Initialize` that erases a renderer that failed. These are the only two
  ways a renderer leaves the effect. The `CLAUDE.md` sentence that said the
  effect never removes one is corrected, and so is the lifetime note on
  `PublishedBuffer`.
- **Measured.** A probe built under AddressSanitizer reproduces both paths as a
  heap-use-after-free in `Framebuffer::GetName`: 5 stale records after the
  effect is destroyed, 1 after the erase. With the fix, both leave 0 records
  and ASan reports nothing. `neon-scale-check check` and `partition` pass.
- **Open.** A renderer kept past the effect and drawn on its own still
  publishes into a registry nothing clears. I65 to I73 are unchanged.

## 2026-10-15 - The buffer registry and the debug buffer view; reviewed

`181acdd` on `design_system_sharing_resources_between_renderers`. AMD Radeon
Pro 5300M. I64 to I73 in [`review-findings.md`](review-findings.md) have the
review's detail.

- **What changed.** `EdgeLightingEffect` owns a `BufferRegistry`
  (`core/buffer-registry.h`), handed to every renderer it registers and
  cleared at the top of each `Render`. Each layer publishes READ-ONLY records
  of the offscreen buffers it drew or read - a pointer to its own
  `Framebuffer`, an attachment, and the box of the frame the buffer maps onto
  when it maps onto one - so the name, texture and size are the
  `Framebuffer`'s own, never copied. The neon publishes its emission table,
  coverage tables, gather (both attachments), field, reduced buffer and ring
  field; the lens flare and the spotlight their reduced buffers.
  `DebugConfig::bufferView` (+ `bufferViewAttachment`, `bufferViewChannel`,
  `bufferViewGain`) draws any of them one block per texel - in place, or as a
  lower-left panel for a table (`debug-buffer-view.frag`); C ABI and both demos
  follow. Nothing is lent for writing: the neon's buffers are caches, and a
  pool of transient ones was weighed and set aside (only the flare's and the
  spotlight's qualify, both absent at their default scale 1.0).
- **Measured.** Every published buffer, viewed, matches the texel under each
  pixel exactly (ten buffers, two scenes, panels included but for I72's
  boundary pixels); no heap allocation per frame with a view on;
  `neon-scale-check check` passes; `opaqueOnly` publishes nothing.
- **Reviewed - ten items open.** The one to fix first is I64: a record can
  outlive the `Framebuffer` it points at, reproduced as a SIGSEGV when a kept
  debug layer draws after its effect is destroyed. I65 (one name, two buffers)
  and I68 (a stale attachment) are reproduced too; I65 and I68 share a
  decision - what the view's key is - and the rest are small.

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
