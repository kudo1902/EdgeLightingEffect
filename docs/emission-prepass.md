# Perimeter emission pre-pass

Design notes for `lib/shaders/neon-emission.frag` and the pass structure it
introduces in `NeonRenderer`.

> Written when the half-res path was a second renderer,
> `NeonOptimizedRenderer`, with its own copy of the shader. It is one
> renderer now, drawing at `NeonConfig::resolutionScale` - one path at every
> scale since I57, with the gather a pass of its own (`neon-gather.frag`) that
> is the table's only reader. Sections 3, 4 and 8 describe the code as it
> stands; sections 1, 2 and 7 are the record of what the pre-pass changed.

The short version: the neon gather loop was recomputing an identical per-sample
table in every screen fragment. That table is now baked into a small texture -
once, on the frames where its inputs actually move - and the gather reads it
with `texelFetch`. Same picture; the per-fragment cost stops scaling with the
number of arcs and segments, and a still ring stops paying for the bake at all
(section 3, *When pass 0 runs*).

## 1. Motivation

Each fragment of the neon quad gathers over `NEON_MAX_LOOP_SAMPLES` perimeter
samples. The loop body did four things per sample:

1. Scan `uArcCount` arcs, evaluate `arcInside` for each, keep the winner by
   `mask * intensity`.
2. Fetch the winner's colour from `uArcLUT` or `uGradientLUT`.
3. Scan `uSegmentCount` segments, evaluate a Gaussian bell for each, and fetch
   from `uSegmentLUT` where the segment carries its own stops.
4. Accumulate the distance-weighted colour.

Only step 4 depends on the fragment. Steps 1-3 read the sample's perimeter
position `si`, the clock, and the config - nothing else:

```glsl
float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;  // si = i / N
float bell = seg.z * exp(-e * e);                                 // rel = si - seg.x
baseColI   = texture(uGradientLUT, vec2(ti, 0.5)).rgb;            // ti = si - uTime * rate
```

At the demo's defaults the quad covers a 3840x2160 viewport, so the loop body
ran about **1.06 billion** times per frame (8.3M fragments x 128 samples) to
derive a 128-entry table. The pre-pass computes it 128 times instead.

This is loop-invariant hoisting where the invariant loop is the implicit one
over fragments. You cannot hoist out of that inside a shader, so the work has
to become a separate draw.

## 2. What the pre-pass emits

A `NEON_MAX_LOOP_SAMPLES` x **2** texture. `gl_FragCoord.x` is the sample
index, `gl_FragCoord.y` picks the row:

| row | `.rgb` | `.a` |
| --- | ------ | ---- |
| 0 | `arcColour * arcW` | `arcW` |
| 1 | `SUM(segColour * bell)` | `SUM(bell)` |

where `arcW` is the winning arc's `arcInside * intensity` at that sample.

### Why two rows and not one

The consumer keeps two **independently normalised** accumulators:

```glsl
col       = SUM(baseColI * arcW * g) / SUM(arcW * g)     // arc hue
segColHue = SUM(segColour * bell * g) / SUM(bell * g)    // segment hue
```

That gated normalisation is deliberate - it makes each hue a unit-magnitude
value carrying neither coverage nor intensity, which is what made the effect
size-invariant (see the comments in `neon.frag`). But it means the two colour
terms ride **different weights**: `arcW * g` for the arc, `bell * g` for the
segments.

A single premultiplied colour only collapses when both halves share one weight:

```
lightCol = (SUM Ci*arcWi*gi)/SUM gi + (SUM Si*gi)/SUM gi
         = SUM (Ci*arcWi + Si)*gi / SUM gi        <- only valid with a shared gi
```

With separate denominators each term needs its own numerator **and** its own
denominator - four floats each, so eight in total, so two texels.

> The reference implementation on `improve_neon_by_emission_pre_pass` packs
> this into one texel. It could, because it predates the gated normalisation:
> there both colour terms rode the same `g`, and its `.a` carried a `cover`
> value that fed a *gathered* halo/bloom. On this branch halo and bloom are
> analytic closed forms driven by pointwise coverage, so no per-sample cover is
> needed at all - `.a` carries `arcW` for the denominator instead.

`uIntensity` is deliberately **not** folded in here (the reference does fold
it). It cancels out of the gated normalisation anyway and reaches the emission
through `emitCover` / `filamentGate`, which are pointwise and size-invariant.

