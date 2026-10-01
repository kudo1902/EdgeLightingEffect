# Edge-ring implementation plan for the scaled neon path

The step-by-step plan for building the design in
[`neon-resolution-scale-proposal.md`](neon-resolution-scale-proposal.md): keep
the reduced-resolution glow, and re-shade a thin ring around the rect edge at
full resolution from the reduced pass's gather output. Read the proposal first
for why. This document is only how.

**Status: steps 0-2 done; step 1 committed (`546d2b7`), step 2 not yet. Step 3
is next.** Section 11 lists the decisions still open.

The work is six steps, each committed and checked on its own before the next
starts. Steps 1, 3 and 4 change no pixels at all, and step 2 changes only scenes
with an inside or outside cutoff below scale 1.0. So a regression shows up in
the step that caused it, against a byte-for-byte baseline, rather than at the
end.

| step | what | size | pixels change |
| ---- | ---- | ---- | ------------- |
| 0 | test harness and baseline | small | - |
| 1 | the spotlight and the flare get their own blit shader | small | none |
| 2 | inside/outside cutoffs move into the neon's blit | small | cutoff scenes below 1.0 only |
| 3 | `Framebuffer` gains extra colour attachments | small | none |
| 4 | the reduced pass writes the gather target | medium | none |
| 5 | the edge ring | large | every scene below 1.0 |
| 6 | docs | medium | - |

## 1. The pass schedule, before and after

Today, below scale 1.0:

| order | pass | target | geometry |
| ----- | ---- | ------ | -------- |
| 2a | opaque fill | caller's framebuffer | fill ring |
| 0 | emission pre-pass (when stale) | emission table | fullscreen quad |
| 1 | gather and shading | `mScaledBuffer` | glow quad, scaled |
| 2b | blit with the one-sided cut | caller's framebuffer | fullscreen quad |

After step 5:

| order | pass | target | geometry |
| ----- | ---- | ------ | -------- |
| 2a | opaque fill | caller's framebuffer | fill ring, unchanged |
| 0 | emission pre-pass (when stale) | emission table | unchanged |
| 1 | gather and shading | `mScaledBuffer`: colour + 1 or 2 gather attachments | glow quad, scaled |
| 2b | blit with the one-sided cut AND the cutoffs | caller's framebuffer | everything EXCEPT the ring |
| 2c | full-res shading from the gather target | caller's framebuffer | the ring |

At scale 1.0 nothing changes: one draw, straight onto the caller's framebuffer,
by the same program as today.

## 2. Step 0: test harness and baseline

The harness that produced the proposal's numbers lived in a session scratch
directory and does not survive it. Rebuild it first, against the unmodified
tree:

- the twelve scenes of
  [`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html)
  at scales 1.0, 0.75, 0.5, 0.35, 0.25 and 0.125, through `OffscreenCapture`
  at 1280 x 720, hue rotation and colour cross-fade off;
- scaled scenes for `SpotlightRenderer` and `LensFlareRenderer`, because step 1
  moves both off `neon-blit.frag`;
- the regression pair from
  [`glow-side-comparison.md`](glow-side-comparison.md) changes 5 and 6:
  `glowSide` OUTSIDE with `insideCutoff` off, against `{on, 0, 0}`, at 0.5 and
  0.25. Those two configs describe the same silhouette and must stay
  byte-identical to each other;
- the sub-pixel motion sweep: the hairline rect moved down 1/8 px for 64 steps,
  recording the filament centroid's error against the true edge;
- timing: `glFinish`-bracketed `Render()` loops, variants interleaved across
  rounds, min taken, at 1280 x 720 and 1920 x 1080.

Save every capture as the baseline. Each step below is checked against it, as
raw RGBA compared byte-wise, never through a PNG decoder.

**Done.** 94 captures: the twelve scenes plus a bright-glow `partition` scene
for step 5, each at six scales; the regression pair at two; two spotlight and
two flare scenes at three. Two independent capture runs compared identical, so
the baseline is deterministic, and the regression pair agrees. The harness
(`elcheck`: `capture`, `compare`, `time`) lives in the session scratch
directory for now; decision 4 is still open.

On this machine the timing noise between two builds of IDENTICAL neon code is
about -5% to +7% per variant, even with the minimum over four interleaved
runs. Treat any single A/B difference inside that band as no change.

## 3. Step 1: the spotlight and the flare get their own blit shader

`neon-blit.frag` is compiled by THREE renderers today. `SpotlightRenderer` and
`LensFlareRenderer` use it as a plain premultiplied composite of their reduced
buffers, and switch off the neon's one-sided cut by uploading
`uGlowSide = GlowSide::BOTH`, with a long comment at each call site on why that
upload must be explicit. Steps 2 and 5 would add more neon-only logic to the
same shader (the cutoffs, then a uniform UV map), and each addition would need
another "switch it off" upload in both of those renderers.

So split it first. The two layers that want a plain composite get one, and
`neon-blit.frag` becomes the neon's alone.

### `lib/shaders/blit.frag`, new

The whole shader is the composite `neon-blit.frag` performs when the cut is off:

```glsl
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    // Bilinear upsample of a reduced-resolution premultiplied buffer: colour
    // and coverage alpha filter together, so the composite is fringe-free.
    fragColor = texture(uSource, vPos * 0.5 + 0.5);
}
```

It keeps `neon.vert` as its vertex stage, as both renderers do today: a plain
`uMVP` pass-through, drawn with an identity matrix over their NDC quads.

### Registration, the three places `CLAUDE.md` names for a new shader

- `lib/CMakeLists.txt`: add `shaders/blit.frag` to `CMAKE_CONFIGURE_DEPENDS`,
  and a `file(READ ... BLIT_FRAG_CONTENT)`.
- `lib/shaders/shaders.h.in`: `BLIT_FRAG_SRC`, with `@GLSL_VERSION@` and no
  tuning header.

### `lib/src/renderer/spotlight-renderer.cpp`, `lens-flare-renderer.cpp`

- `setupShaders`: build `mBlitShader` from `BLIT_FRAG_SRC`.
- `Render`: delete the `uGlowSide` upload and the comment block that explains
  it. Nothing in `blit.frag` can be left on by accident, so there is nothing
  left to switch off.

### `lib/shaders/neon-blit.frag`

- Delete the "THREE RENDERERS COMPILE THIS SHADER" block.
- Inject `@NEON_TUNING@` (in `shaders.h.in`), as `neon.frag` and
  `neon-emission.frag` already do. The shader is neon-only now, so it can share
  the neon's constants. Step 2 needs that.

### Check

- Spotlight and flare scaled scenes: byte-identical. For them, the old shader
  computed `src * 1.0`, which is exactly `src` in IEEE arithmetic, from the same
  `uv` expression.
- Every neon scene at every scale: byte-identical. Its blit only lost a comment
  and gained an unused include.
- Spotlight and flare blit timing: equal or lower, since the one uniform branch
  is gone.

**Done.** All 94 captures byte-identical to the baseline, the regression pair
still agrees, and the log shows `SpotlightRenderer.Blit` and
`LensFlareRenderer.Blit` linking from the new source with no compile errors.
Timing shows no measurable change: spotlight and flare scaled variants moved
-2.8% to +7.0% at 1080p (min over four interleaved runs), against -1.7% to +6.3%
for the neon, whose code did not change. The composite is a texture read and
fixed per-pass cost, so the removed branch is too small to show.

Review finding I23 recorded the explicit `uGlowSide` uploads as its fix; it is
now marked FIXED, THEN SUPERSEDED.

## 4. Step 2: the inside/outside cutoffs move into the neon's blit

Independently useful, and part of the ring design either way: outside the ring
the cutoff edges have to be drawn by the blit. Emulated, it takes the
`bounded_band` scene from p99 41 to 4 at scale 0.5 and from 69 to 12 at 0.25.

### `lib/shaders/neon-blit.frag`

- Add `uInsideCutoff`, `uInsideCutoffSoftness`, `uOutsideCutoff` and
  `uOutsideCutoffSoftness`, in FULL-RES px, uploaded exactly as pass 1 uploads
  them but without the scale: a disabled side arrives as
  `CUTOFF_DISABLED_SIZE`, the same sentinel `neon.frag` already receives.
- Mirror `neon.frag`'s band-distance block in GLSL, now that the tuning header
  is here. A cutoff on the side `glowSide` already culls is neutralised with
  `CUTOFF_NEUTRALISED`. Use the band distances (`bandOuterDistance`, per-axis
  at `cornerRadius` 0, and `d + inMid` inside), midpoints
  `size + softness / 2`, half-widths `0.5 * max(softness, aa)` with
  `aa = fwidth(d)`, and `smoothstep` applied to the premultiplied sample like
  the cut. Same "one edge, written twice" comment as the glow-side cut has.
- Compute `d` once, inside a branch on an expression of uniforms only:
  `uGlowSide != GLOW_SIDE_BOTH`, or either cutoff below the sentinel. A branch
  on uniforms keeps `fwidth` inside it legal. With neither set, the shader is
  still the plain texture read whose cost its own comment measures. Re-measure
  BOTH as well as a one-sided and a cutoff scene, as that comment asks.

### `lib/shaders/neon.frag`, scaled path only (`blitOwnsCut`)

- Skip the two post-grade cutoff masks, exactly as the one-sided cut is
  skipped today.
- Move the two cutoff discards out by a new `BLIT_CUTOFF_GUARD_PX`, in BUFFER
  px, so the blit's filter has lit texels to rebuild the boundary from. That
  is the same argument and value (2.0) as `BLIT_SIDE_GUARD_PX`.
- Floor the cutoff ramps that place those discards at one DESTINATION pixel
  (`sideAA`, already in buffer units) instead of `CUTOFF_SOFT_FLOOR_PX`. The
  mask now lives at destination resolution, so the buffer-px floor no longer
  places anything visible.
- Push the quad-edge fade's `cutEdge` out by the same guard, so guard texels
  are not faded before the blit reads them.

### `lib/src/renderer/neon-renderer.cpp`

- `setupGeometry`: on the scaled path, add the guard (converted back to
  full-res px, divided by `scale`) to the outside-cutoff cap and to the inside
  cutoff's `innerReach`, and replace the `CUTOFF_SOFT_FLOOR_PX / scale` floor
  with the 1 px destination floor.
- `renderBlitPass`: upload the four cutoff values in full-res px.

### `lib/include/renderer/neon-tuning.h`

- Add `BLIT_CUTOFF_GUARD_PX` with the documentation `BLIT_SIDE_GUARD_PX` has.
- `CUTOFF_SOFT_FLOOR_PX` loses its job. Remove it and its placement table, or
  keep it only if something still reads it; do not leave it documented as
  doing what it no longer does.

### Check

- `bounded_band`: p99 at most 4 at 0.5 and 12 at 0.25.
- Every scene without a cutoff, at every scale: byte-identical.
- Every scene at 1.0: byte-identical (the direct path is untouched).
- The glow-side regression pair: still byte-identical to each other.
- Neon blit timing with glow on both sides and no cutoff: unchanged.

**Done.** Against the step-0 baseline, exactly the five `bounded_band`
captures below 1.0 changed; the other 89 are byte-identical, including
`bounded_band` at 1.0, every spotlight and flare scene, and the regression
pair (which still agrees with itself). Quality against each scene's own 1.0
render:

| `bounded_band` | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| -------------- | ---- | --- | ---- | ---- | ----- |
| before, p99 / max | 22 / 31 | 41 / 43 | 51 / 71 | 69 / 72 | 95 / 114 |
| after, p99 / max | 2 / 3 | **4 / 5** | 7 / 10 | **12 / 15** | 47 / 51 |
| lit px (1.0: 47,012), before | 47,374 | 50,703 | 52,566 | 53,568 | 65,732 |
| lit px, after | 47,012 | 47,012 | 47,012 | 47,012 | 47,014 |

Both targets are met exactly as emulated, and the band no longer spreads past
its own cutoffs. Four more cutoff scenes were added to the harness to cover
every place the guard band changes the glow quad: glowSide OUTSIDE with an
outside cutoff, INSIDE with an inside cutoff, a square band at `cornerRadius`
0, and a tight band through bright glow. All four keep a constant lit-pixel
count at every scale, so nothing clips, and their remaining error at 0.25
(p99 10-14) sits within +/-6 px of the line, the filament zone step 5
addresses, not at the cutoff edges.

Timing, min over three interleaved runs per resolution: scenes without a
cutoff are unchanged within noise (-1.8% to +4.1%). `bounded_band` below 1.0
costs 2-10% more (0.004-0.017 ms): with a cutoff live, the fullscreen blit now
evaluates the rect SDF at every pixel. That is the cost of drawing the edge at
destination resolution, and it is the same cost a one-sided glow already pays.

Docs changed with the step, because it made them stale: `config-reference.md`
(two places that named `CUTOFF_SOFT_FLOOR_PX` as the scaled path's floor) and a
"since superseded" note in `glow-side-comparison.md`, whose history points at
the placement table this step removed. The step-0 baseline plus the four new
scenes, captured after this step (118 in all), is the reference steps 3 and 4
must reproduce byte for byte.

## 5. Step 3: `Framebuffer` gains extra colour attachments

### `lib/include/gl/framebuffer.h`

- `Resize` gains a trailing `int colorAttachments = 1`, clamped to 1-3. All
  attachments share the format and filter already passed. The early-out also
  compares the count, so a change reallocates.
- Store the textures as an array (`GLuint mTextures[3]`), attached to
  `GL_COLOR_ATTACHMENT0 + i`. Call `glDrawBuffers` once at creation, and only
  when the count is above 1: draw-buffer state belongs to the FBO, so it
  persists, and a count of 1 keeps today's FBO exactly.
- `ClearBuffer` clears every attachment (`glClearBufferfv` per draw buffer).
- `BindTexture(unit, attachment = 0)`, `GetTextureId(attachment = 0)`, and a
  `GetAttachmentCount()`.
- The move constructor and assignment carry the array; `destroy` deletes all
  of it.

Every existing caller keeps the default of 1: `SpotlightRenderer`,
`LensFlareRenderer`, `OffscreenCapture` and the neon's own buffers today.

### Check

- Every capture byte-identical.
- At `NeonRenderer::Initialize`, check `GL_MAX_DRAW_BUFFERS >= 3` and fail
  cleanly otherwise. GL 3.3 and GLES 3.0 both guarantee 4, so this should never
  fire; it exists so a driver that lies fails at startup, not mid-frame.

## 6. Step 4: the reduced pass writes the gather target

### Shader variants from one source

`setupShaders` builds the neon program more than once from the same
`NEON_FRAG_SRC`, inserting a `#define` after the version line (every embedded
source starts with `@GLSL_VERSION@` on line 1, so the insertion point is the
first newline). One small helper in `neon-renderer.cpp`'s anonymous namespace
does the splice.

