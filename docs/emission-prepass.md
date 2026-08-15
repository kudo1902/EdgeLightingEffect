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

Both renderers run the pre-pass first, because it retargets the framebuffer and
the viewport.

**`NeonRenderer::Render`**

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | `packLightBlocks` | - | UBO upload only |
| 0 | `renderEmissionPass` | `mEmissionBuffer` (N x 2) | `mFullVertexArray`, identity MVP |
| 1 | opaque fill (opaque modes only) | backbuffer | black rounded-rect fill |
| 2 | neon gather | backbuffer | tight glow quad, `neon.frag` |
| dbg | LUT strip / stop markers | backbuffer | unchanged |

`NeonOptimizedRenderer` is the same with pass 1 rendering into the half-res FBO
and a blit afterwards.

The arc and segment UBOs are packed **before** pass 0, because both the
pre-pass and the main pass read them - the pre-pass for the gathered emission,
the main pass for the continuous filament gate.

### Pass contract

`renderEmissionPass` binds its own render target and returns with the default
framebuffer and the full viewport bound - which is why it takes the viewport
dimensions. It also turns `GL_BLEND` off for its duration (a table write is not
a composite; blending would mix this frame's emission into last frame's) and
back on afterwards.

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
renderers fall back to `GL_RGBA8` and log a warning once; `mEmissionIsFloat`
records which was obtained. The fallback clamps highlights above 1.0 but is
otherwise exact.

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

## 7. Measured results

3840x2160 framebuffer, 1920x1080 rect, mean of 210 frames with `glFinish`
inside the timed region (GPU time, not command submission).

| scene | renderer | before | after | |
| ----- | -------- | ------ | ----- | - |
| 1 full arc, no segments | full-res | 27.62 ms | 10.47 ms | 2.6x |
| 1 partial arc + 1 segment | full-res | 40.80 ms | 10.27 ms | 4.0x |
| 8 arcs + 8 segments | full-res | 158.21 ms | 10.63 ms | 14.9x |
| 1 full arc, no segments | half-res | 6.48 ms | 4.87 ms | 1.3x |
| 1 partial arc + 1 segment | half-res | 10.82 ms | 4.12 ms | 2.6x |
| 8 arcs + 8 segments | half-res | 23.02 ms | 4.81 ms | 4.8x |

The headline is not any single row: it is that the "after" column barely moves
across scene complexity (10.27 - 10.63 ms full-res, 4.12 - 4.87 ms half-res)
where before it went 27.6 -> 158.2 and 6.5 -> 23.0. Per-fragment cost is now
`O(samples)` instead of `O(samples * (arcs + segments))`.

The half-res renderer gains least because its fixed costs - FBO clear, black
fill, full-res blit - do not shrink.

### Correctness

Frame dumps before vs. after, deterministic scene and fixed clock step, at
3840x2160 (33.2M bytes per frame):

| scene | renderer | differing bytes | max delta |
| ----- | -------- | --------------- | --------- |
| 1 | full-res | 27,927 (0.08%) | 1 |
| 2 | full-res | 14,089 (0.04%) | 1 |
| 3 | full-res | 14,798 (0.04%) | 1 |
| 1 | half-res | 30,730 (0.09%) | 1 |
| 2 | half-res | 15,740 (0.05%) | 1 |
| 3 | half-res | 14,725 (0.04%) | 1 |

Max delta 1 everywhere - one LSB, from `RGBA16F` storage and from `si` being
computed directly (`floor(gl_FragCoord.x) * invN`) instead of accumulated
through a 128-iteration `si += dti` chain. The direct form is the more accurate
of the two. There are no behaviour changes: unlike the reference
implementation, the segment filament gate was already continuous on this
branch, so nothing needed moving.

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
