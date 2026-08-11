# Full-res filament

Why `NeonOptimizedRenderer` splits the filament out of its half-resolution
pass and draws it at full resolution, and what that costs. Companion to
[`architecture-design.md`](architecture-design.md) §4.2.

## 1. The problem

The optimized neon rendered the *same* filament settings at two visibly
different thicknesses depending only on the rect geometry:

| Geometry (viewport 1920x1080)         | Look                    |
| ------------------------------------- | ----------------------- |
| `1902 x 1062` at `(9, 9)`, radius 0   | thin, crisp filament    |
| `1900 x 1060` at `(10, 10)`, radius 0 | wider, blurred filament |

Nothing but `Config::geometry` changed. The full-res `NeonRenderer` showed no
such difference.

### Root cause: the filament is thinner than the buffer it was drawn in

The filament profile is a generalized Gaussian around the rounded-box SDF:

```glsl
float sigma = max(uLineWidth * 0.5, 0.5);
float core  = exp2(-pow(ad / sigma, N));    // ad = |SDF|
```

In the old design `uLineWidth` arrived pre-scaled into FBO space, so at the
default `lineWidth = 4` and `resolutionScale = 0.5` the whole filament was
about **one texel wide**. A feature that narrow is at the resolution limit of
the buffer, so what the blit reconstructed depended on where the rect edge fell
relative to the texel centres:

```
  edge ON a texel centre           edge ON a texel boundary
  (crisp)                          (blurred)

  texel:   |   |   |   |             texel:   |   |   |   |
  sample:    .   X   .                 sample:  .  X   X  .
  value:   .06  1.0  .06             value:   .21 .84 .84 .21
           one bright texel,        two texels both on the shoulder;
           neighbours already       a plateau that blits out twice
           down the falloff         as wide and softer
```

Full-res pixel coordinates alternate between those phases: at
`resolutionScale = 0.5` an **odd** coordinate is a texel centre, an **even**
one a boundary. `1902 @ 9` puts the edges at x = 9 and 1911 (odd, crisp);
`1900 @ 10` puts them at x = 10 and 1910 (even, blurred). Moving the rect by
1px, or resizing it by 2px, flipped the look.

Measured on the real renderer - left-edge scanline, `lineWidth = 2`,
`glowRadius = 0`, `intensity = 0.1`, position swept so the left edge lands on
successive full-res coordinates:

```
  edge x= 8 (even)     1  32  91 121 121  91  32   1     shoulder/peak = 0.75
  edge x= 9 (odd)      8  25  67 134 134  67  25   8     shoulder/peak = 0.50
  edge x=10 (even)     1  35 100 133 133 100  35   1     shoulder/peak = 0.75
  edge x=11 (odd)      9  28  72 141 141  72  28   9     shoulder/peak = 0.51
```

The halo and bloom were never affected - they are wide, low-frequency layers,
many texels across at any usable `glowRadius`. **Only the filament was narrow
enough for the sampling phase to read.** That asymmetry is what the design
below exploits.

### The alternative that was tried first, and why it lost

Snapping the rect to the FBO texel grid (round the size to whole texels, place
it from a corner snapped to a texel centre) does pin the crisp phase, and it is
a ten-line CPU change. It was implemented and then removed, because the price
is paid in geometry:

- the rect lands up to **one full-res pixel** off where it was asked to be, so
  the optimized renderer no longer matched the base renderer - toggling between
  them jumped the whole ring by a pixel;
- the far edges could only sit on texel centres, so a width slider moved the
  right edge in 2px steps;
- continuously animated geometry would step rather than glide.

A crisp filament requires the edge on a texel centre, and texel centres only
exist at odd full-res coordinates. **Exact placement and guaranteed crispness
are mutually exclusive in a single half-res pass** - no rounding rule gets both.
The way out is to stop asking the half-res pass to carry the filament at all.

## 2. Design

Split the work by frequency, not by convenience:

