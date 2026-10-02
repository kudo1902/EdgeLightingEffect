# Edge-ring implementation plan for the scaled neon path

The step-by-step plan for building the design in
[`neon-resolution-scale-proposal.md`](neon-resolution-scale-proposal.md): keep
the reduced-resolution glow, and re-shade a thin ring around the rect edge at
full resolution from the reduced pass's gather output. Read the proposal first
for why. This document is only how.

**Status: all seven steps done and committed - steps 0-5 (`546d2b7`, `45bd1f0`,
`c155a37`, `8a50121`, `ea62200`), `db2c250` (`invariant gl_Position`), and
step 6 (`9be1f6f`, `16b36b3`, `d9d9a1f`). The harness is checked in as
`tools/neon-scale-check` (decision 4). What is left is section 11's open
decisions and a run on the target device (section 10).** Step 5 went ahead on
the recommendations (always on below 1.0, RGBA8 gather target, harness kept
out of the tree - since reversed: decision 4).

After the step 5 commit, a review rebuilt a probe and re-verified step 5
against the committed code (section 7, "Re-verified after the commit"). The
quality and byte-identity claims hold. The cost claims hold only on the machine
they were measured on. The review also added one follow-up, committed as
`db2c250`: `invariant gl_Position` in `neon.vert`, which the partition between
the blit and the ring needs. And it found public docs - the `Cutoff` comment in
`config.h`, its copy in the C ABI header, `config-reference.md` - still
describing the cutoff blur step 2 fixed. Those are corrected with the rest
of step 6. Section 11 lists the decisions
taken and the ones still open, three of them new; decision 4 is now taken, and
the harness is checked in as `tools/neon-scale-check`.

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

Before step 1, below scale 1.0:

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
by the same fragment program as before (its shared vertex stage gained
`invariant gl_Position` after step 5; see section 9).

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

On the machine steps 0-5 were timed on (by its numbers the proposal's M2 Pro;
see the cost note in section 7) the timing noise between two builds of
IDENTICAL neon code is
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
the placement table this step removed. It missed the passages that describe
the DEFECT this step fixed rather than the constant it removed: the `Cutoff`
comment in `config.h`, its copy in the C ABI's `el-effect.h`, and the
`softness` row of `config-reference.md` all still say the reduced path blurs
every cutoff edge, with the pixel just inside `size` at 63% at 0.25 and an
exact edge needing scale 1.0. Measured on an outside cutoff of 20 px, that
pixel holds 100% at 0.5 and 0.25, at softness 0, 4 and 16, from step 4 on; the
build before step 1 reproduces the loss (75% and 56% at softness 0). All
three were corrected after step 5; see section 8. The step-0 baseline plus the
four new scenes, captured after this step (118 in all), is the reference steps
3 and 4 must reproduce byte for byte.

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

**Done**, with two deviations from the text above:

- An attachment count outside 1-3 is REFUSED with an error, not clamped, the
  same way `Resize` already treats an invalid size: a pass whose shader writes
  three outputs into a two-attachment buffer has a bug worth hearing about.
- The driver-limit check lives in `Resize` itself, on the allocation path
  only (past the early-out, so never per frame), checking both
  `GL_MAX_COLOR_ATTACHMENTS` and `GL_MAX_DRAW_BUFFERS`. A failure there takes
  the path every `Resize` failure already takes: the scaled pass is skipped and
  the frame degrades to the fill. Whether step 4 should ALSO check at
  `Initialize` is open; nothing requests more than one attachment until then.

All 118 captures are byte-identical to step 2, and every framebuffer the
renderers create logs `attachments=1`. Pixels cannot exercise the new path
while nothing uses it, so the harness gained a direct test (`elcheck fbtest`),
19 checks, all passing: counts 0 and 4 refused with nothing allocated; three
distinct textures; `ClearBuffer` reaching every attachment; a three-output
shader landing output `i` in attachment `i`; an identical request keeping the
same textures; a move carrying all three and emptying the source; 3 -> 2 -> 1
reallocating; three outputs into one attachment writing only the first;
`Release` freeing everything; and no GL error raised by any of it. Timing was
not measured: the per-frame changes are one more integer compare in `Resize`'s
early-out and a one-iteration loop in `ClearBuffer`.

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

**Done**, with the first check amended - it was wrong as written.

The final frames are byte-identical at scale 1.0 (the direct path compiles
the unchanged source) and in 107 of the 118 captures. The other 11 are the
reduced-scale captures of the three widest, brightest glows (`soft_wash`,
`overdrive`, `partition`), each differing on 4 to 16 pixels by exactly 1
level, all 60 to 326 px from the edge in smooth glow at levels 137 to 209.
Two experiments pin the cause on the PROGRAM, not the pass:

| scaled pass draws with | blending | buffer | vs step 3 |
| ---------------------- | -------- | ------ | --------- |
| `mNeonScaledShader` | off | 2-3 attachments | 11 differ, max 1 |
| `mNeonScaledShader` | on | 2-3 attachments | the same 11, max 1 |
| `mNeonShader` | off | 2-3 attachments | all 118 identical |

So neither disabling the blend nor the multi-attachment buffer moves a pixel.
The variant's extra outputs keep the four gather values live, and the compiler
schedules the arithmetic around them differently, which lands a value sitting
on an 8-bit rounding boundary one level over. GLSL 3.30 and ES 3.0 have no
`precise` to pin it. Those pixels are far from the edge, so they stay on the
reduced path after step 5 too; the step 5 quality bar is measured against 1.0,
not against step 4, and is unaffected.