The ring could have been a new shader file instead, but it would be a copy of
`neon.frag`'s thousand lines of shading with only the gather swapped out, and
two copies of that drift. Defines keep one source. The blit in step 1 went the
other way because there the two users wanted nothing in common.

| member | define | used for |
| ------ | ------ | -------- |
| `mNeonShader` | none | scale 1.0. The source is unchanged, so 1.0 stays byte-identical by construction, and it never writes to a host's extra draw buffers. |
| `mNeonScaledShader` | `NEON_WRITES_GATHER` | pass 1 below scale 1.0 |
| `mNeonRingShader` | `NEON_RING_PASS` | pass 2c (step 5) |

Each needs the same three `SetUniformBlockBinding` calls. `renderNeonPass` takes
the program to drive as a parameter rather than growing a second copy of its
forty uniform uploads. Measure the extra compile time at `Initialize`.

### `lib/shaders/neon.frag`

Under `NEON_WRITES_GATHER`, declare and write two more outputs once the gather
values exist:

```glsl
layout(location = 1) out vec4 oGather;    // col.rgb, enc(emitCoverGathered)
layout(location = 2) out vec4 oGatherSeg; // segColHue.rgb, enc(segCoverGathered)
// enc(c) = c / (1.0 + c): the coverages are unbounded (arc intensity, segment
// boost), and this keeps them in an RGBA8 channel without a float target.
```

