# Upgrade notes: from `main` to the edge-ring branch

What a host has to know when it moves from `main` (`1b5cf94`) to this branch.
Six of the changes are silent - the host still compiles and links, and the
picture moves - so read section 0 first (the default resolution scale is now
0.5), the next two before upgrading anything that uses an opaque fill or a
cutoff softness, and section 5 before upgrading anything that lights only part
of the ring.

## 0. `NeonConfig::resolutionScale` defaults to 0.5, and 1.0 takes the same path

The neon has ONE path now (I57). The direct path - `resolutionScale` 1.0
drawing the glow straight onto the target with no buffer, blit or edge ring -
is gone, and the default dropped from 1.0 to 0.5.

- A host that never set the scale now renders at 0.5: the glow shaded into a
  half-resolution buffer and blitted back, a ring round the edge at full
  resolution. Within 2/255 of the 1.0 render on every scene in
  [`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html),
  and cheaper on frames whose config animates (an intensity pulse, an arc wipe,
  a travelling segment). To keep full resolution, set 1.0 explicitly.
- A host that sets 1.0 gets the same passes with a full-size buffer: within 1
  level of what the direct path drew (a rounding step on up to 17% of
  channels, from the extra 8-bit buffer), and holding the reduced buffer at
  full size over the glow's region (up to 8.3 MB for a full-screen rect at
  1920 x 1080).
- The C ABI follows: `el_effect_get_neon_resolution_scale` on a fresh effect
  reads 0.5.

The sentence below about byte-identity at scale 1.0 describes the branch
before this change.

At `resolutionScale` 1.0, as long as no opaque fill was bounded by the glow's
cutoffs, every cutoff softness is 0 and the ring is lit all the way round (one
arc over the whole of it, no segments), the output is byte-identical to `main`.
Seven scenes were compared pixel for pixel on Mesa llvmpipe: the default, both
one-sided glows (one with a 6 px side softness), a square rect, a hairline, an
inside cutoff at softness 0, and an outside opaque fill with no cutoffs.

## 1. The opaque fill has its own cutoffs, and they default to OFF

**Before:** an `OpaqueMode::INSIDE`, `OUTSIDE` or `BOTH` fill was bounded by the
GLOW's cutoffs, `NeonConfig::insideCutoff` / `outsideCutoff`, with one feather
of its own, `NeonConfig::opaqueSoftness`, centred on each boundary.

**Now:** the fill has its own pair, `NeonConfig::opaqueInsideCutoff` /
`opaqueOutsideCutoff`, each with its own softness, and the glow's cutoffs no
longer reach it. Both default to disabled, so a host that relied on the glow's
cutoffs to bound its fill sees it grow:

| mode | with both fill cutoffs disabled |
| ---- | ------------------------------- |
| `INSIDE` | fills the whole rect |
| `OUTSIDE` | runs to the viewport edge |
| `BOTH` | fills the whole viewport |

Measured at scale 1.0 with `BOTH` and glow cutoffs `{15, 4}` / `{25, 4}`: up to
80/255 off `main` over a black backdrop. Over real content the whole screen
turns `opaqueColor`.

To keep the old shape, copy the glow's bounds into the fill's, moving each
size in by half the old feather (see section 2 for why):

```cpp
auto fillCutoff = [&](const Cutoff &glow) {
    return Cutoff{glow.enable, std::max(glow.size - 0.5f * oldOpaqueSoftness, 0.0f), oldOpaqueSoftness};
};
cfg.neon.opaqueInsideCutoff = fillCutoff(cfg.neon.insideCutoff);
cfg.neon.opaqueOutsideCutoff = fillCutoff(cfg.neon.outsideCutoff);
```

```c
el_effect_set_opaque_cutoff(fx, EL_CUTOFF_SIDE_INSIDE, inEnable, fmaxf(inSize - 0.5f * soft, 0.0f), soft);
el_effect_set_opaque_cutoff(fx, EL_CUTOFF_SIDE_OUTSIDE, outEnable, fmaxf(outSize - 0.5f * soft, 0.0f), soft);
```

`NeonConfig::opaqueSoftness` is gone from the C++ config, so C++ hosts get a
compile error at the old field. C hosts keep
`el_effect_set_opaque_softness` / `el_effect_get_opaque_softness`, now
deprecated forwarders in `el-deprecated.h`. They set the feather on both fill
cutoffs and read back the wider of the two, but they do NOT restore the
bounds. The fill's bounds come only from `el_effect_set_opaque_cutoff`.

## 2. `Cutoff::softness` now runs outward from `size`

**Before:** the feather was centred on `size`: full strength at
`size - softness/2`, gone at `size + softness/2`.

**Now:** the feather starts at `size`: full strength there, gone at
`size + softness`. This holds for the glow's cutoffs and the fill's alike, and
at every resolution scale.

Softness 0 is unchanged (its 50% point is still `size`). Any other softness
moves the edge out by `softness/2`. Measured at scale 1.0, `outsideCutoff`
`{20, 8}` differs from `main` by up to 63/255. For the old look, subtract half
the softness from the size:

```cpp
c.size = std::max(c.size - 0.5f * c.softness, 0.0f);
```

## 3. Below scale 1.0 the neon looks different, costs differently, and holds more memory

- **Quality.** A thin ring around the edge is re-shaded at full resolution, so
  the line and every edge near it now look as they do at 1.0. On the comparison
  page's scenes the result is within 2/255 of 1.0 down to scale 0.125, and the
  cut and cutoffs no longer blur. See
  [`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html).
  Small rects are the exception: a 20 x 17 rect reads 18 / 53 / 93 levels off
  at 0.5 / 0.25 / 0.125, no better than before the ring. Keep small rects at
  1.0.
