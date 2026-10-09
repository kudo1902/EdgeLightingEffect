# The glow coverage table at the resolution each light needs: implementation plan

Status: **proposed 2026-10-09, nothing built.** Measured on an **AMD Radeon Pro
5300M** (macOS GL 4.1), Release build, the neon layer alone, 1920 x 1080, scale
0.5 unless a row says otherwise, on top of `f54372a`. Every number below comes
from a prototype built in a scratch copy of the tree; none of them is checked
in (section 10 says what each was). Read ratios, not milliseconds: the target
GPU is neither of the two this project measures on.

This plan carries out the open half of item 10 in
[`neon-perf-plan.md`](neon-perf-plan.md) ("columns sized to the perimeter")
and extends it to the light type that needs it most, the segments. It follows
[`neon-animation-perf-analysis.md`](neon-animation-perf-analysis.md), whose
numbers predate D3, R7, V23 and V24; section 2 re-measures them.

## 1. Summary

- **The problem.** A frame where arcs or segments change costs 1.7-5x a still
  frame on the production band, and 2.5-5.6x on a 960 x 540 rect (section 2.1).
  On the band, the largest single pass in those frames is the glow coverage
  bake (pass 0b): 0.55 ms for ONE segment, 2.47 ms for eight, of a 4.0 ms frame.
- **What does not fix it.** Section 2.2 rules out the redesigns of the shading
  that would let a light change skip pass 1b or the ring. Pass 1b's cost is
  split between the analytic halo / bloom and the table reads, and each hides
  the other's latency, so removing either half saves 9-25%. Re-shading only
  the band of the outline that moved still costs 65% of a full pass.
- **What does.** The table is 1024 columns for every rect and every light
  type, which is far more than most configs need (section 2.3). At 640
  columns the band and the 960 x 540 rect stay within 2 levels of today. A
  segment's bell is wide, so its channels hold within 1 level at 128 columns.
  Only a thin glow on a large rect needs all 1024, and the plan keeps 1024
  there.
- **The plan.**
  1. The table's layout becomes a parameter (byte-identical).
  2. The arcs' table sizes its width from the rect and the halo width.
  3. The segments are baked into a small table of their own, at a width set
     by the narrowest bell, then copied into the main table's segment
     channels by a cheap fill pass. So `neon.frag` still reads one texel per
     piece and does not change.
  4. Optional, the owner's call: an 8-node rule for the bells.
- **Expected** (section 6), production band at 0.5:

  | frame | now | after steps 2-3 |
  | --- | --- | --- |
  | 8 segments changing | 4.0 ms | ~2.2 |
  | 1 segment | 1.78 | ~1.5 |
  | 8 arcs | 2.04 | ~1.85 |
  | 4 arcs + 4 segments | 3.0 | ~2.1 |

  The 960 x 540 rect gains more on segments (5.6 -> ~3.3 ms). Memory falls
  from 1.0 MB to ~0.5-0.8 MB. Images stay within the criterion of section 5
  (max 2 levels, 99.9% of lit pixels within 1) against the build before each
  step.

## 2. Why the table, measured

### 2.1 What an animated frame costs

GPU ms per frame (timer query round `Render`, median of 120 frames) and per
pass (a query round each draw, classified by program as `PassRecorder` does).
"Hue" is a still config with the hue rotating, the default frame.

Production band (1840 x 1000 rect, `glowSide` OUTSIDE, outside cutoff 20 + 4):

| frame | bake (P0b) | gather | pass 1b | blit | ring | **frame** |
| ----- | ---------- | ------ | ------- | ---- | ---- | --------- |
| hue, 1 arc | - | 0.14 | 0.30 | 0.08 | 0.07 (field) | **0.80** |
| 1 arc changing length | 0.17 | 0.16 | 0.33 | 0.08 | 0.37 | **1.37** |
| 8 arcs | 0.69 | 0.16 | 0.37 | 0.08 | 0.47 | **2.04** |
| 1 segment | 0.55 | 0.16 | 0.33 | 0.08 | 0.37 | **1.78** |
| 8 segments | 2.47 | 0.16 | 0.34 | 0.08 | 0.40 | **4.0** |
| 4 arcs + 4 segments | 1.61 | 0.16 | 0.35 | 0.08 | 0.43 | **3.0** |

960 x 540 rect, default look:

| frame | bake | gather | pass 1b | blit | ring | **frame** |
| ----- | ---- | ------ | ------- | ---- | ---- | --------- |
| hue, 1 arc | - | 0.18 | 0.15 (field) | 0.42 | 0.04 (field) | **0.99** |
| 1 arc changing length | 0.15 | 0.18 | 1.22 | 0.42 | 0.36 | **2.54** |
| 8 arcs | 0.66 | 0.18 | 1.25 | 0.42 | 0.44 | **3.18** |
| 1 segment | 0.54 | 0.21 | 1.24 | 0.43 | 0.37 | **2.93** |
| 8 segments | 2.44 | 0.23 | 1.32 | 0.64 | 0.46 | **5.58** |