Clamp the hues to [0, 1] on write; they are convex combinations of stop colours
and should already be in range.

### `lib/src/renderer/neon-renderer.cpp`

- `renderNeonPass`, scaled path: `mScaledBuffer.Resize(..., colorAttachments)`
  with 3 when the effective segment count is above 0 and 2 otherwise. Only a
  host adding the first segment or clearing the last one reallocates; segment
  animations move segments without changing the count.
- `Render`: **disable blending for pass 1 on the scaled path.** The target was
  just cleared to zero and the glow quad covers each texel once (the four strips
  tile without overlap), so premultiplied-over reduces to a plain write and the
  colour target is unchanged. The gather attachments are data, not colour, and
  must never be blended; GLES 3.0 has no per-attachment blend state to exempt
  them with. Blending is re-asserted before 2b, which is where `Render` already
  owns that timeline.
- `OnConfigChanged`: the existing `mScaledBuffer.Release()` already frees every
  attachment with the buffer.

### Check

- The colour attachment, and therefore every final frame, byte-identical at
  every scale.
- Read attachments 1 and 2 back (`CaptureUtil::ReadTexture2D`) on the default,
  `arcs` and `segments` scenes: decoded hue and coverage match the shader's
  values to within 1/255.
- Pass 1 costs at most ~5% more on the scaled path.

