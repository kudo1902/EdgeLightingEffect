# Neon frame cost: where the time goes, and what is left to improve

Measured 2026-10-08 on the working tree on top of `6042866`, on an **AMD Radeon
Pro 5300M** (macOS GL 4.1), Release build, the neon layer alone, 1920 x 1080.
Started from a report that the frame rate drops when several arcs or segments
change length every frame; widened to every frame type, since the default frame
(a rotating hue) is what most frames are. Read ratios and shares, not
milliseconds: the target device is not this one (see section 10).

## 1. Summary

- **What a frame costs depends on what moved, far more than on anything else.**
  At scale 0.5 with a 960 x 540 rect: a still frame 0.42-0.65 ms, a rotating-hue
  frame 0.9 ms, 8 arcs changing length every frame 3.07 ms, 8 segments 5.36 ms.
  A config change invalidates the three caches that make still and hue frames
  cheap - the reduced buffer's reuse, the glow's field and the ring's field - and
  re-runs the coverage bake.
- **Three passes are the animated frame**: the coverage bake (P0b, up to 2.43 ms
  with 8 segments), the shading (P1b, ~1.2 ms at 0.5) and the edge ring shaded
  directly (P2c, ~0.4 ms). The CPU side - `SetConfig`, `Update`, `Render` - is
  under 0.25 ms on this machine.
- **Two exact fixes are built** (section 7): the filament's pointwise inputs are
  skipped off the line (P1b stopped growing with the light count: 8 arcs 1.90 ->
  1.20 ms), and the coverage bake re-integrates only the light type that changed
  (arcs over still segments: bake 1.35 -> 0.35 ms, frame 1.42x).
- **Nothing that is left is both exact and large.** The biggest remaining
  levers are a scale choice (no code), a second channel for the field so
  segment configs stop re-shading on every hue frame (exact like today's field,
  ~1.1 MB), and approximations in the bake. Section 8 ranks them.
- **The floor**: the blit, 0.38 ms on every frame - 1.64M blended full-resolution
  pixels, 96% of which it really lights. It is not wasted work; it is the bloom's
  footprint on screen.

## 2. Method

Probes in the scratchpad, linked against the static library (none checked in;
`tools/common/pass-recorder.cpp` is the pattern):

