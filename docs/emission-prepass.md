# Perimeter emission pre-pass

Design notes for `lib/shaders/neon-emission.frag` and the two-pass structure it
introduces in `NeonRenderer` and `NeonOptimizedRenderer`.

The short version: the neon gather loop was recomputing an identical 128-entry
table in every screen fragment. That table is now baked once per frame into a
one-texel-per-sample texture, and the gather loop reads it with `texelFetch`.
The output is the same picture; the per-fragment cost stops scaling with the
number of arcs and segments.

## 1. Motivation

Each fragment of the neon quad runs a gather over `NEON_MAX_LOOP_SAMPLES`
perimeter samples. The old loop body did four things per sample:

1. Scan `uArcCount` arcs, evaluate `arcInside` for each (four `smoothstep`s),
   and keep the winner by `mask * intensity`.
2. Fetch the winner's colour from `uArcLUT` or `uGradientLUT`.
3. Scan `uSegmentCount` segments, evaluate a Gaussian bell for each, and fetch
   from `uSegmentLUT` where the segment carries its own stops.
4. Accumulate the distance-weighted halo, bloom and colour.

Only step 4 depends on the fragment. Steps 1-3 read the sample's perimeter
position `si`, the clock, and the config - nothing else:

```glsl
float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;  // si = i / N
float bell = seg.z * exp(-e * e);                                 // rel = si - seg.x
baseColI   = texture(uGradientLUT, vec2(ti, 0.5)).rgb;            // ti = si - uTime * rate
```

At the demo's defaults the neon quad clips to a full 3840x2160 viewport, so the
loop body executes about **1.06 billion** times per frame (8.3M fragments x 128
samples). The table it was deriving has 128 entries. The pre-pass computes it
128 times instead.

This is loop-invariant hoisting, except the invariant loop is the implicit one
over fragments. You cannot hoist out of that inside a shader, so the work has to
become a separate draw.

## 2. What the pre-pass emits

One RGBA texel per perimeter sample:

| channel | contents |
| ------- | -------- |
| `.rgb`  | `arcColour * arcMask * uIntensity + SUM(segColour * bell)` |
| `.a`    | `cover = max(arcMask, min(SUM(bell), 1))` |

```glsl
fragColor = vec4(arcCol * (arcW * uIntensity) + segSum, cover);
```

### Why it fits in one texel

The old shader carried two colour accumulators and combined them at the end:

```glsl
vec3 col      = acc    / max(wsum, WSUM_EPSILON);
vec3 segCol   = segAcc / max(wsum, WSUM_EPSILON);
vec3 lightCol = col * uIntensity + segCol;
```

Expanding, with `I` = `uIntensity`, `Ci` = arc colour, `Si` = the segment sum,
and `gi` = the gather weight:

```
lightCol = (SUM Ci*arcWi*gi)*I / SUM gi  +  (SUM Si*gi) / SUM gi
         = SUM (Ci*arcWi*I + Si)*gi / SUM gi
```

`I` is a uniform and both halves ride the **same** weight `gi`, so the bracket
is a per-sample constant. Pre-multiplying intensity into the arc term and adding
the segment term collapses two vec3 accumulators into one:

```
Ei = arcColour_i * arcMask_i * uIntensity + SUM_s segColour_is * bell_is
```

With `cover` as the fourth channel that is exactly four floats.

The last thing standing in the way was `wsumSeg`, which needed the segment
coverage as a separate per-sample value. Moving the segment filament gate to the
continuous path (§6) removed it, which is what freed the packing.

## 3. Pass structure

Both renderers run the pre-pass first, because it retargets the framebuffer and
the viewport. `Render()` is then a pass list and nothing else - the derived
transform, then one call per pass, with each pass in its own private method.

**`NeonRenderer::Render`**

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | `packLightBlocks` | - | UBO upload only |
| 0 | `renderEmissionPass` | `mEmissionBuffer` (`NEON_MAX_LOOP_SAMPLES` x 1) | `mFullVertexArray`, identity MVP |
| 1 | `renderOpaqueFill` (opaque modes only) | backbuffer | black rounded-rect fill |
| 2 | `renderNeonPass` | backbuffer | tight glow quad, `neon.frag` |
| dbg | `renderGradientLUTStrip` | backbuffer | colour-ring strip, unblended |
| dbg | `renderColorStopMarkers` | backbuffer | one disc per colour stop |