A light change brings back three things a still frame skips: the bake, pass 1b
shaded directly instead of composited from its field, and the ring shaded
directly instead of composited from its field. Of the three, only the bake
grows with the number of lights, and it is the largest pass on every
production frame that moves a segment.

### 2.2 Redesigns of the shading that do not pay

Each row is `neon.frag` rebuilt with one part removed. The images are wrong;
only the time is read. Pass 1b and the ring on a frame with one arc changing
length:

| variant | pass 1b, 960 x 540 | pass 1b, band | ring, 960 x 540 | ring, band |
| ------- | ------------------ | ------------- | --------------- | ---------- |
| HEAD | 1.22 | 0.33 | 0.37 | 0.37 |
| analytic halo / bloom removed, table reads kept | 1.11 | 0.27 | 0.28 | 0.31 |
| table reads removed, analytic halo / bloom kept | 0.92 | 0.26 | 0.27 | 0.29 |
| both removed | 0.33 | 0.13 | 0.12 | 0.13 |
| both removed, no filament | 0.30 | 0.12 | 0.09 | 0.10 |
| one straight and one corner only, no filament | 0.79 | 0.21 | 0.25 | 0.25 |
| far straights skipped whole | 1.18 | 0.31 | 0.36 | 0.36 |

The costs are not additive: each half of the per-piece work hides the other's
latency. That rules out, on measurement:

- **A table holding each piece's whole contribution**, so that no analytic
  term runs per fragment: 9% on pass 1b (1.22 -> 1.11), plus an accuracy risk
  at the piece ends that the ratio table was designed around (V21).
- **Fields per band of the outline**, re-baking only the bands a light change
  reaches and compositing the rest: one band costs 65% of all four (0.79 of
  1.22; 0.21 of 0.33 on the band), before the composite.
- **Cached per-piece kernel weights** (13.7 MB at 0.5): they remove the
  analytic half, and the table reads that remain cost about as much on their
  own (the first row against HEAD).
- **Skipping the far straights** as the far corners are skipped: 3%, and a
  bound loose enough to move 1-5% of pixels by a level.

So pass 1b and the ring get cheaper only with fewer fragments (a lower
`resolutionScale`), not with less work per fragment. The work that grows with
the lights, and the one that can shrink, is the bake.

### 2.3 The table is larger than the image needs

Scratch builds with the table's compile-time constants changed. They are
compared against HEAD over 10 animated scenes, 6 frames each, at three scales,
by max level difference, with the share of lit pixels off by one level in
brackets. Scenes: the 960 x 540 rect with 8 arcs, with 8 segments, with 1 arc,
and with arcs plus travelling segments (`mid_*`); the band with a segment
(`band_cut`); a 1840 x 1000 rect at the default glow (`big_default`) and at
`glowRadius` 2 / `lineWidth` 2 (`big_thin`); a 200 x 120 rect; a 500 px
circle; and a 3600 x 2000 rect at 3840 x 2160 with `glowRadius` 2
(`uhd_thin`).

| build | mid rects | band | big default | circle, small | big thin | 4K thin |
| ----- | --------- | ---- | ----------- | ------------- | -------- | ------- |
| 1024 wide, overhangs 64 / 32 (HEAD) | - | - | - | - | - | - |
| 640 wide, overhangs 64 / 32 | 1 (0.1-0.7%) | 2 (0.5%) | 2 (0.4%) | 1 | 5 | 12 |
| 512 wide, overhangs 32 / 16 | 1 (0.1-0.8%) | 2 (0.6%) | 2 (0.5%) | 1 | 6 | 14 |
| 1024 wide, overhangs 32 / 16 | 1 (0.1-0.7%) | 1 (0.4%) | 1 (0.3%) | 1 | 2 | 5 |
| 256 wide, overhangs 16 / 8 | 3 | 7 | 7 | 1 | 17 | 30 |
| 128 wide, overhangs 8 / 4, segments-only scene | 1 (2.3%) | | | | | |

The overhangs are the columns past each end of a piece. Three things follow:

- **The interior density sets the error on the default glow.** At 640 columns
  the band reads 4.1 px of straight per column, within 2 levels; the
  960 x 540 rect at 512 reads 2.3 px per column, within 1.
- **The overhangs set it on a thin glow.** Halving them at full width moves
  `uhd_thin` by 5 levels and the default glow by 1. They resolve distance past
  a piece's end in halo widths, so they matter where the halo is narrow.
- **A thin glow on a large rect needs the full 1024**, as V21's sweep found
  (768 columns read 6 levels off a 1 px halo on a 4K-sized rect). The width
  has to follow the rect AND the halo width. Cutting it uniformly would break
  the TV-panel case V21 sized it for.
- **Segments need far fewer columns.** A bell's standard deviation is
  0.354 x its length x the perimeter: ~100 px for a length-0.05 segment on the
  band, against the arcs' 14 px feather. Its channels hold within 1 level at
  128 columns.

What the width buys (bake ms on the band, then the frame):

