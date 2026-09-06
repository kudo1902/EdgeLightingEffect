# Perimeter emission pre-pass

Design notes for `lib/shaders/neon-emission.frag` and the pass structure it
introduces in `NeonRenderer`.

> Written when the half-res path was a second renderer,
> `NeonOptimizedRenderer`, with its own copy of the shader. It is one
> renderer now, drawing at `NeonConfig::resolutionScale`; the pass tables
> below have been updated, and the two-consumer warnings that used to run
> through this document now have a single consumer to keep honest. See
> `neon-unification-plan.md`.

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
is owned by `Render`; each pass owns its shader, and those that retarget the
framebuffer restore it themselves.

**`NeonRenderer::Render`** - one schedule, both resolution paths. `scaled` is
`resolutionScale < 1.0`; it changes only where pass 1 lands and whether pass 2b
runs, never the order or the guards.

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | (inline) | - | derives proj / center / mvp, in SCALED space |
| 2a | `renderOpaqueFill` | caller's framebuffer | black rounded-rect fill (opaque modes only), always full-res; a band ring bounds it, and a coverage-1 fill (`ALL`, or `BOTH` with both cutoffs disabled) is a scissored `glClear` with no draw unless depth / stencil testing is on |
| - | `packLightBlocks` | - | UBO upload only; the pack is gated, the bind is not |
| 0 | `renderEmissionPass` | `mEmissionBuffer` (N x 2, allocated at `Initialize`) | `mFullVertexArray`, identity MVP; runs only when the table is stale |
| 1 | `renderNeonPass` | caller's framebuffer, or `mScaledBuffer` when scaled | tight glow quad, `neon.frag` |
| 2b | `renderBlitPass` | caller's framebuffer | bilinear composite of `mScaledBuffer`; scaled path only |

At `resolutionScale` 1.0 the last row does not run and pass 1 IS the composite.
The debug overlays that used to close this table are a separate layer now -
`DebugRenderer`, drawn after this renderer, always at full resolution.

The arc and segment UBOs are packed **before** pass 0, because both the
pre-pass and the main pass read them - the pre-pass for the gathered emission,
the main pass for the continuous filament gate.

### When pass 0 runs

The pre-pass hoists the gather's fragment-invariant half out of every
*fragment*. The same argument hoists it out of every *frame*: the table is a
pure function of `(si, uTime, config)`, and `mEmissionBuffer` is allocated once
for the renderer's lifetime and written by nothing else, so a frame that moves
neither input can read the texels the last bake left. `isEmissionTableStale`
is that test, and it has exactly two terms.

- **`mEmissionDirty`**, set by `OnConfigChanged` on **any** config change.
  Deliberately not a narrow gate: the table reads a wide and indirect slice of
  the config - `hueRotationRate`, `numSamples`, all three LUT textures and both
  light UBOs - so a missed field would be a silently stale ring, while a spare
  rebuild is one small pass. It is also set from `NeonRenderer::Update` when
  `GradientRingLUT::Tick` reports a re-upload: a cross-fade moves the ring
  texture with no config change to announce it, so `Tick` returns whether it
  uploaded and the flag accumulates (`|=`, never `=`, or a settled ring would
  clear a change made earlier in the same frame). It starts `true`, because the
  buffer holds undefined texels until the first bake and no config change is
  guaranteed before the first frame.
- **`uTime`**, which reaches `neon-emission.frag` in exactly one place:
  `float ti = si - uTime * uHueRotationRate`. At a rate of 0 the product is
  exactly zero, so time drops out of the table altogether and a still ring
  rebakes nothing however the clock runs. At any other rate the comparison is
  `time != mEmissionTime`, exact rather than tolerant, because `time` is fed
  straight back from the last bake rather than recomputed.

`renderEmissionPass` records both (`mEmissionDirty`, `mEmissionTime`) on its
way out, so the staleness test always reads a snapshot written by the only
thing that ever writes the buffer.

Two consequences worth keeping in mind when changing this:

- **Skipping pass 0 must leave nothing behind for pass 1.** It does:
  `renderNeonPass` binds all three LUTs *and* the emission texture itself, and
  `Render` re-asserts the blend mode before it. Pass 0's `glDisable(GL_BLEND)`
  therefore moved inside the gate rather than in front of it.
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

### One inversion: pass 2a runs first

`Render` calls **pass 2a before pass 0**, the one place call order and pass
numbering differ. Two reasons, and they compound:

- The fill depends on nothing above it and must end up UNDER the glow either
  way. Hoisting it is what lets a single schedule serve both resolution paths:
  the direct path needs it down before the gather composites over it, and the
  scaled path does not care, because its gather goes to an independent buffer.
- It puts the fill ahead of the `debug.opaqueOnly` early-out, so the fill-only
  mode skips a UBO upload and two draws it would never sample.

Pass 0 cannot fail: its target is secured at `Initialize` (section 5), so it
returns `void` and only pass 1's scaled-buffer allocation can still skip the
composite.

This is safe only because `renderEmissionPass` restores the framebuffer it was
handed (see below); against a version that restored framebuffer 0 it would have
been a bug.

Declaration order in the headers follows the **pass numbering**, not the call
order, and matches the definition order in the .cpp. The inversion above is
noted at the declaration site so the two do not silently drift.

### Pass contract

State splits into two kinds, and they have opposite owners.

**Modes belong to `Render`.** Blend enable and blend function are properties of
the *phase*, not of a pass: the fill and glow composite premultiplied, the stop
markers composite straight alpha, the LUT strip draws unblended, and the
renderer hands the world back on straight alpha. `Render` sets the mode
immediately before each pass that depends on one, and **no pass touches
`GL_BLEND` at all**. Two consequences worth having:

- The whole blend timeline reads top-to-bottom in one function.
- Setting the mode per phase, rather than relying on carry-over from an earlier
  pass, means the pass order can change without silently breaking compositing.

Each pass states the mode it needs as an `@pre` on its declaration.

**Excursions belong to the pass.** `renderEmissionPass` binds its own render
target, so it captures `Framebuffer::GetBoundId()` and restores exactly that -
**not** framebuffer 0. The target is not always the default framebuffer: an
offscreen frame capture (`OffscreenCapture`) binds a real FBO, and the gather
has no bind of its own, so restoring 0 would redirect the whole neon pass to
the window and leave the capture empty.

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
unchanged; architecture-design.md §9 still describes it.

What capture buys is one fewer assumption to carry. It costs a single
static-state `glGet` on a path that only runs when the table is stale, and it
is worth slightly more on pass 0 than elsewhere precisely because that pass can
now be **skipped**: anything a run frame leaves behind that a skipped frame
does not is a difference the glow can show, and "hand back what you found"
needs no precondition to hold for the two to agree.

