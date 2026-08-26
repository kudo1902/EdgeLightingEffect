# Perimeter emission pre-pass

Design notes for `lib/shaders/neon-emission.frag` and the pass structure it
introduces in `NeonRenderer` and `NeonOptimizedRenderer`.

The short version: the neon gather loop was recomputing an identical per-sample
table in every screen fragment. That table is now baked once per frame into a
small texture, and the gather reads it with `texelFetch`. Same picture; the
per-fragment cost stops scaling with the number of arcs and segments.

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

`Render` in both renderers is a pass schedule and nothing else: derive the
transform, then one call per pass, each in its own private method. Blend state
is owned by `Render`; each pass owns its shader, and the two that retarget the
framebuffer restore it themselves.

**`NeonRenderer::Render`**

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | (inline) | - | derives proj / center / mvp for the passes |
| - | `packLightBlocks` | - | UBO upload only |
| 0 | `renderEmissionPass` | `mEmissionBuffer` (N x 2) | `mFullVertexArray`, identity MVP |
| 1 | `renderOpaqueFill` | backbuffer | black rounded-rect fill (opaque modes only) |
| 2 | `renderNeonPass` | backbuffer | tight glow quad, `neon.frag` |
| dbg | `renderGradientLUTStrip` | backbuffer | colour-ring strip, unblended |
| dbg | `renderColorStopMarkers` | backbuffer | one disc per colour stop |

**`NeonOptimizedRenderer::Render`**

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | `packLightBlocks` | - | UBO upload only |
| 0 | `renderEmissionPass` | `mEmissionBuffer` | `mBlitVertexArray`, identity MVP |
| 1 | `renderHalfResNeonPass` | `mHalfResBuffer` | scaled glow quad, `neon-optimized.frag` |
| 2a | `renderOpaqueFill` | backbuffer | black rounded-rect fill (opaque modes only) |
| 2b | `renderBlitPass` | backbuffer | bilinear composite of the half-res FBO |

The arc and segment UBOs are packed **before** pass 0, because both the
pre-pass and the main pass read them - the pre-pass for the gathered emission,
the main pass for the continuous filament gate.

### One inversion, in NeonRenderer only

`NeonRenderer` executes **pass 1 before pass 0**. The emission table feeds only
pass 2, so deferring it past the `opaqueOnly` early-out lets the fill-only debug
mode skip a UBO upload and a draw it would never sample. This is safe only
because `renderEmissionPass` restores the framebuffer it was handed (see below);
against a version that restored framebuffer 0 it would have been a bug.

`NeonOptimizedRenderer` has no such inversion - both passes sit inside the same
`!opaqueOnly` guard, so it runs 0, 1, 2a, 2b in order.

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
it too, which is why it takes the viewport dimensions. Note the deliberate
asymmetry: the framebuffer is restored by **capture**, the viewport by
**reconstruction** (`glViewport(0, 0, viewportWidth, viewportHeight)`). That is
not sloppiness. `BaseRenderer::Render` documents the viewport origin as a
precondition every renderer relies on, and the shaders bake it in - they read
`gl_FragCoord` in window coordinates against uniforms computed as if the origin
were (0, 0). Capturing `GL_VIEWPORT` here would restore more precisely while
the rendering itself stayed wrong under a sub-viewport, advertising a
generality that does not exist. See architecture-design.md §9.

The distinction is whether the pass *goes somewhere and comes back* (an
excursion, which only it can undo correctly) or merely *needs the world in a
certain state* (a mode, which the schedule owns).

`renderHalfResNeonPass` is the deliberate non-excursion: it renders into
`mHalfResBuffer` for the next phase to consume rather than returning, so
`Render` performs that framebuffer transition, using the `targetFbo` it
captured before pass 0.

## 4. The main shader

`neon.frag` and `neon-optimized.frag` gained `uEmission` and lost `arcInside`
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

`mEmissionBuffer` asks for `GL_RGBA16F` / `GL_HALF_FLOAT` / `GL_NEAREST`.

- **Float** because both rows exceed 1.0 in ordinary use: row 1 sums stacked
  `SegmentBoost::boost` values (absolute peak brightness), and row 0 carries
  `Arc::intensity`.
- **NEAREST** because the consumer uses `texelFetch` and adjacent texels are
  unrelated perimeter samples - and the two rows are different quantities
  entirely, so filtering across them is meaningless.

GLES 3.0 exposes float colour-renderability only through an extension, so both
renderers fall back to `GL_RGBA8` and log a warning once. The fallback clamps
highlights above 1.0 but is otherwise exact - `docs/branch-vs-main-comparison.md`
section 3.5 measures how far it drifts.

The fallback **latches** in `mEmissionFloatUnavailable`, and that is load-bearing
rather than a log guard. `Framebuffer::Resize` treats a format change as a
reallocation, so re-requesting `GL_RGBA16F` every frame on a driver that refuses
it destroys and recreates the texture and FBO twice per frame - measured at 1218
allocations over a run that should need one.

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
where before they rose by more than 5x. The half-res renderer gains least,
because its fixed costs - FBO clear, black fill, full-res blit - do not shrink.

**Correctness.** Max delta **1 LSB** on under 0.1% of pixels, across three
scenes on both renderers. Two causes, both benign: `RGBA16F` storage of the
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
- **Keep the two consumers in step.** `neon.frag` and `neon-optimized.frag`
  share the pre-pass, so a change to the packing has to land in both.
  `neon-emission.frag` itself is shared verbatim.
- **Adding a shader means three edits** - `lib/CMakeLists.txt`
  (`CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)`) plus `shaders/shaders.h.in`.
  `neon-emission.frag` needs `@NEON_TUNING@` because it uses `MAX_ARCS`,
  `MAX_SEGMENT_BOOSTS` and `NEON_MAX_LOOP_SAMPLES`.

Rules for the pass structure itself (§3):

- **A pass that retargets restores what it was handed**, framebuffer *and*
  viewport *and* blend - never framebuffer 0, never a forced `glEnable`. The
  target is a real FBO under `OffscreenCapture`, and a caller may legitimately
  be rendering unblended.
- **`Render` owns blend state; a pass owns its shader.** The two passes that
  deviate (the emission pre-pass, the LUT-strip overlay) say so in their own
  comments.
- **Adding a pass means three places stay in step**: the header declaration
  (pass-number order), the .cpp definition order, and `Render`'s call order.
  Where call order deviates - `NeonRenderer` runs pass 1 before pass 0 - the
  reason is recorded at the declaration, not left to be rediscovered.

## 9. Not done

- **Colour-stop alpha is still read pointwise** from the three LUTs, which
  leaves an `O(arcs + segments)` term per fragment in the worst case. A third
  emission row would remove it (§6).
- **The pre-pass runs every frame** even though its output depends only on
  `(uTime, config)`. With a paused clock and an unchanged config the table is
  bit-identical frame to frame, and the renderers already have equality-gated
  rebuild machinery to hang a skip on.
- **The gather still visits every sample** for every fragment, including the
  far majority whose weight is negligible. Now that a fragment knows its own
  `sPos`, a windowed gather could cut that to ~24-32 samples for typical glow
  radii. The catch is `wsum`: it is a full-perimeter density normaliser, so
  windowing changes the denominator and needs care around small rects where a
  fragment genuinely sees the whole perimeter. This is the largest remaining
  win and it interacts directly with the size-invariance work.