```
  Pass 1  (half res, the expensive one)      Pass 2  (full res, the cheap one)
  ------------------------------------       ---------------------------------
  N-sample perimeter gather                  upscale pass 1's two fields
  arc/segment resolution + LUT reads         rasterise the FILAMENT (SDF only)
  halo + bloom accumulation                  sum, tone-map ONCE, premultiply
     |                                          ^
     +--> lightCol, haloTerm, segGate  ---------+
```

The gather - the part that costs 64 texture-free but ALU-heavy iterations per
fragment - stays at half resolution. The filament is an analytic SDF
evaluation with no loop, so drawing it at full resolution is nearly free, and
it is the only part that needed the resolution.

### What crosses between the passes

Pass 1 stops emitting a picture and emits three values:

| Value      | Where              | What it is                                    |
| ---------- | ------------------ | --------------------------------------------- |
| `lightCol` | RGBA16F `.rgb`     | gathered emission colour, `col * uIntensity + segCol` |
| `haloTerm` | RGBA16F `.a`       | scalar halo+bloom weight, all masks applied   |
| `segGate`  | R8 `.r`            | sample-based segment gate for the filament    |

Two decisions make that fit:

1. **Halo and bloom share `lightCol`**, so their entire contribution is one
   scalar (`glow * HALO_GAIN * haloGate + bloom * uBloomStrength`). That is
   what lets colour and halo share a single RGBA target instead of needing six
   channels.
2. **The filament gate splits in half.** Its arc half is continuous and purely
   geometric, so pass 2 recomputes it from its own fragment position (better -
   full-res feathering). Only the sample-derived segment half has to travel,
   and 8 bits is plenty for a value that feeds a `smoothstep`.

Both transported fields are smooth and many texels wide, so bilinear upscaling
them is faithful - which is exactly what was *not* true of the filament.

### Consequences that are improvements

- **Geometry is exact.** No snapping; the rect goes where `Config::geometry`
  says. Verified equal to the base renderer on all four edges across position,
  width and height sweeps.
- **The filament no longer degrades with `resolutionScale`.** It is drawn at
  full res whether the scale is 0.5 or 0.25 - only the halo coarsens.
- **The tone map moved after the upscale.** Mapping is non-linear, so running
  it before a bilinear filter was filtering already-compressed values. It now
  runs once, at full res, over filament + halo summed.
- **Pass 1 got cheaper**: no `perimeterPosition`, no continuous arc loop, no
  filament profile, no tone map.

## 3. Results

Agreement with the full-res `NeonRenderer` over the whole middle scanline,
same config both sides (`max|d|` is out of 255):

```
  lineWidth/glowRadius/bloom/intensity     before            after
  2 / 0    / 0   / 0.1  (filament only)    max 88..98        max 5..10
  4 / 0    / 0   / 1.0  (filament only)    max 82..83        max 5..10
  4 / 5    / 0.3 / 1.0                     max 45..51        max 23..24
  8 / 20   / 0.6 / 1.0                     max 30..31        max 30..31
```

The filament error drops from ~35% of range to ~2-4%. The remaining difference
in the halo-heavy rows is pre-existing and unchanged (mean |d| 18.86 -> 18.55,
24.33 -> 24.16): the halo is still gathered at half res with scaled kernel
widths, which is the point of the renderer. Note where the worst pixel moved -
before the change it sat on the rect edge (x = 12, x = 1906), i.e. on the
filament; after, it sits in the middle of the glow field (x = 321, x = 449).

Edge placement, base vs optimized, across a position sweep:

```
  1900x1060 @ 9, 9   delta L +0.0  R +0.0  T +0.0  B +0.0
  1900x1060 @10,10   delta L +0.0  R +0.0  T +0.0  B +0.0   (was +1 +1 -1 -1)
  1900x1060 @11,11   delta L +0.0  R +0.0  T +0.0  B +0.0
  1900x1060 @12,12   delta L +0.0  R +0.0  T +0.0  B +0.0   (was +1 +1 -1 -1)
```