## 7. Step 5: the edge ring

### Geometry: `setupRingGeometry(config)`, new

Builds TWO vertex arrays in FULL-RES rect-local px, from one set of computed
box coordinates:

- `mRingVertexArray`: a rectangular annulus covering everything within `R` of
  the rect edge. Four strips, built exactly as `setupFillGeometry` builds the
  fill ring: outer box `halfSize + R`, hole `halfSize - R` minus the corner
  inset `(r - R) * (1 - 1/sqrt(2))`, clamped at zero.
- `mBlitVertexArray`: the exact complement. A frame from +/-65536 px (the
  `FILL_MAX_OUTER_MARGIN` construction) in to the ring's outer box, plus the
  ring's hole as one quad.

The two share their boundary vertices bit for bit, because both are emitted
from the same floats in the same function. Axis-aligned edges at identical
coordinates rasterise identically, so every pixel goes to exactly one of the
two draws. No per-pixel test is needed in either shader, and nothing depends on
two programs computing the same float. Both passes composite premultiplied, so
a pixel drawn twice would composite twice; this is what rules it out.

The ring is a CONSERVATIVE bound: near the corners it covers some pixels
farther than `R` from the edge. They are shaded at full resolution and so are
exact; they only cost a little.

Rebuilt on its own narrow gate,
`ringDirty = geometry || resolutionScale || lineWidth || filamentFalloff`, and
only while `UsesScaledBuffer`. An intensity or glow-radius animation does not
rebuild it.

### Ring width

```
R = min(filamentReach, RING_MAX_SIGMAS * sigma) + RING_GUARD_TEXELS / scale
```

in full-res px, with `RING_MAX_SIGMAS` and `RING_GUARD_TEXELS` new constants in
`neon-tuning.h`.

- `filamentReach` and `sigma` are the full-res filament reach and half-width.
  They are already computed twice, in `neon.frag` and in `setupGeometry`; a
  third copy here is how they drift. Factor the CPU side into one helper that
  `setupGeometry` and `setupRingGeometry` both call.