**`NeonOptimizedRenderer::Render`**

| pass | method | target | draw |
| ---- | ------ | ------ | ---- |
| - | `packLightBlocks` | - | UBO upload only |
| 0 | `renderEmissionPass` | `mEmissionBuffer` | `mBlitVertexArray`, identity MVP |
| 1 | `renderHalfResNeonPass` | `mHalfResBuffer` | scaled glow quad, `neon-optimized.frag` |
| 2a | `renderOpaqueFill` (opaque modes only) | backbuffer | black rounded-rect fill |
| 2b | `renderBlitPass` | backbuffer | bilinear composite of the half-res FBO |

The arc and segment UBOs are packed **before** pass 0, because both the pre-pass
and the main pass read them - the pre-pass for the gathered emission, the main
pass for the continuous filament gate.

### Pass contract

Every `render*Pass` method binds its own shader **and its own render target**,
and returns with the default framebuffer and the full viewport bound. That is why
`renderEmissionPass` and `renderHalfResNeonPass` take the viewport dimensions:
they retarget the framebuffer, so they are the ones that have to put it back.

Blend state is owned by `Render()`. Two passes deviate and say so in their
header comment: `renderEmissionPass` turns `GL_BLEND` off for its duration (a
table write is not a composite) and back on after, and
`renderGradientLUTStrip` leaves it off so the strip stays readable over the
tone-mapped glow.

## 4. The pre-pass shader

`gl_FragCoord.x` is the sample index:

```glsl
float invNumSamples = 1.0 / float(max(uNumSamples, 1));
float si = floor(gl_FragCoord.x) * invNumSamples;
float ti = si - uTime * uHueRotationRate;
```

`si` is computed directly rather than accumulated, so it is also more accurate
than the old `si += dti` chain over 128 iterations.

`uNumSamples` is a uniform rather than `NEON_MAX_LOOP_SAMPLES` because
`NeonOptimizedRenderer` has a runtime sample-count slider: sample `i` sits at
`i / uNumSamples`, matching how the CPU walks `GetPointOnRectangle` when it fills
`LoopSamplesBlock`. `NeonRenderer` always passes `NEON_MAX_LOOP_SAMPLES`. Texels
past `uNumSamples` are written but never read.

Uniform interface:

| uniform | source |
| ------- | ------ |
| `uMVP` | identity (the NDC quad is only there to rasterise the row) |
| `uTime`, `uHueRotationRate`, `uIntensity` | `Config::neon` |
| `uPerimeter` | `mPerimeter` (scaled to FBO space in the optimized renderer) |
| `uNumSamples` | fixed, or `optimizedNeon.numSamples` |
| `uGradientLUT`, `uSegmentLUT`, `uArcLUT` | units 0 / 1 / 2 |
| `SegmentBlock`, `ArcBlock` | bindings 0 and 2, shared with the main pass |

`LoopSamplesBlock` is deliberately **not** bound - the pre-pass works in
perimeter-parameter space and never needs the sample's pixel position.

The body is the old inner code moved verbatim: same winner-take-all, same bells,
same LUT fetches with the same filtering. Being the same code reading the same
textures is what makes the result exact by construction. A CPU-side bake would
have had to replicate bilinear `REPEAT` LUT sampling and could drift.

## 5. The main shader

`neon.frag` lost three samplers (`uGradientLUT`, `uSegmentLUT`, `uArcLUT`) and
three scalars (`uIntensity`, `uTime`, `uHueRotationRate`), and gained
`uEmission`. `arcInside` moved out entirely. The loop body is now:

```glsl
vec2  dv = vPos - uLoopSamples[i].xy;
float dd = dot(dv, dv);
float g  = 1.0 / (dd + kg2);

vec4 emission = texelFetch(uEmission, ivec2(i, 0), 0);

acc   += emission.rgb * g;
wsum  += g;
glow  += emission.a * g * sqrt(g);   // -> ~1/D^2 neon halo
bloom += emission.a / (dd + bw2);    // -> ~1/D   wide spill
```

