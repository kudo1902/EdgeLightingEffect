# Emission pre-pass: before / after

What the `improve_perf_by_emission_prepass` branch changes relative to the
neon renderers without it, measured on visuals, performance and memory.

Design rationale lives in [`emission-prepass.md`](emission-prepass.md); this
document is the evidence.

## 1. What is being compared

| side | commit | contains |
| ---- | ------ | -------- |
| before | `52411e0` "update documents" | neon renderers with the in-loop gather |
| after | `0034532`, `ee3b5c2`, + follow-ups | same, plus the emission pre-pass |

The "after" side is the pre-pass commit plus four follow-ups, each verified
byte-identical to the state before it:

| follow-up | verified |
| --------- | -------- |
| framebuffer restore (`BindId`, not `BindDefault`) | required for `OffscreenCapture` |
| blend-state restore instead of forced `glEnable` | 4 modes, 0 differing bytes |
| pass 0 deferred past the `opaqueOnly` early-out | 4 modes, 0 differing bytes |
| `Render` split into per-pass methods | 8 modes, 0 differing bytes |

None of them changes a pixel, so the visual and performance tables below still
describe the current tree. The timings were taken before the last two landed;
both are per-frame-constant work (one `glIsEnabled`, and function-call
boundaries hit once per frame each), so they do not move the numbers.

Both sides carry the colour-stop alpha and stop-sorting work (`ec9fc87`,
`9d9d090`), so this isolates the pre-pass alone. It is **not** a diff against
`main` - `main` also lacks those two commits, which change visuals and would
confound the comparison.

The one-line summary: the per-sample half of the gather is a pure function of
`(si, uTime, config)`, so it is baked once per frame into an `N x 2` texture
instead of being recomputed in every screen fragment. Same picture, and
per-fragment cost stops scaling with the number of arcs and segments.

## 2. Visual comparison

Frame dumps at 3840x2160 (33.2M bytes/frame), deterministic scene, fixed clock
step, identical config on both sides.

| scene | renderer | differing bytes | % of frame | max delta |
| ----- | -------- | --------------- | ---------- | --------- |
| 1 full arc, no segments | full-res | 27,927 | 0.08% | **1** |
| 1 partial arc + 1 segment | full-res | 14,089 | 0.04% | **1** |
| 8 arcs + 8 segments | full-res | 14,798 | 0.04% | **1** |
| 1 full arc, no segments | half-res | 30,730 | 0.09% | **1** |
| 1 partial arc + 1 segment | half-res | 15,740 | 0.05% | **1** |
| 8 arcs + 8 segments | half-res | 14,725 | 0.04% | **1** |

**Max delta 1 everywhere** - a single LSB on under 0.1% of pixels. Two causes,
both benign:

- `RGBA16F` storage of the emission table instead of full `float` registers.
- `si` is now computed directly as `floor(gl_FragCoord.x) * invN` rather than
  accumulated through a 128-iteration `si += dti` chain. The direct form is the
  *more* accurate of the two.

There are **no intentional behaviour changes**. (The reference implementation on
`improve_neon_by_emission_pre_pass` moved the segment filament gate to the
continuous path as part of its port; on this branch that gate was already
continuous, so nothing needed moving.)

Colour-stop alpha, the continuous arc/segment coverages and the filament gate
are all still read pointwise at the fragment's own perimeter position - baking
them at sample resolution would reintroduce the quantisation their pointwise
evaluation exists to avoid.

## 3. Performance

3840x2160 framebuffer, 1920x1080 rect, mean of 210 frames after a 30-frame
warmup, `glFinish` inside the timed region so this is GPU time rather than
command submission. Both sides built from the same scaffolded `main.cpp` and
measured back-to-back in one session.

### Full-resolution `NeonRenderer`

| scene | before | after | speedup |
| ----- | ------ | ----- | ------- |
| 1 full arc, no segments | 24.84 ms | 20.77 ms | 1.20x |
| 1 partial arc + 1 segment | 40.61 ms | 20.76 ms | 1.96x |
| 8 arcs + 8 segments | 134.67 ms | 22.88 ms | **5.89x** |

### Half-resolution `NeonOptimizedRenderer`

| scene | before | after | speedup |
| ----- | ------ | ----- | ------- |
| 1 full arc, no segments | 9.50 ms | 5.96 ms | 1.59x |
| 1 partial arc + 1 segment | 12.61 ms | 5.76 ms | 2.19x |
| 8 arcs + 8 segments | 34.19 ms | 7.18 ms | **4.76x** |

### The result that matters

Not any single speedup - the **flatness**:

```
before   24.84 -> 40.61 -> 134.67 ms     (5.4x worse as the scene grows)
after    20.77 -> 20.76 ->  22.88 ms     (1.1x)
```

Per-fragment cost went from `O(samples * (arcs + segments))` to `O(samples)`.
Arcs and segments are now close to free, which is a capability change as much
as a speed one: scenes that were previously unaffordable are now ordinary.

### Measurement caveat

Absolute timings on this machine varied by roughly 2x between measurement
sessions (full-res scene 1 measured 9.25 ms in one session and 20.77 ms in
another, same binary), almost certainly thermal. The table above is
back-to-back within one session, so the comparison is sound, but:

- The **complexity-scaling** result is robust - it is a 5x effect against ~2x
  session noise, and it reproduced in every session.
- The **simple-scene** speedups (1.20x, 1.59x) are within noise of each other
  and should be read as "somewhat faster", not as precise figures.