| build | 8 arcs | 1 segment | 8 segments | frame, 8 arcs / 1 seg / 8 segs |
| ----- | ------ | --------- | ---------- | ------------------------------ |
| 1024 (HEAD) | 0.69 | 0.55 | 2.47 | 2.04 / 1.78 / 4.0 |
| 512 | 0.40 | 0.36 | 1.40 | 1.76 / 1.60 / 2.84 |
| 256 | 0.26 | 0.26 | 0.87 | 1.61 / 1.50 / 2.23 |
| 128 | 0.19 | 0.22 | 0.65 | 1.56 / 1.48 / 2.02 |
| 1024, 8-node rule + narrower bell support | 0.69 | 0.35 | 1.33 | 2.05 / 1.58 / 2.65 |
| 512, 8-node rule + narrower bell support | 0.40 | 0.24 | 0.77 | 1.76 / 1.48 / 2.09 |

## 3. Design

### 3.1 The layout becomes a parameter

Today `neon-pieces.glsl`'s six maps use the compile-time constants below:

- the forward maps `glowCoverStraightUV` and `glowCoverCornerUV`, which the
  shading reads through;
- their inverses `glowCoverStraightAt` and `glowCoverCornerAt`, which the bake
  writes through;
- the overhang spacings `glowCoverStraightOver` / `Past` and
  `glowCoverCornerOver` / `Past`.

They read `GLOW_COVER_WIDTH`, `GLOW_COVER_OVERHANG`,
`GLOW_COVER_CORNER_OVERHANG` and `GLOW_COVER_SHARED`. Each map takes them
instead as one `vec4 layout`:

| component | meaning |
| --------- | ------- |
| `.x` | columns, which is also the texture's width: the forward maps' divisor |
| `.y` | a straight's overhang columns, past each end |
| `.z` | a corner's overhang columns, either side of its arc |
| `.w` | shared columns, `.x - 2 - 2 .y - 2 .z`, split between a band's straight and corner by `uGlowCoverSplit` |

`GLOW_COVER_ROWS` (32) and `GLOW_COVER_HEIGHT` (128) stay constants. Rows are
not what the image is sensitive to (V21: 48 rows bought 1 level for 1.5x the
bake), and keeping them fixed keeps the row maps and the figure tool as they
are.

On the CPU, a `GlowCoverLayout` struct (width, overhang, cornerOverhang,
minInterior, `Shared()`, `AsUniform()`) replaces the constants in:

- `GetGlowCoverSplit(config, layout)`;
- `renderGlowCoverPass`'s piece rectangles (`splitColumn`, the table's width);
- `ensureGlowCoverBuffer`'s allocation.

**The invariant that makes this safe:** the layout a pass reads with is the
one the table was baked with. So the layout is stored with the table
(`GlowCoverTable::layout`), set when the buffer is (re)allocated, and every
upload reads it from there. It is never recomputed from the config at read
time. Today `GetGlowCoverSplit(config)` is called separately by the bake and
by `uploadNeonUniforms`, which is right only because both see the same config
in a frame. The split joins the stored layout too.

### 3.2 The arcs' table: width from the rect and the halo width

The needed width covers the overhangs plus the longest band's interior at a
target spacing:

```
L        = max(width - 2r, height - 2r) + PI/2 * r       full-res px: the longest band's straight plus its quarter arc
S(kh)    = clamp(GLOW_COVER_PX_PER_COLUMN_PER_KH * kh, GLOW_COVER_MIN_PX_PER_COLUMN, GLOW_COVER_MAX_PX_PER_COLUMN)
need     = 2 + 2 * OVERHANG + 2 * CORNER_OVERHANG + ceil(L / S(kh))
W_arc    = clamp(roundUp(need, GLOW_COVER_WIDTH_STEP), GLOW_COVER_MIN_WIDTH, GLOW_COVER_MAX_WIDTH)
```