Beyond the removed arithmetic, three structural things went with it:

- **Two nested dynamic loops.** `uArcCount` and `uSegmentCount` are uniforms, not
  constants, so the compiler could not unroll or software-pipeline the outer
  128-loop through them.
- **A serial reduction.** `if (mask > bestMask)` made each arc iteration depend
  on the previous one.
- **A loop-carried dependency.** `ti += dti; si += dti;` forced the 128
  iterations to stay ordered.

Register pressure dropped by roughly half (`bestMask`, `bestIdx`, `baseColI`,
`segFallback`, `segAcc`, `segMask`, `ti`, `si`, `dti` are all gone), which raises
occupancy and therefore the GPU's ability to hide fetch latency.

`texture()` also became `texelFetch()`: no implicit derivatives for LOD, no
`REPEAT` wrap math, no bilinear filter, and the 2 KB emission texture stays
resident in texture cache.

**This gather stays.** It is the one part the SDF cannot replace: `d` from
`sdRoundBox` is the distance to the *nearest* perimeter point, a `1/D^3` falloff,
while integrating the kernel along the perimeter yields the `1/D^2` neon falloff,
correct soft caps at arc ends, and correct brightening where two perimeter
stretches are both near a fragment (corners, thin rects).

## 6. Behaviour changes

Two deliberate ones. Everything else is bit-level identical up to float rounding.

### Segment filament gate is now continuous

Arcs were already gated at the fragment's own perimeter position. Segments were
the odd one out, deriving their gate from the gather:

```glsl
float segFraction  = wsumSeg / max(wsum, WSUM_EPSILON);
float filamentGate = smoothstep(0.5, 1.0, segFraction);
```

That is a proximity-weighted average of 128 stepped samples approximating a value
that can be evaluated exactly, since a segment's bell is a pure function of
perimeter position:

```glsl
float segCover = 0.0;
for (int s = 0; s < uSegmentCount; s++) {
    float rel = sPos - seg.x;
    rel      -= floor(rel + 0.5);
    float e   = rel * seg.y;
    segCover  = max(segCover, seg.z * exp(-e * e));
}
float filamentGate = max(contCover, smoothstep(0.5, 1.0, min(segCover, 1.0)));
```

`sPos` comes from `perimeterPosition(vPos)`, the geometric inverse of
`GetPointOnRectangle`. Keeping the `smoothstep(0.5, 1.0, ...)` preserves the old
gate's shape. What changes: the segment's filament head no longer quantises to
the gather points (visible stepping on a slow tracer) and no longer spills onto
the corner preceding its tail - the same two defects the arc gate was moved to
fix earlier.

### Arc end feather is pixel-space with a sample floor

```glsl
float fHead = max(HEAD_FEATHER_PX / max(uPerimeter, 1.0), invNumSamples);
float fTail = 0.25 * fHead;
```

The old `fHead = invNumSamples` was sample-relative, so on a small rect the halo
feathered over ~3 px while the filament feathered over the fixed
`HEAD_FEATHER_PX` (14 px). The floor preserves the anti-stepping property -
adjacent samples' fade-in ranges have to stay contiguous - so on typical geometry
the floor wins and nothing changes.

## 7. Texture format

`mEmissionBuffer` asks for `GL_RGBA16F` / `GL_RGBA` / `GL_HALF_FLOAT` /
`GL_NEAREST`. Float is needed because `SegmentBoost::boost` is an absolute peak
brightness and several segments can stack, so `.rgb` above 1.0 is normal.

GLES 3.0 only exposes float colour-renderability through an extension, so both
renderers fall back to `GL_RGBA8` and log a warning; `mEmissionIsFloat` records
which one was obtained. The fallback clamps segment highlights above 1.0 but is
otherwise exact.

This is what motivated the format parameters on `Framebuffer::Resize`:

```cpp
bool Resize(int width, int height,
            GLint internalFormat = GL_RGBA8, GLenum format = GL_RGBA,
            GLenum type = GL_UNSIGNED_BYTE, GLint filter = GL_LINEAR);
```

Defaults match the previous behaviour, so existing callers are unaffected. The
wrapper now tracks `internalFormat` and `filter` alongside the size, so a format
change forces a reallocation instead of silently no-opping.