The gather attachments, read back from the renderer's own buffer
(`elcheck gathertest`):

- `default`, a fully lit ring: all 227,520 written texels store coverage
  `enc(1.0)` as 127 or 128. The 2,880 unwritten ones are exactly the 4-texel
  strips either side that the glow quad does not reach.
- `crisp_tube`, one stop at (1.0, 0.25, 0.55): all 198,000 written texels store
  the hue (255, 64, 140) exactly - worst error 0.
- `arcs`: coverage ranges 0 to 126, never reaching a full ring's 127.5.
- `segments`: three attachments; the segment coverage peaks at 1.48 on the top
  edge where the third segment sits, with its colour (102, 255, 230) stored
  as (103, 254, 230).
- `default`, `crisp_tube` and `arcs` get two attachments, `segments` three.

Timing, min over interleaved runs against step 3: the reduced-scale variants
moved -10.3% to +3.1%, so the extra outputs cost nothing measurable. One
direct-path variant, `segments` at 1.0 and 1080p, read +9.6%; it runs the
unchanged program, and earlier sessions measured that same code at 5.13 to
6.03 ms, so the step-3 minimum of 4.76 ms is the outlier.

**Startup cost: about +8 ms.** Constructing and initialising a neon effect
went from 12 ms to 20 ms, which is the second compile of `neon.frag`. Step 5's
ring variant will add about as much again. The programs are built eagerly at
`Initialize`, the policy the blit already follows, so a host never pays a
compile the first time a reduced scale is selected. If startup matters more
than that first-switch hitch, the two variants could be built on first use
instead; that is a decision, not a fix, and is left open.

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
two draws. No per-pixel test is needed in either shader. Both passes composite
premultiplied, so a pixel drawn twice would composite twice; this is what rules
it out.

It does rest on ONE float that two programs compute: `gl_Position`, which the
blit program and the ring program each derive from those shared vertices
through their own compiled copy of `neon.vert`. GLSL promises that two programs
agree on an output only when it is declared `invariant`, and the spec's own
example of the failure is this one: geometry misaligned between the passes of
a multi-pass algorithm. So `neon.vert` declares `invariant gl_Position`. (The
first version of this plan said nothing depended on a float computed across
programs; that was wrong, and the qualifier was added after step 5.)

The ring is a CONSERVATIVE bound: near the corners it covers some pixels
farther than `R` from the edge. They are shaded at full resolution and so are
exact; they only cost a little.

Rebuilt on its own narrow gate,
`ringDirty = geometry || resolutionScale || lineWidth || filamentFalloff`, and
only while `UsesScaledBuffer`. An intensity or glow-radius animation does not
rebuild it.

### Ring width

As planned. The calibration in the Done notes below dropped `RING_MAX_SIGMAS`
and widened R to cover the reduced pass's filament as well; `GetRingWidth` and
the note on `RING_GUARD_TEXELS` in `neon-tuning.h` hold the rule that shipped.

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

**Done**, with the ring-width rule changed by what the calibration found.

**Quality.** Max error against each scene's own 1.0 render, over all 17 neon
scenes the harness now carries (the twelve, the partition scene, and the four
cutoff scenes step 2 added; "before" covers the thirteen that existed then):

| scale | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ----- | ---- | --- | ---- | ---- | ----- |
| worst max, before (step 0) | 70 | 77 | 89 | 93 | 126 |
| worst max, after | **2** | **2** | **2** | **3** | 11 |
| worst p99, after | 1 | 1 | 1 | 1 | 4 |

The 0.25 worst is `small_rect` at 3, inside the target of 4; everything else
is within 2 at every scale but 0.125, where `small_rect` reads 11 - its whole
rect is 20 x 12 buffer texels there, and the emulation predicted exactly 11.
The hairline went from 77 / 93 to 2 / 1 at 0.5 / 0.25, the cutoff band from
43 / 72 to 2 / 2, the flat-top tube from 41 / 60 to 1 / 1.

**Motion.** The hairline's centroid now stays within +/-0.034 px of the true
edge at 0.5, +/-0.027 at 0.25 and +/-0.031 at 0.125, against +/-0.153, +/-0.526
and +/-1.797 before and +/-0.018 at 1.0. 0.75 is unchanged at +/-0.064: its
ring is only ~3 px wide, so the reduced halo still dominates the 12 px window
the sweep measures over. Not a target; see the calibration below for the knob.

**Partition.** On the bright `partition` scene the error within 1.5 px of the
ring's edge is 2 at 0.5, 0.25 and 0.125 - no worse than inside or outside it,
so nothing is drawn twice or skipped. Every 1.0, spotlight and flare capture
is byte-identical to step 4, and the regression pair still agrees.

**What the calibration sweep changed.** Run first with the rule as planned,
the 80-config sweep failed 6 configs at 0.5 and 12 at 0.25 - all thin lines,
small glowRadius - for two reasons the emulation could not have seen:

1. R covered only the full-res filament. Below 1.0 pass 1 draws the filament
   floored to what its buffer can sample, which for a thin line reaches much
   further (a 1 px line at 0.25: 1 px real, 5.7 px as drawn), and that showed
   just outside the ring - the hairline read 40 at 0.25. R is now the wider
   of the two reaches (`GetRingWidth`).
