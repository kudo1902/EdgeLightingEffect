# Half-res texel snap

Why `NeonOptimizedRenderer` draws its Pass-1 rect snapped to the FBO texel
grid, and what it fixes. Companion to [`architecture-design.md`](architecture-design.md)
§4.2.

## 1. Symptom

The optimized (half-res) neon rendered the *same* filament settings at two
visibly different thicknesses depending only on the rect geometry:

| Geometry (viewport 1920x1080)       | Look                     |
| ----------------------------------- | ------------------------ |
| `1902 x 1062` at `(9, 9)`, radius 0 | thin, crisp filament     |
| `1900 x 1060` at `(10, 10)`, radius 0 | wider, blurred filament |

Nothing but `Config::geometry` changed between the two. The full-res
`NeonRenderer` showed no such difference.

## 2. Root cause

It is a sampling-phase problem in the half-res FBO, not a geometry-math bug.

The filament profile in `neon-optimized.frag` is

```glsl
float sigma = max(uLineWidth * 0.5, 0.5);   // uLineWidth = lineWidth * resolutionScale
float core  = exp2(-pow(ad / sigma, N));    // ad = |rounded-box SDF|
```

`uLineWidth` arrives pre-scaled into FBO space, so at the default
`lineWidth = 4` and `resolutionScale = 0.5` the whole filament is about
**one texel wide**. A feature that narrow is at the resolution limit of the
buffer it is rasterised into, so what the blit reconstructs depends on where
the rect edge (`d == 0`) falls relative to the texel centres:

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

Full-res pixel coordinates alternate between those two phases: at
`resolutionScale = 0.5` an **odd** full-res coordinate maps to a texel centre,
an **even** one to a texel boundary. That is exactly the two cases above -
`1902 @ 9` puts the edges at x = 9 and 1911 (odd, crisp); `1900 @ 10` puts them
at x = 10 and 1910 (even, blurred). Moving the rect by 1px, or resizing it by
2px, flipped the look.

Measured on the real renderer (left-edge scanline through the middle of the
viewport, `lineWidth = 2`, `glowRadius = 0`, `intensity = 0.1`, position swept
so the left edge lands on successive full-res coordinates):

```
before the fix - OPTIMIZED
  edge x= 8 (even)     1  32  91 121 121  91  32   1     shoulder/peak = 0.75
  edge x= 9 (odd)      8  25  67 134 134  67  25   8     shoulder/peak = 0.50
  edge x=10 (even)     1  35 100 133 133 100  35   1     shoulder/peak = 0.75
  edge x=11 (odd)      9  28  72 141 141  72  28   9     shoulder/peak = 0.51

before the fix - BASE (full-res, for reference)
  edge x= 8           10  85 166 166  85  10             shoulder/peak = 0.51
  edge x= 9            0  10  80 160 160  80  10         shoulder/peak = 0.50
  edge x=10            0   0  10  85 166 166  85  10     shoulder/peak = 0.51
```

The base renderer is phase-stable because at full res there is no resampling
step: every integer geometry lands on the same phase.

Note the halo and bloom are **not** affected - they are wide, low-frequency
layers, many texels across at any usable `glowRadius`. Only the filament is
narrow enough for the phase to read.

## 3. Fix

`lib/src/renderer/neon-optimized-renderer.cpp` pins the crisp phase for every
geometry with two helpers in the anonymous namespace:

- `GetSizeScaled(geom, scale)` - the rect size in FBO texels, **rounded to
  whole texels**.
- `GetCenterScaled(geom, sizeScaled, viewportHeight, scale)` - the rect centre
  in FBO texels, placed by snapping the rect's **top-left corner** to a texel
  centre and growing by `sizeScaled` from there. Because the size is integral,
  the far edges land on texel centres too.

Anchoring the corner rather than the centre matters: rounding the size leaves
up to half a texel of error, and a centre-based placement splits that error
across *both* edges. With the centre snapped, the left edge hopped a texel back
and forth on every other width - dragging the width slider visibly wobbled the
left edge:

```
centre-anchored (rejected), pos.x = 10, scale 0.5

  width   sizeScaled   left edge (FBO)   snapped   full-res
  1900        950            5.00           5.5      11.0
  1901        951            4.75           4.5       9.0    <- hops back
  1902        951            5.00           5.5      11.0
  1903        952            4.75           4.5       9.0    <- and again
```

Growing from the corner puts all the rounding on the far edges, where the rect
is growing anyway. `Config::geometry::position` is the top-left corner, so this
also matches what the caller thinks it pinned.

Both are applied in `Render()` (the MVP centre + `uRectSize` uniform) and the
same `GetSizeScaled` feeds:

- `setupGeometry()` - the Pass-1 quad half-extents, so the quad's
  `uQuadMargin` soft fade still ends exactly `margin` past the rect edge;