- `kh` is `neon.frag`'s halo width at scale 1, `max(glowRadius,
  EMISSION_MIN_WIDTH)` px.
- `S` is in full-res px, because the edge ring reads the table at full
  resolution. Lengths enter the layout only as ratios, so every scale still
  shares one table.
- Preliminary constants, from section 2.3, to be fixed by step 2's
  calibration:
  - `PX_PER_COLUMN_PER_KH` 0.8, `MIN_PX_PER_COLUMN` 1.0 and
    `MAX_PX_PER_COLUMN` 4.0. At `kh` 5 that is 4.0 px per column, which the
    640-column build held within 2 levels. At `kh` 2 it is 1.6 px, which
    sends every large thin rect to the 1024 cap: today's value there.
  - `WIDTH_STEP` 64, `MIN_WIDTH` 256 (the overhangs plus two
    `GLOW_COVER_MIN_INTERIOR` need 226), `MAX_WIDTH` 1024 (today's width;
    still the cap).
- The overhangs stay 64 / 32 in this plan. Halving them is measured to cost a
  level on the default glow and 5 on a thin 4K one (section 2.3), so it is
  left as an owner's decision (section 8).

Examples:

| config | `L` | S | W_arc | memory (RGBA16F) |
| ------ | --- | - | ----- | ---------------- |
| 960 x 540, r 40, `kh` 5 | 943 px | 4.0 | 448 | 0.46 MB |
| band 1840 x 1000, r 40, `kh` 5 | 1823 px | 4.0 | 704 | 0.72 MB |
| 200 x 120, `kh` 5 | ~180 px | 4.0 | 256 | 0.26 MB |
| 1840 x 1000, `kh` 2 | 1823 px | 1.6 | 1024 | 1.0 MB (today) |
| 3600 x 2000, `kh` 2 | 3583 px | 1.6 | 1024 | 1.0 MB (today) |

Every input of `W_arc` is already in `glowCoverShapeDirty` (width, height,
cornerRadius, glowRadius), so a width change always coincides with a full
re-bake. It costs the reallocation and nothing more.

### 3.3 The segments' table and the fill pass

Segments get a table of their own, the **segment pre-table**: RG16F (RG8
fallback, the same tier as the main table's), its own layout `layout_s`.

- **Bake.** Pass 0b's program with a new `uBakeTarget` uniform:
  - 0 writes the main table: arcs in `.rg`, segments in `.ba` (direct mode,
    below);
  - 1 writes the pre-table: segments in `.rg`.

  The integrals are today's.
- **Fill**, a new pass after the bakes, `neon-glow-cover-fill.frag`. For each
  main-table texel of a dirty segment piece:
  1. Decode the texel to its piece and piece coordinates through the MAIN
     layout's inverse map: a straight's projection and distance, or a
     corner's offset `w`.
  2. Map those through the PRE-TABLE layout's forward map.
  3. Fetch that texel bilinearly and write it to `.ba`, under
     `glColorMask(false, false, true, true)`.

  The pre-table's corner guards and seam are handled by its forward map,
  exactly as `neon.frag`'s read handles the main table's.
- **Width.**

  ```
  sigma_min = 0.3536 * min(length of every segment with length > 1e-6) * perimeter    full-res px
  S_s       = sigma_min / GLOW_COVER_SEG_COLUMNS_PER_SIGMA
  need_s    = 2 + 2 * SEG_OVERHANG + 2 * SEG_CORNER_OVERHANG + ceil(L / S_s)
  W_seg     = clamp(roundUp(need_s, GLOW_COVER_WIDTH_STEP), GLOW_COVER_SEG_MIN_WIDTH, W_arc)
  ```

  Preliminary values, to be fixed by step 3's calibration:
  - `SEG_COLUMNS_PER_SIGMA` 5;
  - `SEG_OVERHANG` 16 and `SEG_CORNER_OVERHANG` 8;
  - `SEG_MIN_WIDTH` 128.

  A segment of length 0.05 on the band (sigma ~100 px) gets ~192 columns.
  Length is taken from the packed block the bake reads (`seg.y` = 2 / length),
  so the two cannot disagree. A segment of length ~0 is uniform over the ring
  (the bake's `seg.y <= 1e-6` branch) and does not constrain the width.
- **Direct mode.** When `W_seg` would be at least
  `GLOW_COVER_SEG_DIRECT_SHARE` (0.75) of `W_arc` (very short segments), the
  pre-table saves nothing. The segments are then baked straight into the main
  table's `.ba` as today, with no pre-table and no fill.
- **Why a fill pass rather than a second table read in `neon.frag`.** A second
  table would add eight fetches and eight forward maps to every pass that
  shades, on every frame of a segment config. Section 2.2's second row shows
  what table reads cost there. This program is also the one whose cost moves
  with code it never runs (`neon-pieces.glsl`'s note on the forward maps). The
  fill keeps `neon.frag` unchanged and pays only on frames that move a
  segment.
- **Why not bake the segments into the main table at a stride and
  interpolate in place.** It needs a ping-pong of the main table (a pass
  cannot read the texture it writes), and the column layout kinks at the
  overhang and corner boundaries. The fill handles both through the maps.

### 3.4 Allocation, hysteresis and the dirty masks

- **Width = allocation.** Each table's layout width is its texture's width, so
  the forward maps divide by the texture they read. A table is reallocated
  when the needed width rises above the allocated one, or falls to
  `allocated - 2 * WIDTH_STEP` or below. In between, the wider allocation is
  kept and used as the layout: more interior columns than needed, which is
  correct, costs a little more bake, and stops a resizing rect or a segment
  whose length oscillates from reallocating every frame.
- **Where the width is decided.** In `OnConfigChanged`, from the inputs it
  already gates on: `glowCoverShapeDirty` for `W_arc`, and `segmentsDirty`
  plus the shape for `W_seg`. The wanted widths are stored there and applied
  in `ensureGlowCoverBuffer` on the next frame that reads the table, the only
  place GL calls are allowed.
- **Masks.** `dirtyArcPieces` and `dirtySegmentPieces` keep their meaning, one
  bit per piece:
  - `dirtySegmentPieces` drives both the pre-table bake and the fill.
  - A reallocation of the main table sets both masks whole, as today.
  - A reallocation of the pre-table, or a switch into or out of direct mode,
    sets `dirtySegmentPieces` whole.
  - A main-table reallocation in pre-table mode also re-runs the fill for
    every piece, because its `.ba` is undefined after the allocation. That
    is one mask, whole.

  `GetGlowCoverPieceSpans` and both dirty-piece mirrors work in perimeter
  units and are unaffected.
- **Release.** The pre-table is released with the main table: when the layer
  is disabled, after `GLOW_COVER_RELEASE_SECONDS` unread, when segments
  disappear (the main table already drops to two channels then), and in
  direct mode.

### 3.5 The pass schedule

Pass 0b becomes up to three groups of draws, all in the offscreen phase, blending
off, each restoring the framebuffer, viewport and colour mask it found
(`renderGlowCoverPass` already does the latter two):

1. **P0b**, arcs into the main table: today's `arcs & ~segments` group, and in
   direct mode today's other two groups.
2. **P0s**, segments into the pre-table: the dirty segment pieces'
   rectangles of `layout_s`.
3. **P0f**, the fill: the dirty segment pieces' rectangles of the main layout.

`IsGlowCoverUnread` still decides whether any of it runs. The emission table,
the gather, the fields' eligibility and their schedules do not change.

### 3.6 What does not change

- `neon.frag`'s read: one fetch per piece, the decode, `uUniformCover` and
  `GLOW_PIECE_MIN`. It gains only the layout uniform in step 1.
- The rows and their maps, and the encoding `c / (1 + c)`.
- The integrals, until step 4.
- The CPU's piece placement, `GetCornerSkip`, `GlowBoundTerms` and the C ABI.

## 4. Steps

Each step is a separate commit. The owner commits; nothing here touches git.

### Step 0: tooling (no library change)

The prototypes ran on scratch probes. Before step 1, put what they did into
`tools/neon-scale-check`, so every later step is measured by the checked-in
harness and on both GPUs:

- **`time --mode lights`**: N arcs and M segments changing length every frame
  (`--arcs N --segments M`, defaults 8 / 0), on the configured scenes, the way
  `arc-wipe` animates one. That covers the frames of the original report,
  which `arc-wipe` and `segment-travel` (one light each) do not.
- **`generate --set cover`**: the ten scenes of section 2.3, plus segments of
  length 0.01 / 0.02 / 0.2 with boosts up to 2, plus `cornerRadius` 0 and a
  large radius. It writes each animated frame's pixels (six frames per scene,
  at 1.0 / 0.5 / 0.25), as the steady images are written today. It needs the
  1920 x 1080 and 3840 x 2160 frames, which the tool's 1280 x 720 default
  does not cover.
- **`diff DIR_A DIR_B`**: per file, max level, p99.9 and the share of lit
  pixels off by 1 / 2 / 3-4 / 5-8 / more. This is the measure section 5 uses.
- Every run logs `GL_RENDERER`. The AMD machine switches to its Intel GPU per
  process, and a switch looks exactly like a regression in an A/B diff.

### Step 1: the layout as a parameter, still 1024 wide

| file | change |
| ---- | ------ |
| `lib/shaders/neon-pieces.glsl` | Every map takes `vec4 layout` (section 3.1); the four constants leave the maps. |
| `lib/shaders/neon.frag` | `uniform vec4 uGlowCoverLayout;`, passed to `glowCoverStraightUV` / `glowCoverCornerUV`. |
| `lib/shaders/neon-glow-cover.frag` | The same uniform; `main()`'s `split` (`2 * OVERHANG + inner`) and the two `*At` calls take it. |
| `lib/include/renderer/neon-tuning.h` | `GLOW_COVER_WIDTH` becomes `GLOW_COVER_MAX_WIDTH`, `GLOW_COVER_SHARED` goes, `GLOW_COVER_MIN_WIDTH` and `GLOW_COVER_WIDTH_STEP` join. |
| `lib/include/renderer/neon-renderer.h` | `GlowCoverLayout`; `GlowCoverTable::layout`. |
| `lib/src/renderer/neon-renderer.cpp` | The layout set at allocation, always `{1024, 64, 32, 16}` in this step. `GetGlowCoverSplit(config, layout)`, `renderGlowCoverPass` and `uploadNeonUniforms` read `mGlowCover.layout`. The `static_assert` on the shared columns becomes a check where the layout is built. |

Gates:

- **Byte-identical**, the plan's strictest: `neon-scale-check check` and
  `partition` (seeds 1 and 7), `generate --set cover` diffed against the build
  before (0 everywhere), and the 68 guide figures diffed (0).
- **Time:** `still`, `hue`, `arc-wipe`, `segment-travel` and `lights` at 1.0
  and 0.5, three interleaved rounds, on both GPUs. `neon.frag` swaps
  constants for a uniform; the memory note on its register cliff says to
  measure even a change that adds nothing. If it costs, pass `.y` / `.z` as
  the compile-time constants and keep only `.x` / `.w` uniform. The fill's
  program reads the pre-table through the same maps with its own overhangs,
  which is why the maps take the full `vec4`.

### Step 2: the arcs' table sized from the rect

| file | change |
| ---- | ------ |
| `neon-tuning.h` | `GLOW_COVER_PX_PER_COLUMN_PER_KH`, `GLOW_COVER_MIN_PX_PER_COLUMN` and `GLOW_COVER_MAX_PX_PER_COLUMN`, with the calibration recorded beside them, as `RING_GUARD_TEXELS`'s is. |
| `neon-renderer.cpp` | `GetGlowCoverWidth(config)` (section 3.2); `OnConfigChanged` stores the wanted width under `glowCoverShapeDirty`; `ensureGlowCoverBuffer` applies it with the hysteresis of section 3.4 and sets both masks whole on a reallocation (it already does). |
| `neon-renderer.h` | `GlowCoverTable::wantedWidth`; the docs of `GlowCoverTable` and `ensureGlowCoverBuffer` (no longer "fixed size"). |

**Calibration** (step 2's first task, on the `cover` set plus a sweep):

1. Sweep the spacing over rects 200 x 120 to 3600 x 2000 and `glowRadius` {1,
   2, 5, 10, 20}, both windings, `cornerRadius` {0, 40, a circle}, partial
   arcs with and without colour stops, at scales 1.0 / 0.5 / 0.25.
2. Force the width through a temporary constant, with two builds.
3. For each `kh`, take the largest spacing that meets section 5's criterion
   against 1024, then fit `S(kh)`.
4. Record the sweep in `neon-tuning.h` beside the constants.

Gates:

- Section 5's criterion against step 1 on the `cover` set.
- `check`: the 1.0 column against the committed images stays within its bound
  of 2. If it moves at all, say so in the commit, since the page's images
  would then need regenerating as `tools/neon-scale-check/README.md` describes.
- `partition`, untouched by this but cheap.
- The guide figures: only `pass-p0b-glow-cover.png` changes shape (a
  narrower table); every other figure within 1 level.
- Time as in step 1, plus a resize animation (width and height changing every
  frame) for the reallocation path.
- Memory per config (section 3.2's table).

### Step 3: the segments' pre-table and the fill

| file | change |
| ---- | ------ |
| `lib/shaders/neon-glow-cover-fill.frag` (new) | Section 3.3's fill. The texel-to-piece decode is moved out of `neon-glow-cover.frag`'s `main()` into `neon-pieces.glsl`, so the bake and the fill cannot decode a texel differently. |
| `lib/shaders/neon-pieces.glsl` | Guards: forward maps under `#ifndef NEON_GLOW_COVER_BAKE` (as now), inverse maps under `#if defined(NEON_GLOW_COVER_BAKE) \|\| defined(NEON_GLOW_COVER_FILL)`. The fill compiles both directions and none of the integrals, which keeps its first-frame compile small (V21's lesson about this compiler). |
| `lib/shaders/neon-glow-cover.frag` | `uniform int uBakeTarget;` and the output packing of section 3.3. |
| `lib/CMakeLists.txt`, `lib/shaders/shaders.h.in` | The new shader, in all three places CLAUDE.md lists, with `#define NEON_GLOW_COVER_FILL` in its `shaders.h.in` entry. |
| `neon-tuning.h` | `GLOW_COVER_SEG_COLUMNS_PER_SIGMA`, `GLOW_COVER_SEG_OVERHANG`, `GLOW_COVER_SEG_CORNER_OVERHANG`, `GLOW_COVER_SEG_MIN_WIDTH` and `GLOW_COVER_SEG_DIRECT_SHARE`. |
| `neon-renderer.h` | `GlowCoverTable` gains `segBuffer`, `segLayout`, `segFormat` (or the shared tier), `segWantedWidth` and `segDirect`; `mGlowCoverFillShader`; `PROGRAM_GLOW_COVER_FILL` (bit 10); `ensureGlowCoverFillProgram`. |
| `neon-renderer.cpp` | `GetGlowCoverSegmentWidth(segmentBlock, config, W_arc)`; `OnConfigChanged` as section 3.4; `ensureGlowCoverBuffer` allocates or releases the pre-table and keeps the modes; `renderGlowCoverPass` draws P0b / P0s / P0f (section 3.5); `Update` releases the pre-table with the main table. |
| `tools/common/pass-recorder.{h,cpp}` | `PassKind::P0S` (the bake writing the pre-table, told apart by `uBakeTarget`'s value or by the target's width) and `P0F` (the fill, by `uSegCover`). `partition` replays only phase 2, so it needs nothing more. |
| `tools/neon-guide-figures/src/figures.cpp` | The P0b panel reads the main table after its last write (P0F when present); a new panel for the pre-table. |