- **Cost.** The gather - the sample loop, most of the cost - now runs once,
  in its own pass, at a resolution set by how smooth its result is (typically
  about an eighth of the viewport's), and the glow is shaded from it. On Mesa
  llvmpipe the default scene renders about 13.5x faster at 0.5 than at 1.0 and
  about 19x faster at 0.25; `main`'s reduced path, timed on the same machine,
  managed 3.6x and 11.5x.
  The ring, the composite and the extra pass are a fixed cost, so a layer
  already cheap at 1.0 (a tight cutoff band) can run slower below it, and a
  rect small enough to gather at the scale itself (under about 450 px of
  perimeter at 0.5) gains nothing from the split. See
  [`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md) section 13
  and [`neon-resolution-scale-perf-comparison.md`](neon-resolution-scale-perf-comparison.md).
- **Memory.** One RGBA8 buffer at the reduced scale, as on `main`, but covering
  only the part of the frame the glow reaches (never more than the viewport at
  that scale), plus a small RGBA16F gather buffer over the same area at the
  gather's scale (two attachments with segments). At 1920 x 1080 and 0.5: 2.15
  MB for a full-screen rect (`main`: 2.1 MB), 1.8 MB for a 900 x 540 one. At
  0.25: 0.6 MB and 0.54 MB. A driver that cannot render to RGBA16F (GLES 3.0
  without `EXT_color_buffer_half_float`) gets RGBA8 instead, logged once, and
  reads up to 3/255 off 1.0 rather than 2.

## 4. Neon shaders are compiled on first draw

`Initialize` now compiles only the emission pre-pass and the opaque fill. The
neon's own programs are built on the first frame: the gather, the shading
twice - once for the reduced buffer and once for the edge ring, so no program
draws two targets in a frame - and the blit; the glow coverage table's bake on
the first frame that needs it (section 5), and the fields' programs on the
first frame of a config they can serve.

- A host creates its effect faster. On Mesa llvmpipe, init plus the first
  frame at 0.25 dropped from ~50 ms to ~40 ms.
- A neon shader that fails to compile no longer fails `Initialize`, so the
  renderer is no longer dropped from the effect. The error is logged once, on
  the first frame. The frame then draws the opaque fill and no glow, and the
  compile is not retried.

## 5. The glow on a partly lit ring looks different

When arcs light only part of the ring, or segments shine on it, each piece of
the outline's halo and bloom is now scaled by that piece's own coverage, read
from a table baked once per config change, instead of by one coverage
averaged around the pixel (V19 and V20 in
[`review-findings.md`](review-findings.md)). What a host will see:

- **No faint outline along an unlit stretch.** The averaged coverage let a dark
  stretch keep some of the lit part's light, a thin line at a narrow
  `glowRadius` and a bright one at a high `intensity`. It is gone.
- **Darker past each arc end**, by up to 60-80 levels right beside it on a
  narrow halo: the halo now ends with the arc, as its own falloff says.
- **Brighter where a lit edge's bloom reaches a dark region**, by up to 17-27
  levels on a small rect with a strong bloom: that light is no longer dimmed
  by the dark piece's coverage.
- **A fully lit ring is unchanged**, bit for bit.

It costs one more offscreen pass and a 1.0 MB RGBA16F table (0.5 MB in RGBA8
on a driver that cannot render to half float) - since I59 sized to the rect and
the halo width, 0.25-0.69 MB for a default glow, 1.0 MB only for a thin glow on
a large rect. V20's version of it - one
table per perimeter, 2 MB - cost ~1.1x on a partly lit ring and ~1.04x on a
fully lit one at 1.0 (~1.2x and ~1.08x at 0.5) on an M2 Pro; V21's, one table
per piece, measures 1.00-1.01x of that at 1.0 and 4-7% faster below it, on an
AMD Radeon Pro 5300M. The pass runs only when the config changes, and not at
all on a ring lit uniformly (one full arc, no segments), which never reads it;
its program is compiled on the first frame that needs it rather than in
`Initialize`, which on the AMD drops from ~8.7 ms to ~4.5 ms. Under an
animation of the arcs or segments it runs every frame: on the AMD, 0.14 ms a
frame in all with one arc animating and 0.27 ms with three arcs and two
segments (0.17 and 0.55 ms before V21), everything a config change costs
included - still worth measuring on a slower GPU before shipping an animated
segment. See V20 and V21 for the measurements.

## 6. Nothing else a host can see moved

The pass schedule now runs every offscreen pass before it touches the caller's
framebuffer. This changes no pixel, but saves a store and reload of the target
on tile-based GPUs when an opaque fill is on. The blit and the edge ring now
cover only where the glow can be lit. This changes at most 1/255 on a few dozen
pixels from rounding, and saves up to 97% of the composite's fragments on a
cutoff band. See [`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md)
section 12.