## 8. Measured results

3840x2160 framebuffer, 1920x1080 rect, mean of 210 frames with `glFinish` inside
the timed region (so this is GPU time, not command submission).

| scene | renderer | before | after | |
| ----- | -------- | ------ | ----- | - |
| 1 full arc, no segments | full-res | 49.5 ms | 14.6 ms | 3.4x |
| 1 partial arc + 1 segment | full-res | 65.2 ms | 14.7 ms | 4.4x |
| 8 arcs + 8 segments | full-res | 261.3 ms | 15.6 ms | 16.8x |
| 1 full arc, no segments | half-res | 7.75 ms | 5.70 ms | 1.4x |
| 1 partial arc + 1 segment | half-res | 9.53 ms | 5.65 ms | 1.7x |

The headline is not any single row: it is that the full-res "after" column barely
moves (14.6 -> 15.6 ms) from 1 arc to 8 arcs + 8 segments, where before it went
49 -> 261 ms. Per-fragment cost is now `O(samples)` instead of
`O(samples * (arcs + segments))`.

The half-res renderer gains least because its fixed costs - FBO clear, black
fill, full-res blit - do not shrink, and it was already running a quarter of the
fragments at 64 samples.

### Correctness

Frame dumps before vs. after, deterministic scene and fixed clock step:

| scene | renderer | result |
| ----- | -------- | ------ |
| full arc, sharp corners | full-res | max delta 1 (rounding only) |
| full arc, sharp corners | half-res | max delta 1 |
| partial arc + segment | full-res | deltas > 4 confined to the segment's span |
| partial arc + segment | half-res | same, larger amplitude (64 samples) |

The confined deltas are the intended segment-gate change from §6. Everything
outside them differs by at most one LSB, from `RGBA16F` rounding and from `ti`
being computed directly instead of accumulated.

## 9. Maintenance rules

The whole thing rests on one invariant:

> Anything that is a pure function of `(si, uTime, config)` belongs in
> `neon-emission.frag`. Anything that reads `vPos` belongs in the main shader.

Practical consequences when extending the neon renderers:

- **Adding a per-sample light property** (a new arc or segment field affecting
  colour or coverage) goes in the pre-pass. It costs 128 fragments per frame, not
  8 million. Resist the temptation to add it to the gather loop "just for now".
- **Do not fold anything else into `.rgb`.** The intensity fold works only
  because `uIntensity` is a uniform and both colour terms share the weight `gi`.
  A term with a different weight breaks the algebra in §2 and needs its own
  channel or a second row.
- **Do not gate the filament from the gather.** Both lights now read their
  coverage at `sPos`. Reintroducing a gather-derived gate brings back the head
  quantisation and the corner spill.
- **Keep the two shaders in step.** `neon.frag` and `neon-optimized.frag` share
  the pre-pass, so a change to the emission packing has to land in both
  consumers. `neon-emission.frag` itself is shared verbatim.
- **Adding a shader means three edits** - `lib/CMakeLists.txt`
  (`CMAKE_CONFIGURE_DEPENDS` and `file(READ ...)`) plus `shaders/shaders.h.in`.
  `neon-emission.frag` needs `@NEON_TUNING@` injected because it uses
  `MAX_ARCS`, `MAX_SEGMENT_BOOSTS`, `NEON_MAX_LOOP_SAMPLES` and
  `HEAD_FEATHER_PX`.

## 10. Not done

- `NEON_MAX_LOOP_SAMPLES` is still 128. It sets the minimum crisp halo radius via
  `haloFloor = uSampleSpacing * HALO_SPACING_FLOOR`, so raising it buys tighter
  glow at small radii - and it is much cheaper to raise now than it was. That is
  a look decision, not a perf one.
- The gather still visits all 128 samples for every fragment, including the far
  majority whose weight is negligible. Now that a fragment knows its own `sPos`,
  a windowed gather (samples within a few kernel widths of `sPos * N`, wrapped)
  could cut that to ~24-32 for typical glow radii. The catch is `wsum`: it is a
  full-perimeter density normaliser, so windowing changes the denominator and
  needs care around small rects where a fragment genuinely sees the whole
  perimeter.