Verified the same at `resolutionScale` 0.25, with `cornerRadius` 40, and with
`opaqueMode` NONE / BOTH / ALL (fill lands identically in both renderers).

## 4. Costs and risks

1. **RGBA16F must be colour-renderable.** Core in desktop GL 3.3 and in GLES
   3.2; on GLES 3.0 it needs `EXT_color_buffer_half_float`, which every ES3
   device in practice ships. `Framebuffer::Resize` logs the incomplete-FBO
   status with the format if a driver refuses. If a target ever does,
   the fallback is `R11F_G11F_B10F` for the colour plus a separate scalar
   target - more MRT plumbing, same shape.
2. **Bandwidth.** Pass 1 now writes 8+1 bytes/texel at half res instead of 4,
   and pass 2 reads both. Still far below the cost of gathering at full res.
3. **Pass 2 is no longer a trivial blit.** It evaluates the SDF per fragment
   and takes a branch on `ad < sigma * 6.0`, past which the profile is dead
   (`exp2(-36)` at N = 2). Only a thin ring of fragments enters the filament
   body - the interior and the whole halo field skip it - so the cost lands
   where the pixels are, not across the screen.
4. **One-sided modes still bleed at the cut.** Pass 1 discards the culled half
   before the gather (a real saving), so `lightCol` is zero there and the
   bilinear tap darkens the filament slightly at `d ~ 0`. That behaviour is
   unchanged from the old design, which had the same discard.
5. **`showHalfRes` shows less than it used to.** It still switches the source
   sampler to nearest, but the filament is drawn at full res on top, so the
   debug view is a blocky halo field with a sharp line over it.

### Not fixed here: the segment gate sits on a knee

Surfaced while testing the R8 path, and **pre-existing** - the original build
behaves identically, so it is not a consequence of this design.

The segment half of the filament gate is `smoothstep(0.5, 1.0, segFraction)`,
where `segFraction` is the proximity-weighted segment coverage over the gather
samples. For a segment-only filament (one `SegmentBoost`, `arcs` cleared,
`boost = 1`, `length = 0.10`) the optimized renderer measures
`segFraction ~ 0.475` and the base lands just above 0.5, so the same config
lights a dim filament in the base and nothing at all in the optimized. The
inputs differ legitimately - 64 gather samples versus 128, hence different
kernel widths - but the hard knee at 0.5 turns a small numerical difference
into a visible on/off.

Arcs do not have this problem: their gate went continuous and geometric
earlier. Giving segments the same treatment (evaluate the bells at the
fragment's own `sPos` instead of averaging over samples) would remove the knee
and drop the R8 target entirely, at the cost of changing segment appearance in
both renderers - a tuning decision, not a mechanical fix.

## 5. Reproducing the measurements

There is no test target. The numbers above came from a throwaway host that
links `libedge-lighting.a` and renders into a hidden 1920x1080 GLFW window:

1. Register a `NeonOptimizedRenderer` and a `NeonRenderer` on one
   `EdgeLightingEffect`; per case, enable exactly one of them.
2. `SetConfig` the geometry under test, render a few frames, `glReadPixels`
   the viewport, and diff the two renderers' rows pixel by pixel.
3. Sweep each dimension on its own - position, then width, then height - so
   placement problems separate from sharpness problems. A sharpness problem
   changes the *shape* of the cross-section; a placement problem moves an edge
   that should have stayed put.
4. Locate an edge with a brightness-weighted centroid over its cross-section:
   a symmetric two-pixel plateau lands on `i + 0.5` in pixel indices, i.e.
   continuous coordinate `i + 1`.

Use a low `intensity` (~0.1) and `glowRadius = 0` when looking at the filament.
At `intensity = 1` the tone-map shoulder (`TONE_MAP_SHOULDER` in
`neon-tuning.h`) saturates the core and hides the difference - which is why the
original bug was easy to miss on default settings and obvious on a dim thin
line. Conversely, keep `glowRadius` at 0 for edge-position measurements: with
the halo on, a centroid over the row locates the glow field, not the edge.