The same rule covers the two scaled-path composites (`NeonRenderer`'s pass 2b
and `LensFlareRenderer`'s blit), where it buys only the consistency. Those
retarget from `Render` rather than from inside a pass, so each reads the box
once at the head of `Render`, next to `targetFbo` and on the scaled path only -
the direct path never retargets and pays no query.

The distinction is whether the pass *goes somewhere and comes back* (an
excursion, which only it can undo correctly) or merely *needs the world in a
certain state* (a mode, which the schedule owns).

`renderNeonPass` is the deliberate non-excursion: on the scaled path it renders
into `mScaledBuffer` for pass 2b to consume rather than returning, so `Render`
performs that framebuffer transition, using the `targetFbo` and viewport box it
captured before pass 0.

## 4. The main shader

`neon.frag` gained `uEmission` and lost `arcInside`
entirely. The loop body is now:

```glsl
vec2  dv = vPos - uLoopSamples[i].xy;
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

The three LUT samplers stay bound to the main shader - not for the gather, but
for the **colour-stop alpha**, which is read pointwise at the fragment's own
perimeter position. See §6.

**The gather itself stays.** It is the part the SDF cannot replace: `d` from
`sdRoundBox` is the distance to the *nearest* perimeter point, a `1/D^3`
falloff, while integrating the kernel along the perimeter yields the `1/D^2`
neon falloff, correct soft caps at arc ends, and correct brightening where two
perimeter stretches are both near a fragment.

## 5. Texture format

`mEmissionBuffer` asks for `GL_RGBA16F` / `GL_HALF_FLOAT` / `GL_NEAREST`, from
a candidate list in preference order (`EMISSION_FORMATS`).

- **Float** because both rows exceed 1.0 in ordinary use: row 1 sums stacked
  `SegmentBoost::boost` values (absolute peak brightness), and row 0 carries
  `Arc::intensity`.
- **NEAREST** because the consumer uses `texelFetch` and adjacent texels are
  unrelated perimeter samples - and the two rows are different quantities
  entirely, so filtering across them is meaningless.

GLES 3.0 exposes float colour-renderability only through an extension, so the
walk falls through to `GL_RGBA8` and logs one line naming both formats. The
fallback clamps highlights above 1.0 but is otherwise exact -
`docs/branch-vs-main-comparison.md` section 3.5 measures how far it drifts.

Re-requesting a refused format would be expensive, not merely noisy:
`Framebuffer::Resize` treats a format change as a reallocation, so asking for
`GL_RGBA16F` every frame on a driver that refuses it destroys and recreates the
texture and FBO twice per frame - measured at 1218 allocations over a run that
should need one. Two things prevent it:

- **The walk starts where the buffer already is.** A live buffer is asked for
  the format it is holding, which `Resize` early-outs on; only a buffer with no
  attachment starts at the top of the list. The buffer's own state is the record
  of how far down the list a previous call got - there is no flag tracking it.
  (There was: `mEmissionFloatUnavailable`, since removed.)
- **The walk runs once.** `resizeEmissionBuffer` is called from
  `NeonRenderer::Initialize`, not per frame. The table's dimensions are
  compile-time constants (`NEON_MAX_LOOP_SAMPLES x 2` - it is allocated at the
  sample-count CEILING, and `numSamples` only bounds how much of it is read), so
  nothing a later call could discover has changed. If no candidate allocates,
  `Initialize` fails and the effect reports it, rather than the renderer
  degrading silently to fill-only for the rest of the run.

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

Full before/after tables - performance, visuals and memory, with the
methodology and its caveats - live in
[`emission-prepass-comparison.md`](emission-prepass-comparison.md). That
document is the single source for the numbers; this section only states the
shape of the result so the design rationale above stands on its own.

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
- **The packing has one consumer.** `neon.frag` is the only reader of the
  table, at either resolution scale, so a change to the row layout lands in one
  place. (This used to say "keep the two consumers in step" - the second
  consumer was `neon-optimized.frag`, now merged away.)
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
- **`Render` owns blend state; a pass owns its shader.** The two passes that
  deviate (the emission pre-pass, the LUT-strip overlay) say so in their own
  comments.
- **Adding a pass means three places stay in step**: the header declaration
  (pass-number order), the .cpp definition order, and `Render`'s call order.
  Where call order deviates - `NeonRenderer` runs pass 2a before pass 0 - the
  reason is recorded at the declaration, not left to be rediscovered.

Rules for the frame-level gate (§3, *When pass 0 runs*):

- **A new input to the emission table must invalidate it.** Anything reaching
  `neon-emission.frag` from the config is already covered, because
  `mEmissionDirty` is set on *any* config change. Anything reaching it from
  somewhere else is not: a second time-varying uniform would need a term
  alongside the `uTime` one, and a second texture that moves without a config
  change would need what `GradientRingLUT::Tick` got - a return value saying it
  re-uploaded, accumulated into `mEmissionDirty` from `Update`.
- **Do not narrow `mEmissionDirty`.** The temptation is to gate it on the
  handful of fields the shader reads. The failure mode is a silently stale
  ring, which is far worse than the one small pass a spare rebuild costs, and
  the field list would have to be kept in step with a shader in another
  language. The narrow gate next door (`mLightBlocksDirty`) earns its narrowness
  by having two visible inputs in the same file.

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