**Calibration:** sweep `SEG_COLUMNS_PER_SIGMA` and the segment overhangs on
segment lengths 0.01-0.2, boosts 0.5-2, overlapping and abutting segments,
segments straddling a corner, and travelling segments. Pick the smallest
values that meet section 5 against step 2.

Gates:

- Section 5 against step 2 on the `cover` set.
- `check`, `partition`, the figures.
- Time in `segment-travel`, `lights --segments 1/4/8` and `lights --arcs 4
  --segments 4`, and `still` / `hue` to show the fill costs nothing there.
- The first frame of a segment config: `Initialize` plus first-frame time,
  since this compiler finishes a program on its first draw.
- The fill's own cost per dirty piece. It is the one number here that is an
  estimate (0.05-0.2 ms for the whole table).

### Step 4 (optional, owner's decision): the bells' quadrature

In `segmentsOnPiece`:

- the 16-node Gauss-Legendre becomes an 8-node rule;
- the bell's support narrows from `5 / (sqrt 2 * invSigma)` to `3 /
  invSigma`.

`GetGlowCoverDirtySegmentPieces` takes the same support, because it is the
bake's CPU mirror, so a segment then dirties fewer pieces. Measured at 1024:

- 8-segment bake 2.47 -> 1.33 ms;
- max 1 level, on up to 9.3% of the band's lit pixels (1.3% for the narrower
  support alone).

On the pre-table it should halve the remaining segment bake again. It changes
pixels a full table does not, so it is a quality trade and not part of the
exact series.

### Step 5: documents and references

- `CLAUDE.md`'s pass-0b paragraph ("1024 x 128 ... allocated once at this
  fixed size" and the memory figures) and the pass schedule.
- `neon-tuning.h`'s table block, `neon-pieces.glsl`'s layout notes,
  `neon-glow-cover.frag`'s header ("Output"), and the `neon-renderer.h`
  Doxygen of `GlowCoverTable` and the new members.
- `docs/neon-onboarding-guide.md`: the table rows at its buffer, pass and
  constant tables (`mGlowCover.buffer`, the P0b pass card, the frame diagram,
  `GLOW_COVER_WIDTH`), plus a line for P0s / P0f.
- `docs/neon-renderer-reference.html` Fig 4's caption,
  `docs/neon-shader-outputs.html`'s P0b box and grid label,
  `docs/emission-prepass.md`'s pass table, and `docs/implementation.md`.
- `docs/neon-perf-plan.md` item 10: second bullet done.
- `docs/neon-animation-perf-analysis.md`: section 3 re-measured; section 8's
  rows replaced by this plan's.
- `docs/review-findings.md`: I59 (layout parameter, adaptive arc width), I60
  (segment pre-table) and, if taken, I61 (quadrature), each with its numbers.
- `docs/progress-log.md`.
- Rerun the guide figures and diff the directory.

## 5. Verification

**Criterion for a step that may move pixels:** against the build before it, on
the same GPU, for every scene, frame and scale of the `cover` set:

- max difference at most 2 levels;
- at least 99.9% of lit pixels within 1 level.

Plus `check` and `partition` passing. Step 1 alone must be byte-identical.

| gate | step 1 | step 2 | step 3 | step 4 |
| ---- | ------ | ------ | ------ | ------ |
| `cover` set diff | 0 | criterion | criterion | criterion, reported separately |
| `check` | pass, 0 drift | pass | pass | pass |
| `partition` seeds 1, 7 | pass | pass | pass | pass |
| guide figures | 0 | table panel only | table panels only | within 1 |
| time: still / hue | no worse | no worse | no worse | no worse |
| time: arc-wipe / segment-travel / lights | no worse | faster | faster | faster |
| first frame, Initialize | - | - | measured | - |
| both GPUs (AMD 5300M, M2 Pro) | yes | yes | yes | yes |

## 6. Expected result

AMD Radeon Pro 5300M, scale 0.5, frame ms. Steps 2-3 are estimated from the
fixed-width prototypes of section 2.3: 640 or 704 columns for the arcs, 128-192
for the segments, plus 0.05-0.2 ms for the fill.

Step 2 alone already helps segment frames, since until step 3 the segments
still live in the main table and are baked at its width.

| frame | now | step 2 | steps 2-3 | + step 4 |
| ----- | --- | ------ | --------- | -------- |
| band, 1 arc changing | 1.37 | ~1.33 | ~1.33 | ~1.33 |
| band, 8 arcs | 2.04 | ~1.85 | ~1.85 | ~1.85 |
| band, 1 segment | 1.78 | ~1.65 | ~1.5 | ~1.45 |
| band, 8 segments | 4.0 | ~3.3 | ~2.2 | ~1.9 |
| band, 4 arcs + 4 segments | 3.0 | ~2.55 | ~2.1 | ~1.9 |
| 960 x 540, 8 arcs | 3.18 | ~2.85 | ~2.85 | ~2.85 |
| 960 x 540, 8 segments | 5.58 | ~4.4 | ~3.3 | ~3.0 |
| still and hue frames | unchanged | unchanged | unchanged | unchanged |

Memory, glow coverage: 1.0 MB today with segments (0.5 MB without). After:
~0.82 MB on the band (0.72 MB main + 0.10 MB pre-table), ~0.56 MB on the
960 x 540 rect, 1.1 MB at most (1.0 MB main + the pre-table, on a large thin
rect).

What is left after it: on the band, an animated frame is the ring (0.37-0.47
ms), pass 1b (0.33), the gather (0.16), the blit (0.08) and the bake (now
~0.1-0.8). That is ~1.7-2.8x a hue frame instead of 1.7-5x. On larger glow
areas pass 1b dominates, and only `resolutionScale` shrinks it (0.25: 1.22 ->
0.43 ms on the 960 x 540 rect, within 3 levels on the comparison page).

## 7. Risks

| risk | where it would show | guard |
| ---- | ------------------- | ----- |
| `neon.frag` slows for a uniform where a constant was | every frame that shades | step 1's timing gate; fold the overhangs back to constants if needed |
| the bake and a read using different layouts | a band read from the wrong texels: large, obvious errors | the layout stored with the table and read from there only (section 3.1); step 1's byte identity proves the plumbing |
| the fill's double interpolation near a corner's seam or guards | circles and large radii, behind the centre | the forward map handles the guards; the circle and large-radius scenes in the `cover` set |
| reallocation churn under a resize or a length animation | frame spikes on drivers slow to allocate (the TV) | hysteresis (section 3.4); step 2's resize animation |
| very short segments | the pre-table would need the main table's width | direct mode (section 3.3) |
| the RG8 / RGBA8 fallback with fewer columns | a driver without half-float targets | untested today at any width (item 10's fourth bullet); measure once with the fallback forced |
| calibrated on one GPU | the target differs | calibrate and time on both GPUs; the criterion is in levels, not ms |
| a new pass misfiled by `PassRecorder` | wrong guide figures | step 3 adds the kinds; the figure diff shows it |
| the fill's first-draw compile | a stall on the first segment frame | built lazily with the bake, timed in step 3 |

## 8. Decisions for the owner

1. **The criterion.** Section 5 allows 2 levels at the worst pixel and 1 on
   99.9% of lit pixels. Holding the default glow to 1 level everywhere moves
   `S(5)` from ~4 px to ~2.5 px per column. The arcs' gain on the band then
   disappears (W_arc 960), while the segments' gain stays.
2. **Halving the main table's overhangs for `kh` >= 5.** Measured at 1 level
   on the default glow (section 2.3) and ~100 more interior columns at the same
   width. Not on a thin glow (5 levels at 4K).
3. **Step 4**, the 8-node rule and narrower bell support: a quality trade (1
   level, up to 9% of a band's pixels).
4. **Whether to take, in the same series, the one exact shading change the
   review found.** Pass 1b never needs the filament, since the ring always
   covers its reach. Skipping it is byte-identical on all 30 captures and
   saves 0.03-0.05 ms with eight lights. It is independent of this plan.

## 9. Related, not in this plan

- **The band's rotating-hue frame shades pass 1b directly** (0.30 ms of 0.80).
  The hue-invariant field refuses it because a thin band fills under half of
  its box (`FIELD_MIN_FILL`). A field packed into strips, as the ring's is
  (`RingFieldLayout`), would serve it: est. 0.80 -> ~0.55 ms. That is the
  production default frame, not an animated one.
- **The ring's filament from a perimeter table:** the arc loop costs up to 0.1
  ms in the ring with eight arcs (section 2.2's rows 4-5). A 1-D coverage table
  baked per light change would make it one fetch. Est., unbuilt.
- **`resolutionScale` for animated content**: the only lever on pass 1b
  (section 6).

## 10. Appendix: the prototypes

All in a scratch copy of the tree at `f54372a`, built Release, timed with a
probe that wraps `glad_glDrawArrays` the way `tools/common/pass-recorder.cpp`
does, and compared with a byte-diff tool. Step 0 re-creates both in
`neon-scale-check`.

| name | change |
| ---- | ------ |
| `pw512`, `pw256`, `pw128` | `GLOW_COVER_WIDTH` 512 / 256 / 128 with the overhangs scaled (32/16, 16/8, 8/4) and `MIN_INTERIOR` 16 / 8 / 4 |
| `w640` | width 640, overhangs unchanged |
| `ovhalf` | width 1024, overhangs 32 / 16 |
| `sqA` | bell support `3 / invSigma` in `segmentsOnPiece` (CPU mirror unchanged) |
| `sqB` | `sqA` plus an 8-node Gauss-Legendre rule in `bellMass` |
| `sqB512` | `sqB` on `pw512` |
| `absproxy` | `neon.frag` with every analytic halo / bloom term replaced by its piece's table read |
| `h1` | HEAD with the table read replaced by the gathered coverage |
| `a1`, `a2` | `absproxy` without the reads; then without the filament |
| `band1` | one straight and one corner only, no filament in pass 1b |
| `nofil1b` | pass 1b without the filament (`filamentLit` false where `uBlitOwnsCut`) |
| `fs` | `nofil1b` plus far straights skipped past `max(reach, 64 kh)` |