## 3. Pass structure

`Render` is a pass schedule and nothing else: derive the
transform, then one call per pass, each in its own private method. Blend state
is owned by `Render`; each pass owns its shader.

**`NeonRenderer::Render`** - one schedule at every `resolutionScale` (at 1.0
the reduced buffer is simply full size; the direct path 1.0 used to take went
with I57). It runs in TWO PHASES: everything that renders offscreen first, then
everything on the caller's framebuffer, so that target is drawn in one unbroken
run per frame (on a tile-based GPU every switch away from it and back stores
and reloads its tiles). `debug.opaqueOnly` short-circuits all of it to the fill
alone. The table below places pass 0 in the schedule; Parts 5.5 and 6 of
[`neon-onboarding-guide.md`](neon-onboarding-guide.md) have the whole schedule
and each pass's inputs, state and outputs.

| pass | method | target | runs |
| ---- | ------ | ------ | ---- |
| - | `ensureGlowPrograms`, `packLightBlocks` | - | programs on first use; the light blocks repacked only when they changed, always bound |
| 0 | `renderEmissionPass` | `mEmission.buffer` (`NEON_MAX_LOOP_SAMPLES` x 2, allocated in `Initialize`) | only when an input moved (`isEmissionTableStale`, below) |
| 0b, 0s, 0f | `renderGlowCoverPass` | `mGlowCover.buffer`, and `mGlowCover.segBuffer` when the segments have a table of their own | the pieces a config change reached; never on a ring lit uniformly |
| 1a | `renderGatherPass` | `mGather.buffer`, at `GetGatherScale` | `neon-gather.frag`, the table's only reader; skipped while what it last drew still answers (`mGather.cache`) |
| 1f | `renderFieldPass` | `mGlowField.buffer` | the hue-invariant field, on `LazyBake`'s schedule |
| 1b or 1c | `renderShadePass` or `renderFieldCompositePass` | `mScaledBuffer` | the shading (`mShadeShader`), or the field's composite in its place |
| 1r | `renderRingFieldPass` | `mRingField.buffer` | the edge ring's field, on its own `LazyBake` |
| 2a | `renderFillPass` | caller's framebuffer | opaque modes only |
| 2b | `renderBlitPass` | caller's framebuffer | `mScaledBuffer`, bilinear, everywhere the glow can be lit outside the edge ring |
| 2c or 2r | `renderRingPass` or `renderRingFieldCompositePass` | caller's framebuffer | the edge ring at full resolution (`mRingShader`), or its field's composite |