Also note the before-side was measured first. If the machine throttled
progressively, the after-side is penalised and these speedups are *understated*.
An earlier, lighter-load session measured 2.6x / 4.0x / 14.9x for the same
full-res scenes.

## 4. Memory

### GPU

| allocation | size | scales with |
| ---------- | ---- | ----------- |
| emission table, `NeonRenderer` | **2 KB** | nothing - fixed |
| emission table, `NeonOptimizedRenderer` | **2 KB** | nothing - fixed |
| (for scale) half-res FBO at 3840x2160 | ~8.3 MB | viewport |

`128 x 2 texels x RGBA16F (8 bytes/texel) = 2048 bytes`, confirmed at runtime:

```
Framebuffer[NeonRenderer.Emission] sized to 128x2
```

Two properties worth noting:

- **Fixed, not viewport-scaled.** Unlike every other FBO in the renderer, the
  emission table's size is set by `NEON_MAX_LOOP_SAMPLES`, not by resolution.
  It costs the same 2 KB at 720p and at 4K.
- **Allocated once.** `Framebuffer::Resize` no-ops when size and format are
  unchanged, so this is a one-time allocation, not per-frame churn.

Worst case with both neon renderers enabled: **4 KB**, about 0.05% of the
half-res FBO it sits beside. Nothing else was added and nothing was removed.

### Format fallback

The table asks for `GL_RGBA16F` because both rows exceed 1.0 in ordinary use
(row 1 sums stacked `SegmentBoost::boost`; row 0 carries `Arc::intensity`).
GLES 3.0 exposes float colour-renderability only through an extension, so the
renderers fall back to `GL_RGBA8` and log a warning once. The fallback halves
the table to 1 KB and is exact except that highlights above 1.0 clamp.

On the measured desktop GL target the float path was obtained - no fallback
warning was logged.

### CPU

No change. The pre-pass reuses the existing segment/arc UBOs (packed once per
frame by `packLightBlocks`, which both passes read) and the existing scratch
buffer for effective segments. No new host allocations.

### Register pressure

Not directly measurable here, but the main shader lost `bestMask`, `bestIdx`,
`baseColI`, `segFallback`, `ti`, `si`, `dti` and both inner loops. Lower
register use raises occupancy, which is part of why the gain exceeds what the
removed arithmetic alone would predict.

## 5. Code size

Cumulative against `52411e0`, excluding docs - the pre-pass commit plus the
follow-ups listed in §1:

| file | delta |
| ---- | ----- |
| `lib/shaders/neon-emission.frag` | +179 (new) |
| `lib/shaders/neon.frag` | 216 changed, net **shrinks** |
| `lib/shaders/neon-optimized.frag` | 129 changed, net **shrinks** |
| `lib/src/renderer/neon-renderer.cpp` | 387 changed (pre-pass + pass split) |
| `lib/src/renderer/neon-optimized-renderer.cpp` | 453 changed (same) |
| `lib/include/gl/framebuffer.h` | +43 (format/filter params) |
| both renderer headers | +84 (pass declarations + docs) |
| `lib/CMakeLists.txt`, `shaders.h.in` | +7 (shader wiring) |

Totals: **958 insertions, 562 deletions** across 10 library files. The renderer
.cpp figures are inflated by the pass split, which moved existing code into
methods rather than adding logic.

Both consumer shaders got smaller - the arc scan, the segment loop and
`arcInside` all moved into the shared pre-pass. The net addition is one new
shader plus the pass plumbing in each renderer.

## 6. Costs and open items

Honest accounting of what the change is not:

- **One more draw call per renderer per frame**, and a framebuffer retarget with
  it. Negligible at 256 fragments, but it is not free on a tiler. Skipped
  entirely in the fill-only debug mode, which never samples the table.
- **The pre-pass runs every frame** even though its output depends only on
  `(uTime, config)`. With a paused clock and unchanged config the table is
  bit-identical frame to frame; the renderers already have equality-gated
  rebuild machinery a skip could hang on.
- **A third file to keep in step.** `neon.frag` and `neon-optimized.frag` now
  share a packing contract with `neon-emission.frag`, so a change to the packing
  has to land in both consumers. This strengthens the existing argument for
  unifying the two neon shaders.
- **Three orderings to keep in step** per renderer: the header declarations
  (pass-number order), the .cpp definitions, and `Render`'s call order. They
  agree except in `NeonRenderer`, which runs pass 1 before pass 0 so fill-only
  mode can skip the table; that exception is recorded at the declaration site.
- **Colour-stop alpha is still read pointwise** from the three LUTs, leaving an
  `O(arcs + segments)` term per fragment in the worst case (only when arcs or
  segments carry their own stops). The gather is complexity-independent; the
  whole shader is not quite. A third emission row would close it.
- **The gather still visits every sample** for every fragment. A windowed gather
  is the largest remaining win, and the obstacle is that `wsum` is a
  full-perimeter density normaliser - windowing changes the denominator.

## 7. Reproducing

Both sides need a temporary harness in `demo/src/main.cpp`: a fixed-scene
config selected by env var, `glFinish` around `gEffect->Render`, and a frame
counter that prints the mean and exits. For the baseline, build a worktree at
`52411e0` and copy the same scaffolded `main.cpp` into it, so the two binaries
differ only by the library.

Verify the baseline really lacks the pre-pass before trusting it:

```bash
grep -c NEON_EMISSION_FRAG_SRC build/lib/generated/shaders.h   # must be 0
```

This matters: shaders are embedded at CMake **configure** time, so a stale
configure silently leaves the pre-pass compiled in and produces a
before/after comparison of one binary against itself.