- `RING_MAX_SIGMAS` caps very soft filaments: at `filamentFalloff` 0.27 the
  reach clamps to 64 sigmas, which would make the ring as large as the glow.
- `RING_GUARD_TEXELS` covers the bilinear footprint of the reduced buffer.
- Starting values: whatever reproduces the proposal's measured widths (8 px
  at 0.5 and 12 px at 0.25 on the default scenes). The sweep in the check
  below sets the final ones.

### The blit (`renderBlitPass`, `neon-blit.frag`)

- Draws `mBlitVertexArray` with the full-res transform the opaque fill already
  builds, instead of the fullscreen NDC quad.
- The texture coordinate can no longer be `vPos * 0.5 + 0.5`, because `vPos`
  is now rect-local px. Replace it with a uniform affine map,
  `uv = vPos * uUVScale + uUVOffset`, uploaded as `1 / viewport` and
  `rectCentre / viewport`: exactly the mapping the fullscreen quad gave. After
  step 1 nothing else compiles this shader, so no other renderer has to learn
  about the map.

### The ring shader (`neon.frag` under `NEON_RING_PASS`)

- The gather loop is replaced by bilinear reads of the gather target, at the
  same `uv` map as the blit, decoded with `c / (1 - c)`. The segment target is
  read only when `uSegmentCount > 0`, the same uniform branch the loop uses.
- Every uniform is uploaded at FULL resolution with `uResolutionScale` 1.0, so
  the shader behaves as the direct path does: the Nyquist filament floor is
  off, and the one-sided cut and the cutoffs are applied in-shader below the
  grade.
- `uQuadMargin` goes up in full-res px (`mQuadMargin / scale`).
- No emission table is bound; the variant does not read it.

### `lib/src/renderer/neon-renderer.cpp`, `Render`

```
2a fill  ->  0 emission  ->  1 gather into the MRT buffer (blend off)
  ->  restore the caller's target
  ->  2b blit over mBlitVertexArray   (premultiplied over)
  ->  2c renderRingPass over mRingVertexArray (premultiplied over)
```

If pass 1 fails (`glowReady` false), skip both 2b and 2c, as the blit is
skipped today. The fill-first, `opaqueOnly` and failure behaviours do not
change.

### `lib/include/renderer/neon-renderer.h`

New members (`mNeonScaledShader`, `mNeonRingShader`, `mRingVertexArray`,
`mBlitVertexArray`, their vertex counts, the ring width) and methods
(`setupRingGeometry`, `renderRingPass`), declared in pass order. Update the
pass-numbering note: the header, the .cpp and the numbering must keep agreeing.

### Check

Quality, against the step-0 baseline's 1.0 captures:

- All twelve scenes: max error at most 2 at scale 0.5 and 4 at 0.25. Those are
  the emulation's numbers; anything above them is a real defect, not noise.
- Motion sweep at 0.25: the hairline's centroid stays within +/-0.05 px of the
  true edge, against +/-0.53 today and +/-0.02 at 1.0.
- A partition scene with bright glow at distance `R` (`intensity` 3,
  `glowRadius` 20): the error map shows no line along either box of the ring.
  A double draw or a gap there would read as a bright or dark 1 px seam.

Cost, interleaved against step 4, at 1280 x 720 and 1920 x 1080:

- Default scene: at most +0.05 ms at 0.5 and +0.06 ms at 0.25, the proposal's
  upper bounds.
- Scale 1.0: unchanged.

Calibrating `R`: sweep `lineWidth` {1, 2, 4, 8, 16} x `filamentFalloff`
{0.5, 1, 2, 4} x `glowRadius` {0, 2, 5, 20} at 0.5 and 0.25. Pick the smallest
`RING_MAX_SIGMAS` and `RING_GUARD_TEXELS` that keep every combination within
the quality check, and record the table in `neon-tuning.h` the way the other
tuned constants there are recorded.

## 8. Step 6: docs