On a frame where nothing moved, passes 1a-1c do not run at all
(`mOffscreen.reusable`), and that frame never leaves the caller's framebuffer.
2b and 2c cover disjoint areas built from the same vertices
(`setupRingGeometry`), so no pixel is composited by both; a pixel neither
covers is one the glow cannot reach. See
[`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md) sections 12 and
13. The debug overlays are a separate layer - `DebugRenderer`, drawn after this
renderer, always at full resolution.

The arc and segment UBOs are packed **before** pass 0, because the pre-pass
reads them and its staleness test reads their upload counts; the shading reads
them too, for the pointwise coverages.

### When pass 0 runs

The pre-pass hoists the gather's fragment-invariant half out of every
*fragment*. The same argument hoists it out of every *frame*: the table is a
pure function of `(si, uTime, config)`, and `mEmission.buffer` is allocated once
for the renderer's lifetime and written by nothing else, so a frame that moves
neither input can read the texels the last bake left. `isEmissionTableStale`
is that test, and it has exactly two terms.

- **What the pass binds**, compared against what the last bake bound
  (`EmissionInputs`, built by `getEmissionInputs`): `uHueRotationRate` and
  `uNumSamples` (clamped) by value, and the three LUT textures and the two
  light blocks by **upload count** - `BaseLUT::GetUploadCount`, which every
  `Upload` moves, and `UniformBuffer::GetUploadCount`, which only an upload
  that changed the bytes moves. Version numbers on the GL objects, not a list
  of config fields: an intensity, bloom, glow or geometry animation moves none
  of them and re-bakes nothing; a colour, arc or segment change moves one and
  re-bakes; and a cross-fade frame, which moves the ring texture with no config
  change at all, moves the ring's count like any re-bake. Plus "never baked"
  (`mEmission.bake.baked`), because the buffer holds undefined texels until the
  first bake and its stored key means nothing until then. The light blocks'
  counts are only current once `packLightBlocks` has run, so `Render` packs
  before it asks.
- **`uTime`**, which reaches `neon-emission.frag` in exactly one place:
  `float ti = si - uTime * uHueRotationRate`. At a rate of 0 the product is
  exactly zero, so time drops out of the table altogether and a still ring
  rebakes nothing however the clock runs. At any other rate the comparison is
  `time != mEmission.bake.time`, exact rather than tolerant, because `time` is fed
  straight back from the last bake rather than recomputed.

The first term used to be `mEmissionDirty`, set by `OnConfigChanged` on **any**
config change and from `Update` whenever `GradientRingLUT::Tick` re-uploaded.
That was deliberately wide: a gate on the config fields the shader reads would
have to be kept in step with a shader in another language, and a missed field
is a silently stale ring. The upload counts keep that safety without the field
list - they track the objects the pass actually binds, whatever config field
moved them - and drop the cost the wide flag carried: under an intensity or
geometry animation it re-ran pass 0 every frame. On the AMD Radeon Pro 5300M,
where the first offscreen pass of a frame costs ~0.15 ms whatever it draws,
that was the only thing taking an inline-path frame off the caller's
framebuffer (inferred from `neon-perf-review.md` section 10.1, not measured;
`neon-perf-plan.md`, item 6).

`renderEmissionPass` records what it baked (`mEmission.bake`: the inputs, that
it ran, the time, and a version the gather's own key reads) on its way out, so
the staleness test always reads a snapshot written by the only thing that ever
writes the buffer.

Two consequences worth keeping in mind when changing this:

- **Skipping pass 0 must leave nothing behind for the gather.** It does:
  `bindGatherInputs` binds the emission texture itself for pass 1a, and
  `Render` sets the blend mode before every pass. Pass 0's `glDisable(GL_BLEND)`
  therefore sits inside the gate rather than in front of it.
- **A skipped pass and a run pass must leave identical GL state**, or the glow
  would flicker between the two. This is why `renderEmissionPass` restores the
  viewport *box* it queried rather than `(0, 0, viewportWidth, viewportHeight)`
  - the two are the same only while the caller's viewport starts at the origin
  and fills the target, and the difference was invisible while every frame
  overwrote it the same way.

The pack half of `packLightBlocks` gets the **opposite** treatment: its inputs
are narrow and visible (`mEffectiveSegments` and `NeonConfig::arcs`, nothing
else), so it is gated on exactly those two rather than on any config change -
the common animation is an intensity or geometry sweep that touches neither.
That flag **accumulates** rather than being assigned, and the difference is not
subtle: `AddRenderer` calls `OnConfigChanged` before the first `Render`, usually
with a config that matches the defaults it is diffed against, so an assignment
would clear the flag's initial `true`, no `Render` would ever pack, and the two
UBOs would be bound with no data store at all. The bind itself stays
unconditional - `glBindBufferBase` writes global context state this class does
not own between frames.

Pass 0 cannot fail: its target is secured at `Initialize` (section 5), so it
returns `void`; the passes whose buffers are allocated per frame (the gather,
pass 1b) return whether they drew, and a failure skips the composites that
would read them.

### Pass contract

State splits into two kinds, and they have opposite owners.

**Modes belong to `Render`.** Blend enable and blend function are properties of
the *phase*, not of a pass: every offscreen pass draws UNBLENDED - the tables,
the gather and the field bakes are data, and pass 1b's buffer was just cleared
and is covered once per texel - while the fill, the blit and the edge ring
composite premultiplied, and the renderer hands the world back on straight
alpha. `Render` sets the mode immediately before each pass that depends on
one, and **no pass touches `GL_BLEND` at all**. Two consequences worth having:

- The whole blend timeline reads top-to-bottom in one function.
- Setting the mode per phase, rather than relying on carry-over from an earlier
  pass, means the pass order can change without silently breaking compositing.

Each pass states the mode it needs as an `@pre` on its declaration.

**Excursions belong to the pass.** `renderEmissionPass` and
`renderGlowCoverPass` bind their own render targets, so each captures the
target it was handed (`RenderTargetState::Capture`) and restores exactly that -
**not** framebuffer 0. The target is not always the default framebuffer: an
offscreen frame capture (`OffscreenCapture`) binds a real FBO, so restoring 0
would redirect the rest of the frame to the window and leave the capture empty.

The viewport travels with the target - `Framebuffer::Bind()` sets both, since a
target without its viewport is a half-configured state - so the pass restores
it too, and it restores it the same way it restores the framebuffer: by
**capture**, `glGetIntegerv(GL_VIEWPORT)` on the way in and the same four
integers on the way out.

An earlier version reconstructed it instead, as
`glViewport(0, 0, viewportWidth, viewportHeight)`, which `BaseRenderer::Render`
explicitly permits: its `@pre` fixes the viewport at `(0, 0, viewportWidth,
viewportHeight)` and says a renderer may restore it by reconstruction rather
than by querying. **Under that precondition the two produce identical values**,
so this is a preference, not a defect that was fixed. In particular it does not
make a sub-viewport work - the shaders read `gl_FragCoord` in window
coordinates against uniforms computed as if the origin were (0, 0), so a
sub-viewport renders displaced whatever this code restores. That limitation is
unchanged; `implementation.md` section 11 still describes it.

What capture buys is one fewer assumption to carry. It costs a single
static-state `glGet` on a path that only runs when the table is stale, and it
is worth slightly more on pass 0 than elsewhere precisely because that pass can
now be **skipped**: anything a run frame leaves behind that a skipped frame
does not is a difference the glow can show, and "hand back what you found"
needs no precondition to hold for the two to agree.

The rest of the offscreen phase - passes 1a, 1f, 1b or 1c, and 1r - binds one
buffer after another for the passes on the caller's framebuffer to read, so
no one of them can hand the target back: `Render` captures the caller's target
once (`RenderTargetState::Capture`), before the phase and only on a frame that
draws anything offscreen, and restores it after the last of those passes. The
lens flare's scaled path follows the same rule around its blit.

The distinction is whether the pass *goes somewhere and comes back* (an
excursion, which only it can undo correctly) or merely *needs the world in a
certain state* (a mode, which the schedule owns).

## 4. The gather

The gather loop gained `uEmission` and lost `arcInside` entirely. It is
`gatherPerimeter` in `neon-gather.frag` (pass 1a) - it ran inside `neon.frag`
when the pre-pass landed, and became a pass of its own later. The loop body
(the branch with segments) is now:

```glsl
vec2  dv = p - uLoopSamples[i].xy;
float dd = dot(dv, dv);
float g  = 1.0 / (dd + kc2);

vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);
vec4 e1 = texelFetch(uEmission, ivec2(i, 1), 0);