2. `RING_MAX_SIGMAS` capped soft falloffs at 4 sigma, but at falloff 0.5 the
   filament is still at 0.75 there, after its 12x gain. The reach is already
   defined as where the filament has fallen to about 2/255, so the cap only
   ever cut light that shows. It is gone. (Strictly, a cap cuts no light: past
   R the reduced pass draws it. What it costs is that pass's bilinear
   reconstruction of a tail still bright there. This rerun changed three things
   at once and so never isolated the cap; the re-verification below does, and
   confirms it.)
3. Separately, at glowRadius 0 pass 1's quad stopped at the filament's reach,
   short of R, so the ring's outer pixels filtered against gather texels
   nothing wrote and read the hue as black: `lineWidth` 2 / `glowRadius` 0 /
   scale 0.75 read 50 on every lit pixel. `setupGeometry` now extends pass 1's
   quad to R plus a texel on the scaled path. The colour attachment is
   unaffected - past `uQuadMargin` the quad-edge fade zeroes it.

Rerun with all three fixed, at each candidate `RING_GUARD_TEXELS`:

| `RING_GUARD_TEXELS` | 0.75 | 0.5 | 0.25 |
| ------------------- | ---- | --- | ---- |
| 0.5 | worst 3 | worst 12 (8 configs over 2) | worst 3 (6 over 2) |
| **1.0** | worst 4 (3 over 2) | **worst 2** | **worst 2** |
| 2.0 | worst 2 | worst 2 | worst 2 |

1.0 is the smallest that holds the targets, so it stays. 2.0 cleans up 0.75
as well for a ring one buffer texel wider each side - a cheap option if 0.75
matters. The table is recorded in `neon-tuning.h` beside the constant.

**Cost**, min over three interleaved runs against step 4. The default scene's
1.0 time is the 1.87 ms of the proposal's Apple M2 Pro table, so these are that
machine's numbers; the re-verification below has another machine's, which do
not stay inside the budget.

| default scene | 1.0 | 0.5 | 0.25 |
| ------------- | --- | --- | ---- |
| 1280 x 720, step 4 | 1.87 ms | 0.547 | 0.180 |
| 1280 x 720, step 5 | 1.88 ms | 0.583 (+35 us) | 0.219 (+39 us) |
| 1920 x 1080, step 4 | 3.50 ms | 0.935 | 0.309 |
| 1920 x 1080, step 5 | 3.48 ms | 0.977 (+42 us) | 0.357 (+48 us) |

Inside the budget (+50 us at 0.5, +60 us at 0.25) at both sizes. Against 1.0
the default scene is now 3.2x faster at 0.5 and 8.6x at 0.25 at 720p, and
3.6x and 9.8x at 1080p. The ring costs 27-48 us across the scenes; with the
partition scene's ring at ~5% of the frame, most of that is the pass's fixed
cost - a program switch, ~25 uniform uploads and five texture binds - rather
than its fragments. `bounded_band`, whose whole quad is barely wider than the
ring, pays the same fixed cost and so loses most of what its small scaled
saving was: 1.2x at 0.5 and 1.6x at 0.25.

**Startup:** about +6.5 ms for the third program (20 ms -> 26 ms per effect).

**Where the code departs from the text above**, beyond the ring-width rule:

- The ring fades against `GetGlowMargin(config, 1.0f)` (`mRingQuadMargin`),
  the margin the direct path itself uses, not `mQuadMargin / scale`. The two
  differ by exactly what the scaled margin carries and the ring does not draw:
  the Nyquist-widened filament reach and, under an outside cutoff, the blit's
  guard band. The direct path's margin is the one that makes the ring shade as
  the direct path does.
- `setupRingGeometry` builds both arrays whenever the clamped scale is below
  1.0, not only while `UsesScaledBuffer`: `enable` is not in the ring's dirty
  gate and nothing in the build reads it. A disabled layer leaves them undrawn.
- Step 4's "`renderNeonPass` takes the program to drive as a parameter" became
  `uploadNeonUniforms(shader, ...)`: the shading uniforms (22 of them, not
  forty) moved into one helper that pass 1 and pass 2c both call, and
  `renderNeonPass` picks its own program from `scaled`. Nor do all three
  programs take the same three block bindings: the ring program has no
  `LoopSamplesBlock`, so it takes two.

### Re-verified after the commit

The step-0 harness and its baseline did not survive their session (decision
4), so a review on 2026-10-02 wrote a new probe. It rendered nine scenes on a
900 x 560 rect at 1280 x 720 against three builds - `542dad4` (before step 1),
`8a50121` (step 4) and HEAD (`ea62200`). The scenes: default, hairline (width 1,
glow 2), partition (intensity 3, glow 20), a 12 / 8 px cutoff band, glowSide
OUTSIDE, INSIDE with an inside cutoff, two arcs, one segment, and an OUTSIDE
opaque fill. All of it ran on an i7-9750H MacBook Pro whose GL renderer is an
AMD Radeon Pro 5300M, from the x86_64 build. ("AMD 5300M" below.)

**Scale 1.0 is byte-identical** across all three builds, in every scene.

**Quality reproduces.** Against each build's own 1.0 render, max / p99:

| scene | 0.5: before plan -> step 4 -> HEAD | 0.25: before plan -> step 4 -> HEAD |
| ----- | ---------------------------------- | ----------------------------------- |
| default | 7/4 -> 7/4 -> **2/1** | 24/13 -> 24/13 -> **2/1** |
| hairline | 62/19 -> 62/19 -> **2/1** | 86/58 -> 86/58 -> **2/1** |
| cutoff band | 45/16 -> 7/4 -> **2/1** | 64/24 -> 24/13 -> **2/1** |
| arcs | 7/3 -> 7/3 -> **2/1** | 28/8 -> 28/8 -> **2/1** |
| segments | 7/4 -> 7/4 -> **2/1** | 25/12 -> 25/12 -> **2/1** |

The other four scenes read 2/1 or 1/1 at both scales. A second pass measured
the error within 1.5 px of the ring's edge on the default, hairline, partition,
band and OUTSIDE scenes and three soft filaments: never above 1, at 0.5 or
0.25, so nothing there is drawn twice or skipped.

**Cost does not reproduce.** What the ring added over step 4 at 720p, on the
default scene with the rect centred at three sizes, min over four interleaved
rounds of 80 frames:

| rect | perimeter | ring at 0.5 | ring at 0.25 |
| ---- | --------- | ----------- | ------------ |
| 300 x 200 | 1,000 px | +107 us | +90 us |
| 900 x 560 | 2,920 px | +156 us | +123 us |
| 1200 x 680 | 3,760 px | +196 us | +207 us |
| 900 x 560, glowSide OUTSIDE | 2,920 px | +125 us | +113 us |

Read as a line, that is about 75 us that does not move with the rect, plus
about 30 us per 1,000 px of perimeter at 0.5. The fixed part alone is beyond
the budget, and even the smallest rect costs 1.5 to 2 times it. Across the
nine scenes on the 900 x 560 rect, a shorter two-round run gave +96 to
+142 us at 0.5 and +18 to +165 us at 0.25, against same-code noise of -41 to
+12 us (HEAD against step 4 at 1.0). Its +18 was OUTSIDE, and was first read
as the ring being fragment-bound; the four-round run above puts OUTSIDE at
+113 us, so that reading is withdrawn. The cost here is a fixed part and a
per-perimeter part, both real. The reduced path still wins - the
default scene on the 900 x 560 rect runs 2.87 ms at 1.0, 1.09 at 0.5 (2.6x)
and 0.58 at 0.25 (4.9x), against step 4's 0.95 and 0.42 - but the budget is a
property of the machine, and the device has not been measured.

**A tight cutoff band is slower below 1.0 than at it.** The 12 / 8 px band
reads, at 1.0 / 0.5 / 0.25:

| build | 1.0 | 0.5 | 0.25 |
| ----- | --- | --- | ---- |
| before the plan | 0.37 ms | 0.37 | 0.29 |
| step 4 | 0.36 | 0.34 | 0.30 |
| HEAD | 0.34 | **0.46** | **0.37** |

So step 2's full-viewport cutoff blit did not tip it; the ring did. The band's
whole gather quad is barely wider than the ring, so there is little reduced
work left to save, and the ring's fixed part is more than that. The M2 Pro's
`bounded_band` still won (1.2x / 1.6x). Either way the knob is not a guaranteed
saving, which the spotlight's documentation already says of its own scale.

**Soft filaments make the ring wide.** R is the filament's uncapped reach, and
from `filamentFalloff` ~0.3 down that clamps at `FILAMENT_REACH_MAX_SIGMAS` (64):
at falloff 0.3, R is 130 px at `lineWidth` 4 and 258 px at 8. The reduced path's
speedup over 1.0 falls from 2.6x / 4.9x to 1.5x / 2.2x at width 4 and 1.25x /
1.8x at width 8.

Whether a cap would have been safe was tested directly, since the calibration
above dropped `RING_MAX_SIGMAS` in the same rerun as two other fixes and so
never isolated it. A scratch build capped each reach at K sigmas, over
`filamentFalloff` {0.2, 0.3, 0.5, 1} x `lineWidth` {1, 2, 4, 8} x `glowRadius`
{0, 5}:

| cap | worst at 0.5 | worst at 0.25 | cost at 0.5 / 0.25, mean of falloff 0.2 and 0.3 at glow 5 |
| --- | ------------ | ------------- | ------------------------------- |
| none (shipped) | 2 | 2 | 1.68 / 1.21 ms |
| 16 sigmas | 4 | 9 | 1.32 / 0.85 ms |
| 8 sigmas | 4 | 9 | 1.21 / 0.73 ms (0.60 at 0.25 in a second run) |
| 6 sigmas | 5 | 11 | 1.20 / 0.69 ms |
| 4 sigmas | 11 | 36 | 1.20 / 0.76 ms |

So dropping the cap was right for the 2/255 bar. Every failing config is a
soft falloff, and at 8 sigmas they span every line width at 0.25 (`lineWidth`
8 still reads 3), so no width-based exemption rescues a cap. The cost of the
bar on soft filaments is decision 7.

**A cap in buffer texels fails too, and where it fails says why.** A sigma cap
scales with the line. The other natural shape is a fixed number of buffer
texels, `R = min(reach, K / scale) + RING_GUARD_TEXELS / scale`, which never
binds on a thin line. A second scratch build on 2026-10-02 (AMD 5300M) tried
K = 4, 6, 8 and 12 over two layouts - a full-screen rect (1280 x 720,
`cornerRadius` 40, `glowRadius` 20, `bloomStrength` 0.8) and the default
640 x 360 rect with `glowRadius` 0 - x `filamentFalloff` {0.3, 0.5, 1, 2, 4}
x `lineWidth` {2, 4, 8, 12, 16, 24, 32, 48}, 80 configs per scale:

| cap | worst at 0.5 | worst at 0.25 | configs over 2, at 0.5 / 0.25 |
| --- | ------------ | ------------- | ----------------------------- |
| none (shipped) | 4 | 5 | 2 / 1 |
| 12 texels | 10 | 7 | 8 / 5 |
| 8 texels | 17 | 22 | 11 / 14 |
| 6 texels | 25 | 33 | 15 / 17 |
| 4 texels | 39 | 51 | 19 / 23 |

The shipped rule's three are a separate defect, V16 in
[`review-findings.md`](review-findings.md); no cap binds on them, so they are in
every row. A finer sweep of the soft end - `filamentFalloff` 0.1 to 0.75 x
`cornerRadius` {0, 40, 120} x `glowRadius` {0, 5, 20} x `lineWidth` {1, 2, 4,
8, 16, 32} - holds 2 uncapped, and still fails by 3-5 levels at a 24-texel cap
from falloff 0.2 down. Falloff 0.25-0.3 passes at every cap of 12 or more;
that is a window in this grid, not a rule.

Every failing pixel lies on one of three features of the field, and all three
are inside the uncapped reach:

1. **The shoulder of a crisp, thick line.** A flat-top profile drops steeply
   about `lineWidth / 2` from the edge, and the reduced buffer cannot rebuild
   a steep drop however far out it is. At falloff 4, width 32 and a 4-texel cap
   (R 20 px at 0.25), the failing pixels are 20-24 px either side of the edge,
   up to 51 levels.
2. **The interior crease at a rounded corner.** The distance field kinks at
   each corner's centre of curvature and along the diagonal running inward from
   it. Under a cap, a soft or Gaussian line that is still visible that far in
   is left to the blit there. The worst pixel at falloff 0.3 to 2 under an
   8-texel cap is (360, 220) every time, the top-left centre at
   `cornerRadius` 40; under a 12-texel cap it moves to the ring hole's corner
   and down the diagonal (3-12 levels). Glow hides this kind: the full-screen
   layout passed at falloff 0.3 even at 4 texels.
3. **The end of a very soft tail.** From falloff ~0.3 down the tail is clamped
   at `FILAMENT_REACH_MAX_SIGMAS` and pedestal-subtracted to zero there, which
   leaves a kink at 64 sigma - one that shows from about 0.2 down, where the
   tail is still bright at the clamp. At falloff 0.1, width 4 and a 24-texel
   cap, 772 failing pixels lie 128-132 px inside the edge - 64 sigma.

So R is not padding past the visible filament. It ends at the field's last
non-smooth feature, and only the blit's bilinear filter is outside it. A cap
that ignores the profile's shape cannot hold the bar; one that tracked the
shoulder, the corner diagonals and the tail's end would be a different ring
geometry, not a tuning of this one.

What a cap would have bought, full-screen layout at 1280 x 720, median of two
interleaved runs (1.0 is 2.69 ms):

| config | uncapped (R at 0.25) | 12-texel cap | 4-texel cap |
| ------ | -------------------- | ------------ | ----------- |
| falloff 0.3, width 4 | 1.47 / 0.95 ms, 1.8x / 2.8x (R 132 px) | 1.10 / 0.67 ms, 2.5x / 4.0x | 1.04 / 0.55 ms, 2.6x / 4.9x |
| falloff 0.3, width 8 | 1.89 / 1.35 ms, 1.4x / 2.0x (R 260 px) | 1.08 / 0.66 ms, 2.5x / 4.1x | 1.05 / 0.55 ms, 2.6x / 4.9x |
| falloff 0.3, width 16 | 2.03 / 1.53 ms, 1.3x / 1.8x (R 516 px) | 1.11 / 0.66 ms, 2.4x / 4.1x | 1.06 / 0.55 ms, 2.5x / 4.9x |
| falloff 1, width 48 | 1.33 / 0.80 ms, 2.0x / 3.4x (R 89 px) | 1.11 / 0.67 ms, 2.4x / 4.0x | 1.07 / 0.56 ms, 2.5x / 4.8x |

Times are at 0.5 / 0.25. At falloff 1 and widths up to 8 no cap binds and the
times match. From falloff 0.3, width 12, R passes the frame's half-height and
the uncapped cost stops growing, at 2.0-2.1 / 1.53-1.55 ms.

**`invariant gl_Position` added to `neon.vert`**, for the reason in the
geometry section above. `neon.vert` is every renderer's vertex stage, so 36
captures were checked before and after: the nine neon scenes plus a soft
filament at 1.0, 0.5 and 0.25, droplets, the flare and two spotlight lamps at
1.0 and 0.5, and every debug overlay. All 36 are byte-identical, and two runs
of the old build agree with each other. The partition could not be shown to
NEED it on desktop GL, where it never failed; the qualifier is what makes it
promised rather than observed.

**Fewer programs: two is possible, one is not.** `neon.frag` is compiled three
times because its users differ on two independent axes - how many outputs the
program declares, and whether it runs the gather or reads its stored result -
and each one draws in a single, fixed pipeline state:

| program | outputs | gather | draws into |
| ------- | ------- | ------ | ---------- |
| `mNeonShader` | 1 | runs the loop | the caller's framebuffer, blended |
| `mNeonScaledShader` (`NEON_WRITES_GATHER`) | 3 | runs the loop | `mScaledBuffer`'s 2-3 attachments, unblended |
| `mNeonRingShader` (`NEON_RING_PASS`) | 1 | reads it back | the caller's framebuffer, blended |

Scratch builds of the library as it stands at `19049b4` collapsed each axis,
then both, on 2026-10-02 (AMD 5300M):

- **A** - 1.0 draws with the gather-writing program, so `mNeonShader` is
  compiled with `NEON_WRITES_GATHER` and the separate scaled program goes.
- **B** - the ring folds into the plain program behind `uniform int
  uRingPass`: the four `NEON_RING_PASS` blocks become one branch around the
  gather, with the coverage divide moved inside its loop arm. The scaled
  program stays.
- **AB** - both: one program for all three draws.

| build | programs | output against HEAD | frame time against HEAD | startup per effect |
| ----- | -------- | ------------------- | ----------------------- | ------------------ |
| HEAD | 3 | - | - | 50.0 ms |
| A | 2 | byte-identical | -24 to +30 us | 37.7 ms |
| B | 2 | byte-identical | -25 to +26 us | 39.3 ms |
| AB | 1 | byte-identical | **176-198 ms per frame below 1.0**, 93-624x HEAD | 25.2 ms |

"Byte-identical" is 1,182 renders per build: the 210 images
`neon-scale-check generate --images` writes (twelve scenes at six scales, with
heatmaps and crops), and 972 more - each scene x `glowRadius` {0, 5, 20} x
`lineWidth` {1, 4, 16} x `filamentFalloff` {0.3, 1, 4} x scale {1.0, 0.5,
0.25}. `check` passes on all three with HEAD's numbers. Frame time is
`neon-scale-check time`, six rounds with the build order rotated, median per
figure, over every scene and scale; HEAD's own round-to-round spread is 37 us
at the median, so neither A nor B moves. (B reads about 20 us faster at 1.0 on
all twelve scenes, inside that spread.) A soft full-screen filament - falloff
0.3, widths 8 and 16, a ring 260-516 px wide, the case where B's ring runs the
most fragments with the loop compiled in - stays within 24 us for both.
Startup is the median over the 72 effects each `time` run initialises; AB's
figures are its one completed run, since every further one took 38 minutes.

**Why one program fails.** AB is the only build in which one program draws in
two pipeline states in the same frame: pass 1 into `mScaledBuffer`'s
attachments with blending off, then the ring onto the caller's single
attachment with blending on. At 1.0, where it draws in one state, it is within
69 us of HEAD. A second build, AB2, also kept every texture unit at one format
in both passes - the gather samplers on an RGBA8 LUT's unit in pass 1, the
gather itself on units 5-6 in the ring so the RGBA16F emission table keeps
unit 3 - and was just as slow (213-217 ms on the soft full-screen filaments).
That rules out texture format. Render target layout and blend state remain,
and both are what the two passes ARE, so one program is out on this driver
whichever it is. Not tested on the M2 Pro.

**A against B.** B pairs programs with states: plain-plus-ring always draws
onto the caller's framebuffer, blended, and the scaled program always into the
buffer, unblended. A's program draws in both states, across the scale boundary
rather than within a frame - fine in these runs, which hold one scale per
effect, but it is the pattern AB shows this driver penalising. A also has the
1.0 path declare outputs 1 and 2 on the caller's framebuffer, where they land in
the host's draw buffers 1 and 2 if it has them enabled - a new requirement on
the host, beside the state `EdgeLightingEffect::Render` already leaves to it -
and on the M2 Pro this same program moved 11 reduced-scale captures by 1 level
against the plain one (step 4), so 1.0's byte-identity under A is not
established there. B's open risk is the device: the ring's program now carries
the loop's register use even where the branch skips it, which cost nothing
measurable on this desktop GPU and may on a mobile one.

What merging buys is startup, about 11 ms per effect here, and slightly less
variant code; it buys no frame time. Decision 6 weighs it against building on
first use.

## 8. Step 6: docs

**Done** (`9be1f6f`, `16b36b3`, `d9d9a1f`). Every item below has landed; the
ones marked "new" were not in the original list - two came up in the review, and
the rest are docs that still described the scaled path as it was before step 2
or step 5, which the original list missed.

- `CLAUDE.md`: the `NeonRenderer` paragraph now names the program variant and
  the ring among what is conditional, and a new paragraph covers the edge ring:
  the three `neon.frag` builds, the gather attachments, the unblended pass 1,
  the ring width rule, the partition, `invariant gl_Position` and the cost
  caveat. *New:* the reading list now carries the proposal, this plan and the
  comparison page.
- `lib/include/renderer/neon-renderer.h`: done in step 5.
- `lib/include/core/config.h`: the `resolutionScale` comment says what the
  ring does, the quality (within 2/255 to 0.125 but for a tiny rect) and that
  it is not a guaranteed saving. *New, stale since step 2:* the `Cutoff`
  comment no longer claims a buffer-pixel floor or the 63% edge blur.