- **Frame and pass time.** One `GL_TIME_ELAPSED` query round `Render` per frame
  with a `glFlush` after it (a host's submit), median of 120 frames; and in a
  second run one query round each draw, classified by program the way
  `PassRecorder` does (the ring field's composite filed separately). CPU time of
  `SetConfig`, `Update` and `Render` around the same frames.
- **Fragments.** `GL_SAMPLES_PASSED` round each draw. "Visible" = a texel or
  pixel with any channel >= 1/255: read back after the draw for the offscreen
  colour passes, and with that pass alone on a cleared target for the blit and
  the ring. The per-draw queries and readbacks inflate that run's milliseconds
  about 2x; only its fragment counts and ratios are used.
- **Attribution inside the shading.** `neon.frag` rebuilt with one block
  removed (the corner arcs; the straights; the per-piece coverage path forced to
  the uniform one), P1b and the ring timed against the baseline, two rounds
  alternating, median. The images are wrong; only the time is read.
- **Prototypes** of the options in section 8 built in copies of the tree, timed
  the same way, and their pixels compared byte for byte against the current
  tree over 21 animated captures (7 configs x 3 scales x 6 frames).

Configs: a 960 x 540 rect centred, `colorTransitionDuration` 0, the library's
default `hueRotationRate` (0.5) unless said otherwise; arcs spread evenly round
the ring, each changing length on its own phase; segments likewise (length
0.05-0.15, boost 1).

## 3. Frame cost by frame type

GPU ms per frame, scale 0.5, per pass (attribution run) and the frame total
(frame run). "-" = the pass does not run.

| frame | P0 emission | P0b bake | P1a gather | P1b / P1c | blit | ring | frame |
| ----- | ----------- | -------- | ---------- | --------- | ---- | ---- | ----- |
| still (hue 0) | - | - | - | - | 0.38 | 0.03 (field) | **0.42-0.65** (two runs, same passes) |
| still, 8 segments | - | - | - | - | 0.38 | 0.37 (direct) | **0.74** |
| rotating hue | 0.05 | - | 0.18 | 0.11 (P1c) | 0.38 | 0.04 (field) | **0.90** |
| rotating hue, 8 segments | 0.07 | - | 0.21 | 1.22 (P1b) | 0.40 | 0.37 | **2.44** |
| 1 arc changing length | 0.06 | 0.16 | 0.18 | 1.17 | 0.39 | 0.35 | **2.44** |
| 8 arcs changing length | 0.07 | 0.66 | 0.18 | 1.19 | 0.39 | 0.44 | **3.06** |
| 8 segments changing length | 0.08 | 2.43 | 0.22 | 1.24 | 0.56 | 0.38 | **5.36** |
| 4 arcs moving, 4 segments still | 0.07 | 0.35 | 0.22 | 1.20 | 0.54 | 0.40 | **2.79** |

The same frames at lower scales (only P1b shrinks; the bake, the ring, the blit
and the gather do not):

| frame | 0.5 | 0.35 | 0.25 |
| ----- | --- | ---- | ---- |
| 8 arcs changing length | 3.07 | 2.60 | 2.29 |
| 8 segments changing length | 5.40 | 4.75 | 4.59 |
| P1b alone | 1.20 | - | 0.42 |

## 4. Pass by pass

Fragments drawn, share visible, and cost per fragment (from the inflated run -
compare passes with each other, not with section 3):

| pass | fragments | visible | ns / fragment | scales with | re-runs when |
| ---- | --------- | ------- | ------------- | ----------- | ------------ |
| P0 emission | 256 | - | fixed cost | nothing | an input moved (time under a rotating hue) |
| P0b coverage bake | 113k (8 arcs) - 131k (8 segments) | - | 13 (arcs) - 43 (segments) | lights overlapping each piece | a light or the shape changed |
| P1a gather | 12k | - | 35 | gather texels x samples | the emission table or the shape changed |
| P1b shading | 428k at 0.5, 107k at 0.25 | 96-98% | 6.4-7.7 | `resolutionScale`^2 | any neon config change |
| P1c field composite | 428k at 0.5 | 97% | 0.5 | `resolutionScale`^2 | hue / intensity frames |
| blit | 1,641k | 96% | 0.5 | the glow's screen area | every frame |
| ring, shaded | 80k-92k | 100% | 10-12 | perimeter x ring width | every frame without its field |
| ring, field composite | 80k-92k | 100% | 1.0 | the same | every frame with its field |

- **P0b** is the one pass whose cost per texel grows with the number of lights:
  each arc is a few clipped trapezoid integrals, each segment two 16-node
  Gauss-Legendre integrals (`bellMass`, a `tan`, an `exp` and a `cos` per node)
  for every texel of every piece its bell reaches. Eight segments spread round
  the ring reach every piece, so the whole table is re-baked every frame.
- **P1b** is dense: almost every fragment it draws is visible, so only fewer
  fragments (a lower scale) or a cheaper fragment makes it faster.
- **The ring** costs nearly twice as much per fragment as P1b - full resolution,
  next to the line where every near piece contributes - and its width is set by
  the filament's reach as the reduced buffer drew it, plus one guard texel
  (`GetRingWidth`), so the blit never shows the reduced buffer's blocky line.
- **The blit** is already minimal on the default config: one bilinear fetch and
  a premultiplied-over blend; its cut and cutoff math is skipped on a uniform
  branch.

## 5. Inside the shading

Share of P1b and of the ring removed by dropping one block of `neon.frag`
(median of two rounds; the per-piece coverage path sits inside the other two):

| block | P1b | ring |
| ----- | --- | ---- |
| the four straights' halo + bloom, coverage fix included | 34% | 26% |
| the four corner arcs | 24% | 18% |
| the per-piece coverage path (table fetch + fix), inside the two above | 22-25% | 17-25% |
| the rest: SDF, discards, gather read, filament, tone map, quad fade | ~40% | ~55% |

The analytic halo and bloom are over half of P1b. A piece already skips its
table fetch where its own halo plus bloom is below `GLOW_PIECE_MIN` (1e-4 in
linear light), so the far edges of the rect cost little more than their halo's
two square roots; what remains is the near pieces' real work.

## 6. Work that writes nothing

There is almost none. Share of each pass's fragments by the largest channel
they leave (blit, alone on a cleared target):

| config | 0 | 1 level | 2 | 3-4 | 5-8 | > 8 |
| ------ | - | ------- | - | --- | --- | --- |
| default, rotating hue | 3.8% | 3.5% | 3.6% | 10.0% | 16.8% | 62.4% |
| 8 arcs changing length | 4.8% | 6.7% | 10.3% | 15.3% | 18.5% | 44.4% |
| 8 segments changing length | 3.1% | 2.2% | 2.2% | 5.0% | 13.8% | 73.7% |
| production band | 15.4% | 0.1% | 0.0% | 5.5% | 0.1% | 79.0% |

P1b's texels are 96-98% visible. The production band's ring and blit tile the
lit band exactly: across the left edge the ring takes the 2 px safety strip
inside (`LIT_EDGE_SAFETY`, writing 0) and 9 px outside, the blit from there to
the cutoff's end at 24 px, nothing beyond.

## 7. Built 2026-10-08

Both byte-identical: `neon-scale-check check` and `partition` (seeds 1 and 7),
the 68 guide figures, and 33 animated captures aimed at the changed code,
against the build before either change.

| change | what | measured |
| ------ | ---- | -------- |
| `filamentLit` (`neon.frag`) | `sPos`, the pointwise stop alpha, `emitCover` and `segCoverPt` feed only the filament, which is exactly 0 past its reach; the whole block is skipped there | P1b flat in the light count: 8 arcs 1.90 -> 1.20 ms, frame 3.82 -> 3.07 ms; 8 segments on a rotating hue 2.74 -> 2.44 ms |
| per-type coverage re-bake | `dirtyArcPieces` / `dirtySegmentPieces`; a piece only one type dirtied integrates that type and writes its channels under `glColorMask` | arcs over still segments: bake 1.35 -> 0.35 ms, frame 3.96 -> 2.79 ms (1.42x); segments over still arcs 1.55 -> 1.32 ms |

`neon-scale-check time` in `still`, `arc-wipe` and `segment-travel`, three
interleaved rounds against the build before them: no regression (geomeans
0.98-1.09; `still` does not run the changed code and its spread is noise).

## 7b. Built 2026-10-09

[`neon-glow-cover-resolution-plan.md`](neon-glow-cover-resolution-plan.md),
I59-I62. **Read every millisecond in this document as a ratio:** they are
`GL_TIME_ELAPSED` readings, which on this AMD Radeon Pro 5300M are 3.6-4.0x
the wall-clock time of the same frames (found building that plan's step 0).

| frame, scale 0.5 | timer units, before -> after | wall-clock ms, before -> after |
| ---------------- | ---------------------------- | ------------------------------ |
| band, 8 segments | 4.06 -> 1.97 | 1.05 -> 0.61 (1.72x) |
| band, 4 arcs + 4 segments | 3.03 -> 2.08 | 0.87 -> 0.63 (1.38x) |
| band, 8 arcs | 2.22 -> 1.80 | 0.63 -> 0.60 |
| 960 x 540, 8 segments | 5.65 -> 3.37 | 1.44 -> 0.92 (1.57x) |
| 960 x 540, 8 arcs | 3.20 -> 2.85 | 0.94 -> 0.85 |

Item 3 below is half built: the narrower support (I61) is in, the 8-node rule
was measured against the plan's criterion and failed it (3 levels off a long,
bright segment).

## 8. Improvable points, ranked

| # | change | frames it helps | gain (measured / estimated) | image | effort |
| - | ------ | --------------- | --------------------------- | ----- | ------ |
| 1 | `resolutionScale` 0.25-0.35 for animated content | every config change | 8 arcs 3.07 -> 2.29 ms (1.34x), 8 segments 5.40 -> 4.59 ms; P1b 1.20 -> 0.42 ms | <= 3 levels (`check`) | none |
| 2 | **built (I58)**: a two-channel field, so segment configs take the fields - measured 2.09x on hue frames, 1.98x still at 0.5 on an M2 | every hue and still frame of a config with segments | est. hue 2.44 -> ~1.0 ms, still 0.74 -> ~0.45 ms (P1b -> composite, ring -> its field) | within 1/255, like today's field | medium; +~1.1 MB |
| 3 | **half built (I61)**: narrower bell support (4.24 sigma); the 8-node rule failed the criterion (3 levels) and is not in | segment animation | 8 segments: bake 2.43 -> 1.26 ms, frame 5.40 -> 3.91 ms (1.38x) | <= 2 levels on ~3% of pixels in segment scenes; arcs unchanged | small |
| 4 | half-rate soft glow while only lights move (bake + P1b every other frame; filament and ring every frame) | light animation | est. -1 ms (8 arcs) to -2 ms (8 segments) | the halo may trail the line by one frame | medium |
| 5 | bloom-tail threshold (stop the quad and the blit where the glow falls under 2-4 levels) | every frame | blit and P1b area -11% (2 levels) to -21% (4 levels) on the default frame | the faint tail goes | small, plus a look decision |
| 6 | per-piece uniform-coverage skip (no table read on a piece fully lit or dark) | light animation with large arcs (a wipe) | est. up to ~10% of P1b; nothing for many small arcs | <= 1 level (the table's half-float rounding) | small |
| 7 | per-piece light lists in the bake (skip a light whose support misses the piece before any of its maths) | light animation | est. small: the bake already stops at a zero-length image loop | exact | small |
| 8 | cached per-piece kernel weights, so a light change re-shades from the table alone | light animation | est. P1b ~1.7x (the piece maths are ~58% of it) | exact | large; 16 halves per texel = 13.7 MB at 0.5, 3.4 MB at 0.25 |

Notes:

- **#1** is the plan's step 1 (`neon-reduced-scale-plan.md`), re-measured with
  many lights: it is still the largest no-code lever, and the only one that
  touches P1b.
- **#2** is that plan's D3, declined for its memory. The numbers are new: a
  config that keeps segments pays ~1.5 ms more on every rotating-hue frame than
  the same config with arcs (2.44 vs 0.90 ms), and ~0.3 ms more on every still
  one. The factorisation holds with segments: the output is
  `tonemap(col * A + segColHue * B)` with A and B independent of the hue, so the
  field becomes two channels (RG16F: 0.84 -> 1.7 MB for the glow's at 0.5, 0.27
  -> 0.54 MB for the ring's) and the composite reads both hues.
- **#3** was prototyped: the narrower support makes the 8-node rule more
  accurate than 8 nodes alone (<= 3 levels on 9.4% of pixels), because the same
  nodes cover a shorter range. The CPU's dirty-piece mirror would follow the
  narrower support and re-bake fewer pieces, so the real gain is a little
  larger than measured.
- **#4, #5, #6** change the image in ways a still capture cannot show (#4) or the
  owner has to accept (#5, #6).

## 9. Ruled out

- **Folding the field composite into the blit** (the plan's P4): the composite
  would run on the blit's 1.64M full-resolution pixels instead of 428k reduced
  texels - about 4x the fragments at 0.5.
- **A skip for the far straights' halo**: `GLOW_PIECE_MIN` already drops their
  table reads; what is left is two square roots per far edge.
- **A narrower ring**: its width is the reduced buffer's filament reach plus one
  guard texel - exactly what keeps the blocky line off screen.
- **Re-shading only where an arc's coverage moved**: the gathered colour is a
  long-range mean that moves a little everywhere (the plan, section 9).
- **A closed form for a segment's bell under the kernels**: a Gaussian against
  a Lorentzian-type kernel is a Voigt integral; tabulating it is #3's kind of
  approximation, with a 3-D table.
- **Area waste in the blit or P1b**: 96-98% of their fragments are visible
  (section 6).

## 10. What to measure on the target

This GPU is not the target. Before acting on section 8:

1. **Which frames production draws**, and how many lights move at once - the
   spread between frame types (0.42 to 5.4 ms here) decides everything.
2. **Half-float render targets** (`EXT_color_buffer_half_float`): without them
   neither field engages and every hue and still frame shades P1b and the ring.
3. **Fill rate vs ALU**: the blit is 1.64M blended pixels per frame at 1080p;
   a fill-rate-bound GPU will feel it more than this one.
4. **The CPU**: `OnConfigChanged` with many lights (the dirty-piece mirrors)
   is ~0.03 ms here; a slower CPU should be timed.