acc      += e0.rgb * g;   wsumLit  += e0.a * g;
segAcc   += e1.rgb * g;   wsumSegW += e1.a * g;
```

Beyond the removed arithmetic, three structural things went with it:

- **Two nested dynamic loops.** `uArcCount` and `uSegmentCount` are uniforms,
  not constants, so the compiler could not unroll through them.
- **A serial reduction.** `if (mask > bestMask)` made each arc iteration depend
  on the previous one.
- **A loop-carried dependency.** `ti += dti; si += dti;` forced the iterations
  to stay ordered.

`texture()` also became `texelFetch()`: no LOD derivatives, no wrap math, no
filtering, and the 2 KB table stays resident in texture cache.

The three LUT samplers stay bound to the shading (`neon.frag`) - not for the
gather, but for the **colour-stop alpha**, which is read pointwise at the
fragment's own perimeter position. See §6.

**The gather itself stays.** It is the part the SDF cannot replace: `d` from
`sdRoundBox` is the distance to the *nearest* perimeter point, a `1/D^3`
falloff, while integrating the kernel along the perimeter yields the `1/D^2`
neon falloff, correct soft caps at arc ends, and correct brightening where two
perimeter stretches are both near a fragment.

## 5. Texture format

`mEmission.buffer` asks for `GL_RGBA16F` / `GL_HALF_FLOAT` / `GL_NEAREST`, from
a candidate list in preference order (`EMISSION_FORMATS`).

- **Float** because both rows exceed 1.0 in ordinary use: row 1 sums stacked
  `SegmentBoost::boost` values (absolute peak brightness), and row 0 carries
  `Arc::intensity`.
- **NEAREST** because the consumer uses `texelFetch` and adjacent texels are
  unrelated perimeter samples - and the two rows are different quantities
  entirely, so filtering across them is meaningless.

GLES 3.0 exposes float colour-renderability only through an extension, so the
walk falls through to `GL_RGBA8` and logs one line naming both formats. The
fallback clamps highlights above 1.0 but is otherwise exact. Measured when the
pre-pass landed (forcing `RGBA8` on a GPU that has float targets): within 1-2
levels of the float table while every arc intensity and segment boost is at or
under 1.0, and up to 82 levels on a scene with a 2.5 boost, the clamp being the
whole cause.

Re-requesting a refused format would be expensive, not merely noisy:
`Framebuffer::Resize` treats a format change as a reallocation, so asking for
`GL_RGBA16F` every frame on a driver that refuses it destroys and recreates the
texture and FBO twice per frame - measured at 1218 allocations over a run that
should need one. What prevents it:

- **The walk runs once.** `NeonRenderer::Initialize` allocates the table
  (`ResizeInBestFormat` over `EMISSION_FORMATS`), and nothing resizes it per
  frame. The table's dimensions are compile-time constants
  (`NEON_MAX_LOOP_SAMPLES x 2` - it is allocated at the sample-count CEILING,
  and `numSamples` only bounds how much of it is read), so nothing a later call
  could discover has changed, and no flag has to remember a refused format
  (there was one, `mEmissionFloatUnavailable`, since removed). If no candidate
  allocates, `Initialize` fails and the effect reports it, rather than the
  renderer degrading silently to fill-only for the rest of the run.

This motivated the format parameters on `Framebuffer::Resize`:

```cpp
bool Resize(int width, int height,
            GLint internalFormat = GL_RGBA8, GLenum format = GL_RGBA,
            GLenum type = GL_UNSIGNED_BYTE, GLint filter = GL_LINEAR);