- `CLAUDE.md`: the `NeonRenderer` paragraph. The scaled path is no longer
  "only the render target, the blit and the buffer allocation are
  conditional"; it now also draws a ring and writes extra attachments.
- `lib/include/renderer/neon-renderer.h`: the class comment's resolution-scale
  section and the pass list.
- `lib/include/core/config.h`: the `resolutionScale` comment, with what
  reduced scale now costs in quality.
- `docs/emission-prepass.md`: the pass tables.
- `docs/neon-renderer-reference.html`: sections 18 and 19, the flow charts of
  what each pass writes and one frame's draw sequence.
- `lib/include/renderer/neon-tuning.h`: the guard and ring constants, and the
  retired `CUTOFF_SOFT_FLOOR_PX` block.
- `docs/glow-side-comparison.md` and `docs/config-reference.md`: done in
  step 2, which removed the constant they named.
- `docs/review-findings.md`: a FIXED entry for the blurred cutoffs at reduced
  scale, with the before/after.
- Step 1 needed one doc change, made with it: review finding I23. Nothing
  else. `docs/spotlight-renderer.md` and `CLAUDE.md` cite
  `neon-blit.frag` only for its account of why a reduced buffer cannot carry a
  sharp mask, which stays true. `docs/lens-flare-unification-comparison.md`
  says the flare shared the neon's blit; that records what was true when it
  was measured, so it stays as written.
- `docs/neon-resolution-scale-proposal.md`: measured results beside the
  emulated ones.
- `docs/neon-resolution-scale-comparison.html`: regenerated with the new path.

## 9. What does not change

- The C ABI, both demos and every `Config` struct, unless decision 1 below
  chooses an opt-in flag.
- The emission pre-pass, the opaque fill and `DebugRenderer`.
- The spotlight's and the flare's output: step 1 moves them to `blit.frag`
  byte-identically, and nothing after it touches them.
- The direct path's program: scale 1.0 compiles the same source it does today.
- The rule that `EdgeLightingEffect::Render` forces no host state: every new
  draw lands on the framebuffer and viewport the renderer was handed, under
  the same blend timeline `Render` already owns.

## 10. Risks

- **GLES is untested here.** Multiple fragment outputs with
  `layout(location)`, `glDrawBuffers` and RGBA8 colour attachments are all
  GLES 3.0 core, but only macOS builds. A device run is needed before release.
- **Memory.** One or two more RGBA8 attachments at the reduced size: 2.1 MB
  each at 1920 x 1080 and scale 0.5, 0.5 MB each at 0.25.
- **Ring cost scales with the perimeter**, not the area: a large rect at 4K
  pays proportionally more, though it stays a thin ring.
- **The partition rests on shared vertex values.** It holds only while both
  arrays are emitted from the same computed floats in one function. A later
  change that builds them separately can open a seam the byte check would
  catch, but only if a scene puts bright glow at `R`; keep the partition scene
  in the harness.
- **Very soft filaments** are where `RING_MAX_SIGMAS` bites. They are smooth,
  so the reduced pass should carry their tails, but that is a claim the
  calibration sweep has to confirm.

## 11. Decisions to settle before step 1

1. **Always on below 1.0, or opt-in?** Recommended: always on. It fixes a
   quality defect at a cost of about +0.02-0.06 ms, and needs no API change.
   Opt-in means a new `NeonConfig` field covered by `operator==`, a C ABI
   setter and getter with its enum or flag mirrored, and a control in both
   demos.
2. **Commit steps 1 and 2 separately first?** Recommended: yes. Step 1 is a
   pure cleanup and step 2 is useful even if the ring slips.
3. **Gather target format.** Recommended: RGBA8; the emulation shows at most
   1 level of max error from it. `RGB10_A2` is the fallback if banding ever
   shows.
4. **Check the harness in?** The repo keeps harnesses out of the tree, but
   there is no test target, and this design's guarantees (byte-identical
   paths, an exact partition, the quality bounds) have no other regression
   check.