- `lib/capi/el-effect.h`: *new*, the cutoff setter's copy of the blur claim
  (stale since step 2) and `el_effect_set_neon_resolution_scale`'s "bilinear
  -blitted back" (stale since step 5), both rewritten with the same caveats.
- `docs/emission-prepass.md`: the pass table gains 2c and the pass 1 variant,
  the `scaled` sentence and the blend-mode note say pass 1 draws unblended,
  and `mFullVertexArray` is corrected to `mFullscreenVertexArray`.
- `docs/neon-renderer-reference.html`: Fig 4 (one frame's draw sequence) is
  redrawn for the current schedule on both lanes - it also predated the
  emission pre-pass and still drew the debug overlays inside the neon - and
  section 19's vertex shader listing gains `invariant gl_Position` with the
  reason. Fig 3's caption names `ringDirty` / `setupRingGeometry`; the file
  table's `neon-blit.frag` row no longer says "two statements".
- `lib/include/renderer/neon-tuning.h`: done in steps 2 and 5.
- `docs/glow-side-comparison.md`: done in step 2. `docs/config-reference.md`:
  step 2 updated the two places naming the removed constant; *new*, the
  `softness` / `size` rows' blur claim and the `resolutionScale` row (7 and
  28 / 255 then, within 2 now) are rewritten.
- `docs/review-findings.md`: a fourteenth pass with **V15** (the blurred
  edges near the line - FIXED, with before / after from the comparison page)
  and **I25** (a reduced scale can cost more than 1.0 - documented), plus a
  note on V13 and rows in both summary tables.
- `docs/neon-resolution-scale-proposal.md`: a "built since" note, the ring
  width rule as shipped (2.4), measured quality beside the emulated (3.1) and
  measured cost on both machines (4.1), and the risks marked resolved where
  they are.
- `docs/neon-resolution-scale-comparison.html`: **regenerated**. The original
  harness was gone (decision 4), so a new one was built and validated against
  the page itself: all twelve scenes were recovered (the original blurbs
  omitted every colour) and reproduce the stored 1.0 images within GPU
  variance, and re-run on the pre-plan build it reproduces the old page's p99
  within 1 level and max within 2 at every scale. The page now shows the edge
  ring's numbers with the pre-plan ones beside each, on an AMD Radeon Pro
  5300M; its method section records the recovered scene definitions and the
  redefined motion metrics.
- *New:* `docs/effect-reference.md` (3.4 and 3.9), `docs/implementation.md`
  (renderer table, section 6, the forked-pairs paragraph), and the demo's Res
  Scale tooltip ("softer edges").
- *New:* `docs/neon-renderer-explained.html` section 10 gains an "edge ring"
  subsection, and its costs list, settings table and copy-shader sentence are
  corrected; `docs/neon-renderer-overview.html` section 5 likewise. The
  settings table's defaults were also stale (0.5 and 64; they are 1.0 and
  128).
- Step 1's one doc change, review finding I23, landed with it.

**Not done, deliberately:** both HTML tiers and the reference still describe
the lens flare's half-resolution twin and the "optimized" neon renderer in
other sections (about a dozen passages). That predates this plan - it is the
flare and neon unifications, not the ring - and wants a refresh of those pages
of its own.

## 9. What does not change

- The C ABI, both demos and every `Config` struct: decision 1 kept the ring
  always on, so there is no flag to add.
- The emission pre-pass, the opaque fill and `DebugRenderer`.
- The spotlight's and the flare's output: step 1 moves them to `blit.frag`
  byte-identically, and nothing after it touches them.
- The direct path's fragment program: scale 1.0 compiles the same `neon.frag`
  source it did before step 4. Its vertex stage did change after step 5:
  `neon.vert`, which every renderer compiles, gained `invariant gl_Position`.
  Every layer was checked byte-identical across that change.
- The rule that `EdgeLightingEffect::Render` forces no host state: every new
  draw lands on the framebuffer and viewport the renderer was handed, under
  the same blend timeline `Render` already owns.

## 10. Risks

Updated after step 5: the last three entries are new, and the old one on very
soft filaments is replaced by the first of them.

- **GLES is untested here.** Multiple fragment outputs with
  `layout(location)`, `glDrawBuffers`, RGBA8 colour attachments and
  `invariant gl_Position` are all GLES 3.0 core, but only macOS builds. A device
  run is needed before release.
- **Memory.** One or two more RGBA8 attachments at the reduced size: 2.1 MB
  each at 1920 x 1080 and scale 0.5, 0.5 MB each at 0.25.
- **Ring cost scales with the perimeter**, not the area: a large rect at 4K
  pays proportionally more, though it stays a thin ring.
- **The partition rests on shared vertex values and on `invariant
  gl_Position`.** It holds only while both arrays are emitted from the same
  computed floats in one function, and while both programs are promised the
  same `gl_Position` for them. A later change that builds the arrays
  separately, gives either pass its own vertex shader, or drops the qualifier
  can open a seam the byte check would catch, but only if a scene puts bright
  glow at `R`; keep the partition scene in the harness.
- **Soft filaments make the ring wide, and that is a cost risk, not a quality
  one.** `RING_MAX_SIGMAS` is gone and R is the filament's full reach, so from
  `filamentFalloff` ~0.3 down R is 32 x `lineWidth` plus the guard: 130 px at
  width 4, 258 px at 8. Quality stays within 2/255, but the reduced path's
  speedup falls to 1.5x / 2.2x at width 4 (0.5 / 0.25). A cap was measured and
  costs 3-9 levels; see decision 7.