```

Defaults match the previous behaviour, so existing callers are unaffected. The
wrapper tracks `internalFormat` and `filter` alongside the size, so a format
change forces a reallocation instead of silently no-opping.

## 6. What is NOT baked

Anything that reads the **fragment's** own perimeter position `sPos`:

- the continuous arc / segment coverages (`emitCover`, `segCoverPt`),
- the filament gate,
- the **colour-stop alpha**.

Baking those at sample resolution would reintroduce exactly the quantisation
their pointwise evaluation exists to avoid.

The alpha is the reason `neon.frag` still binds `uGradientLUT`, `uSegmentLUT`
and `uArcLUT`. Those pointwise reads are `O(arcs + segments)` per fragment in
the worst case (only when arcs/segments carry their own stops; otherwise one
fetch), so the "cost independent of scene complexity" claim holds for the
gather but not quite for the whole shader. Folding alpha into a third emission
row sampled with filtering at `sPos` would close that gap - it is the obvious
next step and is not done here.

## 7. Results in brief

Measured when it landed (August 2026, a 1920 x 1080 rect in a 3840 x 2160
frame, mean of 210 frames, `glFinish` in the timed region; the GPU was not
recorded, and timings on it varied about 2x between sessions), against the
same tree without the pre-pass. Then there were two
renderers; read "full resolution" as `resolutionScale` 1.0 and "half
resolution" as 0.5 on the one renderer there is now. The comparison document
that held the full tables was retired on 2026-10-09 (it is in git history);
every neon frame has been re-measured many times since, so read these for the
shape, not the milliseconds:

| scene | full res before / after | half res before / after |
| ----- | ----------------------- | ----------------------- |
| 1 full arc | 24.84 / 20.77 ms (1.20x) | 9.50 / 5.96 ms (1.59x) |
| 1 partial arc + 1 segment | 40.61 / 20.76 ms (1.96x) | 12.61 / 5.76 ms (2.19x) |
| 8 arcs + 8 segments | 134.67 / 22.88 ms (5.89x) | 34.19 / 7.18 ms (4.76x) |

The table itself costs 2 KB (128 x 2 texels of `RGBA16F`), fixed.

**Performance.** Per-fragment cost went from `O(samples * (arcs + segments))`
to `O(samples)`. The visible consequence is flatness: the "after" timings
barely move as the scene grows from one arc to eight arcs plus eight segments,
where before they rose by more than 5x. The scaled path gains least,
because its fixed costs - FBO clear, black fill, full-res blit - do not shrink.

**Correctness.** Max delta **1 LSB** on under 0.1% of pixels, across three
scenes on both resolution paths. Two causes, both benign: `RGBA16F` storage of the
per-sample intermediates, and `si` now being computed directly
(`floor(gl_FragCoord.x) * invN`) instead of accumulated through a
128-iteration `si += dti` chain - the direct form being the more accurate of
the two.

There are no intentional behaviour changes. Unlike the reference
implementation, the segment filament gate was already continuous on this
branch, so nothing needed moving.

The one case not covered by those measurements is the `RGBA8` fallback (§5):
where the driver refuses float colour rendering, values above 1.0 clamp, and
both rows carry such values in ordinary use.

## 8. Maintenance rules

The whole thing rests on one invariant:

> Anything that is a pure function of `(si, uTime, config)` belongs in
> `neon-emission.frag`. Anything that reads `vPos` belongs in the main shader.

Practical consequences:

- **Adding a per-sample light property** goes in the pre-pass. It costs 2N
  fragments per frame, not 8 million.
- **Do not fold anything else into a row's `.rgb`.** Each row's packing is
  valid only because its colour and its weight share one denominator. A term
  with a different weight needs its own row.
- **Do not gate the filament from the gather.** Both lights read their coverage
  at `sPos`; a gather-derived gate brings back head quantisation and corner
  spill.
- **The packing has one consumer.** `neon-gather.frag` (pass 1a) is the only
  reader of the table, at every resolution scale, so a change to the row layout
  lands there and in `neon-emission.frag`.
- **Hand the pre-pass and the gather the same `uNumSamples`.** Texel `i` in the
  table has to be sample `i` in the gather; both go through
  `GetClampedNumSamples` for exactly that reason.
- **Adding a shader means three edits** - `lib/CMakeLists.txt`
  (`CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)`) plus `shaders/shaders.h.in`.
  `neon-emission.frag` needs `@NEON_TUNING@` because it uses `MAX_ARCS`,
  `MAX_SEGMENT_BOOSTS` and `NEON_MAX_LOOP_SAMPLES`.

Rules for the pass structure itself (§3):

- **A pass that retargets restores what it was handed**, framebuffer *and*
  viewport *and* blend - never framebuffer 0, never a forced `glEnable`, and by
  **capture** rather than reconstruction (§3, *Pass contract*). The target is a
  real FBO under `OffscreenCapture`, and a caller may legitimately be rendering
  unblended, or clipped by a scissor box of its own.
- **A pass that can be SKIPPED must leave the same state as one that ran.**
  This is what put pass 0's `glDisable(GL_BLEND)` inside its gate rather than
  in front of it, and the reason its viewport restore prefers capture over a
  reconstruction that only agrees while a precondition holds. Before gating any
  pass, enumerate the state it touches and check the skip path against it.
- **`Render` owns blend state; a pass owns its shader.** No pass sets
  `GL_BLEND` or a blend function itself.
- **Adding a pass means three places stay in step**: the header declaration,
  the .cpp definition, and `Render`'s call - the first two in the order the
  third runs the passes.

Rules for the frame-level gate (§3, *When pass 0 runs*):

- **A new input to the emission table must join its key.** Every uniform,
  texture and block `renderEmissionPass` hands the shader belongs in
  `EmissionInputs`, filled by `getEmissionInputs` - a value uniform by
  value, a texture or block by a version number that every write to it moves.
  That list sits beside the pass in the same file, which is what makes it safe
  where a list of config fields was not. A second time-varying uniform needs a
  term alongside the `uTime` one.
- **Do not key it on config fields.** The temptation is to compare the handful
  of `NeonConfig` fields the shader reads. That list would have to follow the
  shader through every LUT and block between the two, and a missed field is a
  silently stale ring. The upload counts follow the GPU objects themselves.
- **Do not make an upload count skip writes.** `UniformBuffer::SetData`
  skipping an identical upload is fine - the contents did not change - but a
  write path that changes a LUT's texels without going through `Upload` would
  leave the count, and so the table, behind.

## 9. Not done

- **Colour-stop alpha is still read pointwise** from the three LUTs, which
  leaves an `O(arcs + segments)` term per fragment in the worst case. A third
  emission row would remove it (§6).
- **The gather still visits every sample** for every fragment, including the
  far majority whose weight is negligible. Now that a fragment knows its own
  `sPos`, a windowed gather could cut that to ~24-32 samples for typical glow
  radii. The catch is `wsum`: it is a full-perimeter density normaliser, so
  windowing changes the denominator and needs care around small rects where a
  fragment genuinely sees the whole perimeter. This is the largest remaining
  win and it interacts directly with the size-invariance work.