- `rebuildLoopSamples()` - the perimeter walk and `mSampleSpacing`, so the
  halo/bloom sample ring stays centred on the filament.

After the fix the profile shape is identical at every geometry (only the
brightness ramp from the perimeter colour gradient differs):

```
after the fix - OPTIMIZED
  1902x1062 @9,9       7  22  63 129 129  63  22   7     shoulder/peak = 0.49
  1900x1060 @10,10     8  25  67 134 134  67  25   8     shoulder/peak = 0.50
  1901x1061 @9,9       9  26  69 138 138  69  26   9     shoulder/peak = 0.50
  1900x1060 @11,11     9  28  72 141 141  72  28   9     shoulder/peak = 0.51
```

and the near edges hold still while the far ones grow (width sweep at
`pos = (10, 10)`, edge positions as brightness centroids in pixel indices):

```
  1896x1060   left 10.50   right 1906.49   top 8.50   bottom 1068.50
  1897x1060   left 10.50   right 1908.50   top 8.50   bottom 1068.50
  1898x1060   left 10.50   right 1908.50   top 8.51   bottom 1068.50
  1899x1060   left 10.50   right 1910.50   top 8.50   bottom 1068.50
  1900x1060   left 10.50   right 1910.50   top 8.50   bottom 1068.50
  1901x1060   left 10.50   right 1912.50   top 8.50   bottom 1068.50
  1902x1060   left 10.50   right 1912.50   top 8.50   bottom 1068.50
  1903x1060   left 10.50   right 1914.50   top 8.50   bottom 1068.50
```

The height sweep is the mirror image: the top edge holds at 8.50 while the
bottom advances. Verified the same at `resolutionScale = 0.25` and with
`cornerRadius = 40`.

## 4. Trade-offs

1. **The rect moves by up to half a texel** (one full-res pixel at
   `resolutionScale = 0.5`). That is below the half-res buffer's own
   granularity. The two axes can round opposite ways - a left edge nudged
   1px right and a top edge nudged 1px up - since each snaps to its own
   nearest texel centre.
2. **The far edges quantise.** An edge can only sit on a texel centre, so the
   right/bottom edge advances in whole texels (2 full-res px at
   `resolutionScale = 0.5`) as the width/height changes: half the 1px steps on
   the debug-UI slider move it, half do not. It is never more than one full-res
   pixel from the requested edge. Before the snap the far edge tracked more
   finely but changed thickness as it went, which is the worse artefact.
3. **The opaque-mode fill is not snapped.** The black-rect pass runs at full
   res off exact geometry, so its boundary and the filament centre can differ
   by that one pixel. Snapping the full-res fill instead would make the
   silhouette itself wrong, and the offset is invisible under the glow band.
4. **Skipped at `resolutionScale >= 1`.** The blit is 1:1 there, so there is
   no resampling to protect against, and snapping would only push this
   renderer half a pixel off the full-res `NeonRenderer`. Both helpers pass
   the value through in that case.
5. **Corner arcs stay unsnapped.** Only the straight edges are grid-aligned;
   a corner curve has no constant cross-section, so the phase does not read
   as a width change there.
6. **Continuously animated geometry would step.** If `geometry.position` /
   `width` / `height` are ever driven per-frame, the neon moves in whole-texel
   increments instead of smoothly changing thickness. Nothing in this repo
   animates geometry per frame today (it comes from the window size or the
   debug-UI sliders).

## 5. Reproducing

There is no test target. The measurements above came from a throwaway host
that links `libedge-lighting.a`, renders into a hidden 1920x1080 GLFW window
and dumps a scanline:

1. Register a `NeonOptimizedRenderer` (and a `NeonRenderer` for the
   reference column) on an `EdgeLightingEffect`.
2. Per case: `SetConfig` with the geometry under test, render a few frames,
   `glReadPixels` the full viewport.
3. Sweep each dimension on its own - position, then width, then height - so
   placement bugs separate from phase bugs. A phase problem changes the
   *shape* of the cross-section; a placement problem moves an edge that
   should have stayed put.
4. Read the row at `viewportHeight / 2` and the column at `viewportWidth / 2`
   as `max(r, g, b)`. The cross-section around each edge is a clean 1D
   profile; a brightness-weighted centroid over it locates the edge to
   sub-pixel precision (a symmetric two-pixel plateau lands on `i + 0.5`,
   i.e. continuous coordinate `i + 1`).

Use a low `intensity` (~0.1) and `glowRadius = 0` when looking at the filament:
at `intensity = 1` the tone-map shoulder (`TONE_MAP_SHOULDER` in
`neon-tuning.h`) saturates the core and hides the difference, which is why the
effect is easy to miss on default settings and obvious on a dim thin line.