- **The ring's cost depends on the machine.** +35-48 us on the M2 Pro. On an
  AMD Radeon Pro 5300M, about 75 us that does not move with the rect plus
  about 30 us per 1,000 px of perimeter: +107 us at 0.5 on a 300 x 200 rect,
  +196 us on a 1200 x 680 one, at 720p. Neither machine says what the device
  pays.
- **Reduced scale can be slower than 1.0.** A tight cutoff band was, on the
  AMD 5300M: 0.34 ms at 1.0, 0.46 at 0.5, 0.37 at 0.25. Hosts that drop the
  scale for speed should measure, as the spotlight's documentation already
  tells them for its own scale.

## 11. Decisions

The first four were posed before step 1. Three are taken, each as recommended;
4 is still open, and 5-7 came up during the steps or after them.

1. **Always on below 1.0, or opt-in?** Taken: always on. It fixes a quality
   defect and needs no API change. The cost was estimated at about +0.02-0.06
   ms; it measured +0.035-0.048 ms on the M2 Pro and +0.09-0.21 ms on the AMD
   5300M, growing with the rect's perimeter (section 7; every scene and scale,
   step by step, in `neon-resolution-scale-perf-comparison.md`). Opt-in would
   mean a new `NeonConfig` field covered by `operator==`, a C ABI setter and
   getter with its enum or flag mirrored, and a control in both demos. Revisit
   if the device's ring cost turns out closer to the AMD figure.
2. **Commit steps 1 and 2 separately first?** Taken: yes. Every step landed as
   its own commit.
3. **Gather target format.** Taken: RGBA8; the emulation showed at most 1 level
   of max error from it, and the shipped path is within 2/255 of 1.0 in every
   scene measured. `RGB10_A2` is the fallback if banding ever shows.
4. **Check the harness in?** Taken: yes, the whole harness, as
   [`tools/neon-scale-check`](../tools/neon-scale-check/README.md) behind
   `-DEDGE_LIGHTING_BUILD_TOOLS=ON`, off by default. The repo kept harnesses
   out of the tree, but there is no test target, and this design's guarantees
   (byte-identical paths, the quality bounds, a line that stays on its edge)
   had no other regression check. The step-0 harness and its 118-capture
   baseline did not survive their session; re-verifying step 5 meant writing a
   probe from scratch, and regenerating the comparison page meant a third
   harness that first had to recover the scenes from the old page's pixels.
   The checked-in tool is that third harness made permanent: its `check` is a
   pass / fail gate (it passes on HEAD and fails on 54 values against
   `542dad4`), its `generate` reproduces every committed image and number on
   the comparison page byte for byte, and it builds standalone against
   another checkout to measure a "before". Not covered: the partition scene,
   the cutoff scenes step 2 added, the calibration sweep and the
   `Framebuffer` tests of steps 0-5, which were in the lost `elcheck`.
5. **Check the draw-buffer limits at `Initialize` too?** Open, from step 3.
   `Framebuffer::Resize` checks them on the allocation path, so a driver short
   of the two attachments a reduced scale asks for (three once there are
   segments) fails on the first such frame, which degrades to the fill. GL 3.3
   and GLES 3.0 both guarantee more, so this is about a driver that lies. A
   check at `NeonRenderer::Initialize` would fail at startup instead.
6. **Build the shader variants eagerly or on first use - or build fewer?**
   Open, from steps 4 and 5. Eager, as shipped, costs about 14.5 ms per effect
   at startup on the M2 Pro (12 -> 26 ms) and 27 ms on the AMD 5300M
   (23 -> 50 ms). On first use moves that to a one-time hitch the first frame
   a reduced scale is selected - the stall the blit's eager build was chosen
   to avoid. Fewer programs is the other lever, measured in section 7: folding
   the ring into the plain program behind a uniform (B) is byte-identical and
   frame-time neutral on the AMD 5300M and takes startup to 39.3 ms there,
   while one program for all three draws runs 93-624x slower below 1.0 and is
   out. The two combine: B plus a scaled program built on first use leaves a
   host that stays at 1.0 compiling one `neon.frag`. B wants a measurement on
   the device before it lands, for the loop's register use in the ring.
7. **Cap the ring on soft filaments?** Open, from the re-verification. Uncapped,
   as shipped, holds 2/255 on every config both cap sweeps covered but V16's
   glow-free crisp hairlines, and leaves a soft filament's reduced path
   only 1.25-2.2x faster than 1.0. Both natural cap shapes were measured
   (section 7). An 8-sigma cap takes the soft configs at 0.25 from 1.21 ms to
   0.60-0.73 ms and reads 3-9/255 on them. A 12-buffer-texel cap takes falloff
   0.3 from 0.95-1.53 ms to 0.66 ms at 0.25 (4.0x over 1.0) and holds 2/255 at
   that falloff, but reads up to 11/255 elsewhere - falloff 0.1-0.2 and
   0.35-0.75, and crisp thick lines - and a 4-texel cap up to 51/255. Every
   failing pixel sits on a feature of the field the blit cannot rebuild - a
   crisp line's shoulder, a corner's interior crease, a clamped tail's end -
   so a cosmetic retune of the width rule will not close this. Accepting a cap
   means accepting those errors; avoiding them means a ring shaped by those
   features.
