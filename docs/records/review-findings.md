# Review findings

Open defects and rough edges found in a full read of the tree at
`improve_perf_by_emission_prepass` (`bbdba62`), with the visual ones
reproduced offscreen rather than argued from the source.

This document holds what is still OPEN, in full ([Open items](#open-items)),
the state table below, and [What is left](#what-is-left) - one line on every
item ever found. The closed items (FIXED, CHANGED, MOOT and the rest), each with
what was changed and how it was verified, and every pass's own introduction,
were removed from the tree on 2026-10-20: `git show
f153e1c:docs/review-findings.md` is the whole document as it last stood. Code
comments cite items as "V21 in `review-findings.md`": What is left has its one
line, git history has the rest.

> **Written before the neon unification.** Findings here describe a tree with
> two neon renderers, `NeonRenderer` and the half-res `NeonOptimizedRenderer`,
> plus the debug overlays living inside the first. Those are now one renderer
> drawing at `NeonConfig::resolutionScale`, with the overlays in a separate
> `DebugRenderer`. Two findings changed state as a result - **I3** is now fixed
> outright and **R6** is moot - and both say so in place. The rest are
> unaffected: the merge was verified byte-identical to each fork at the
> matching scale, so every pixel measurement below still holds. See
> `neon-unification-plan.md`.

Code references here name a **file and symbol**, never a line number. Line
anchors in this document had already drifted twice - once when the fixes below
landed and again when the helpers they added shifted everything under them -
and a wrong line number is worse than none, because it reads as precise. Each fixed item keeps its original
description, so the reasoning that led to the change stays readable next to it.

**A reference that is not a link names a file that no longer exists.** Keeping
the original descriptions means keeping references to `neon-optimized.frag`,
`neon-optimized-renderer.cpp` and `lens-flare-optimized-renderer.cpp`, all
deleted by the unification and the lens-flare merge. Those are written as plain
code spans rather than links, because a link to a deleted path is a 404 dressed
up as a citation. Everything still linked still exists, so in a list that mixes
the two - and several below do - the formatting tells you which halves of the
old fork survived.

| state | items |
| ----- | ----- |
| open | V9, V16, V18, V29, V32, I13, I18, I82, I83, I84, I86, I88, I90, I91, I92 |
| open by decision - documented, audited or a known limit | V5, I5, I8, I25, I28, V17 |
| fixed or changed | every other V, I and R item, through the thirty-seventh pass |

The R items come from a re-read after the V and I fixes landed - see
Second pass. V8 and V9 come from a later read of
`improve_renderer_by_LUT`, the branch that moved the LUTs behind
`BaseLUT` / `GradientRingLUT` / `SpanAtlasLUT` - see
Third pass. They continue the visual numbering
because both are visual; neither is caused by that refactor.

I13 and I14 come from a performance review of `LensFlareRenderer` - see
Fourth pass. That review's
own measurements and reasoning live in `lens-flare-perf-review.md`; only the
defects it turned up are recorded here.

I16 to I20 come from a read of the spotlight layer on
`add_spotlight_renderer` - see
Sixth pass. That pass found no
visual defect, so it adds no V item: the solved strip bound the whole renderer
rests on was verified offscreen and holds. Its design and the verification
behind it live in `spotlight-renderer-plan.md`.

V11 comes from a spotlight artefact reported from a render - see
Eighth pass. It is the visual item
the sixth pass did not find, and it is worth noting WHY that pass missed it:
the sixth pass verified the strip bound, and the strip bound was never the
problem. The reported symptom points straight at the geometry, which is what
makes this one interesting.

V12 comes from a spotlight render like V10 and V11 - see
Thirteenth pass. Half of what
was reported turned out to be designed behaviour and half a real defect, and the
first fix for the real half was worse than the defect; both are recorded there,
the second because the failed attempt is the useful part. V12a was its
remainder - overlapping lamps clipping their sum - and is fixed by screening the
lamps instead of adding them. V12b is the follow-up report that the fix had not actually landed -
it had, for the defect it was measured against, and the measurement was the
wrong one; read it before trusting any "no pixels clipped" result in this layer.

I21 to I24 come from a read of the spotlight CLIP AREA on
`add_clipping_area_for_spotlight` - see
Twelfth pass. The clip landed after the
sixth pass, so none of it had been reviewed. All four are fixed. They add no V
item, but I21 was a genuine visual defect rather than a rough edge - it is
recorded as an I because it is gated on a non-default `resolutionScale`, so no
render ever showed it.

V16 comes from measuring a cap on the edge ring's width - see
Fifteenth pass. The cap was rejected
(section 7 of `neon-resolution-scale-plan.md`); V16 is what the uncapped
baseline of that comparison turned up, in the shipped code.

V30, V31, V32 and I77 to I84 come from a read of the whole tree at `1c76c3e` -
see Thirty-fourth pass. Most of
them sit outside the neon's skip keys, which `neon-scale-check sequence`
already covers: in the time model, the animation system, the C ABI and the
droplets. The five that were reproduced - V30, V31, I77, I78 and I79 - are
fixed; the rest are open. The same pass adds notes to V28, V29 and I76.

V33, V34 and I85 to I89 come from a second read of the tree the same day,
over the code the thirty-fourth pass only skimmed and over its own fixes - see
Thirty-fifth pass. That pass
changed no code; V33, V34, I85, I87 and I89 have been fixed since, with I76,
I80 and I81 from the passes before it, and I86 and I88 are open.

I90 to I92 come from a third read the same day, over the orchestrator, the
shared buffer, the GL wrappers, the modulators and the C ABI's lifecycle - see
Thirty-sixth pass. All three
were reproduced and all are open; that pass changed no code. It also re-found
I13, and added a measurement to it.

V35 comes from `neon-scale-check sequence` on the shared-buffer branch, after
a shader refactor that moved no pixel failed it on the same three frames as
the tree before it - the thirty-seventh pass, one item, fixed.

## How the visual items were reproduced

A throwaway harness linked against `build/lib/libedge-lighting.a`: a hidden
GLFW window, an `OffscreenCapture` at 1000x800, one `SetConfig` / `Update` /
`Render` per case, `CaptureUtil::WritePNG` out. Every case starts from the
same base config:

```
geometry: 600 x 400 at (200, 200), cornerRadius 40, winding CCW (the default)
neon.enable = true, everything else at its Config default
neon.colorTransitionDuration = 0   // no cross-fade, so one frame settles
neon.hueRotationRate = 0           // except where the case is about rotation
```

Two notes for anyone re-running this. `hueRotationRate` defaults to `0.5`, so
leaving it on makes every capture time-dependent and A/B comparisons drift by a
few LSB per frame. And `colorTransitionDuration` defaults above zero, so the
first frame after a colour-stop change still shows the **old** ring - a
single-frame capture of a new gradient is the cross-fade's start, not its end.

Checked and found correct, for the record: stop sorting (unsorted and sorted
inputs render bit-identical), CW/CCW agreement between
`GeometryUtils::GetPointOnRectangle` and the shader's `perimeterPosition`
across all eight perimeter spans, colour-stop alpha gating filament, halo and
bloom together at the right position, `operator==` coverage on every `Config`
sub-struct, `NeonRenderer` vs `NeonOptimizedRenderer` agreement (mean
|diff| 0.16/255, max 7 at `glowRadius` 30), and no beading at
`optimizedNeon.numSamples = 16`.

---

## Open items

Every item still open, in full, V before I and in number order. Each names the pass that
found it, in the archive, where the closed items and the passes' own
introductions live.

### V9. An arc's own gradient quantises to the gather grid, and a reduced sample count makes it visible - OPEN

*From the Third pass.*

**Confirmed.** The two neon renderers agree everywhere at max 7 - except when
arcs carry their own colour stops, where they diverge by up to 100/255 in tight
clusters at each arc's gradient endpoints. Sweeping
`OptimizedNeonConfig::numSamples` isolates the cause exactly:

| config | mean | max | px with diff >= 30 |
| ------ | ---- | --- | ------------------ |
| 2 arcs, own stops, `numSamples` 64 (**the default**) | 0.677 | **100** | 389 |
| 2 arcs, own stops, `numSamples` 96 | 0.441 | 38 | 32 |
| 2 arcs, own stops, `numSamples` 128 | 0.359 | 10 | 0 |
| 2 arcs, no own stops, `numSamples` 64 | 0.337 | 7 | 0 |
| 2 arcs, no own stops, `numSamples` 128 | 0.331 | 7 | 0 |

The pre-pass evaluates the arc-local coordinate at the gather samples -
`si = floor(gl_FragCoord.x) * invNumSamples`, then `uArc = rel / length` - so an
arc's own gradient is resolved to `numSamples * length` distinct levels rather
than the atlas row's full width. `NeonRenderer` always walks
`NEON_MAX_LOOP_SAMPLES` = 128; `OptimizedNeonConfig::numSamples`
([`config.h` OptimizedNeonConfig](../../lib/include/core/config.h)) defaults to 64.
At `length 0.5` that is 32 levels across a 128-texel row.

It shows at the endpoints rather than mid-span because that is where the gather
has fewest contributing samples to average across, so the quantisation is not
smoothed by neighbours. The clusters sit exactly on the arc endpoints: a single
full-perimeter arc with its own stops produces one cluster at its head/tail
junction, and two abutting arcs produce one at each seam.

This is the same mechanism V5 records, seen from the other side. V5 framed it as
the arc's colour feather and its magnitude feather scaling differently, and
closed it as a residual on the grounds that nobody had reported an effect. The
sweep above shows it also produces a hard disagreement between the two
renderers, which the parity check in the second pass did not catch because its
cases used arcs WITHOUT their own stops - exactly the row of the table that
reads max 7 at every sample count. **V5's entry should be read with this
alongside it**: the two feathers are not merely uncoupled, the colour one also
loses resolution against the half-res renderer's default.

**Not fixed**, and the options are not equally good:

- Raising the `numSamples` default to 128 closes the divergence but discards the
  knob's whole purpose - it is the optimized renderer's main performance dial,
  and 64 was presumably chosen deliberately.
- Evaluating the arc-local coordinate pointwise instead of at the samples is
  the correct fix, but that is what the emission pre-pass exists to avoid: the
  invariant stated at the top of `neon-emission.frag` is that a pure function of
  `(si, uTime, config)` belongs in the pre-pass. Moving the arc colour out of it
  would put a per-fragment LUT fetch and winner search back on the hot path, at
  which point the pre-pass has given up most of what it bought.
- Interpolating between adjacent samples' arc colours in the consumer would
  recover most of the resolution for one extra texelFetch, without moving
  anything back. This looks like the right trade, but it changes the gather's
  arithmetic and would want measuring against `emission-prepass-comparison.md`'s
  numbers before it lands.

Left open with the mechanism recorded, because the third option is a real design
decision rather than a patch, and because the visible symptom is confined to a
few hundred pixels at arc endpoints in a configuration (per-arc stops on the
half-res path) that may well have no user today.

### V16. A crisp, thin line with no glow misses the edge ring's 2/255 bar - OPEN

*From the Fifteenth pass.*

**Measured, in the shipped code.** At `glowRadius` 0 a crisp, thin filament
reads more than 2 levels off its 1.0 render at every reduced scale. Max error,
and the pixels over 2:

| `filamentFalloff`, `lineWidth` | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ------------------------------ | ---- | --- | ---- | ---- | ----- |
| 4, 1 | 6 (25 px) | 8 (25) | 6 (25) | 6 (24) | 8 (23) |
| 4, 2 | 4 (14) | 4 (17) | 4 (19) | 5 (17) | 5 (22) |
| 4, 4 | 3 (2) | 3 (408) | 2 | 2 | 3 (8) |
| 2, 1 | 5 (31) | 4 (17) | 3 (10) | 4 (16) | 3 (12) |

Width 8 at falloff 4, and width 2 and up at falloff 2, hold 2 everywhere. So
does every row above at `glowRadius` 2 or 5: any glow covers it.

**Two mechanisms, told apart by where the pixels are.**

- **On the corner arcs, widths 1 and 2.** Every failing pixel lies on a
  rounded corner at the filament's shoulder - |d| 0.6 px at width 1, 1.2 px
  at width 2 - with errors of both signs. The error is about the same size at
  0.75 as at 0.125, survives a single colour stop, is absent at `cornerRadius` 0
  and grows with the radius (`cornerRadius` 120: 4-6 levels on 64-92 px).
  Not confirmed, but those four point at the ring program (`NEON_RING_PASS`)
  and the direct program computing an arc's distance very slightly
  differently. A straight edge's distance is a subtraction and agrees exactly;
  an arc's goes through a `length()` whose rounding scales with the radius;
  and a falloff-4 shoulder at width 1 falls from half to nothing in 0.2 px, so
  a few thousandths of a pixel move a pixel by several levels. The scale only
  decides that the ring program draws those pixels, which is why the error
  does not grow as the scale falls. It also moves with the rect's sub-pixel
  position: shifting the rect by (+0.37, +0.61) takes width 2 from 4 to 2 at
  0.5 and from 5 to 3 at 0.25.
- **Along the top edge, width 4 at 0.5.** 407 of the 408 pixels are in two
  rows on the top edge: 295 of row 177 (d = +2.5 px) and 112 of row 182
  (d = -2.5), each 3 levels off. They vanish with a single colour stop, so
  this half is the gathered hue the ring reads back from the reduced buffer. That hue is a distance-weighted average of the perimeter's
  colours, and how wide the average reaches changes with distance from the
  line, so it varies across the shoulder; a 2 px texel pitch does not rebuild
  that exactly. At 0.35 and 0.25 this config happens to read 2.

**Why the calibration missed it.** Step 5's calibration swept these configs -
`glowRadius` 0, `filamentFalloff` 2 and 4, `lineWidth` 1-4 - and recorded a
worst of 2 at 0.5 and 0.25. The same notes' cost numbers are an Apple M2
Pro's, so the sweep most likely ran there. If the arc mechanism is what it
looks like it depends on the GPU's compiler, and the
sub-pixel dependence means another rect position could have missed it too.
Not re-measured on the M2 Pro. Nothing routine catches it: every one of
`neon-scale-check`'s twelve scenes has some glow (the hairline's
`glowRadius` is 3).

**Severity: low.** It needs `glowRadius` 0, and a filament crisp and thin
enough to have a sub-pixel shoulder. At worst it is 8 levels on 25 pixels of a
1280 x 720 frame, at the line's corners, or 3 levels along parts of two rows
at one edge.

**Not fixed.** If the arc half is a compiler difference, GLSL 3.30 and ES 3.00
cannot forbid it - `precise` arrived in GLSL 4.00 and ES 3.20 - and routing
both programs through one distance function narrows the chance without
guaranteeing it. The useful next steps are cheap: re-measure on the M2 Pro,
which decides whether there is anything GPU-independent to fix, and add a
glow-free crisp hairline to `neon-scale-check` with a bound at today's
numbers so the error cannot grow unnoticed. Repro: `BaseConfig()` with
`filamentFalloff` 4, `lineWidth` 2, `glowRadius` 0, at `resolutionScale` 0.5
against 1.0.

### V18. Colour-stop alpha does not dim the halo or the bloom - OPEN

*From the Eighteenth pass.*

`ColorStop::color.a` was documented (in `config.h`, in the `neon.frag` comment
on the gathered coverages, and in V14's "known limit" above) as attenuating
the filament, halo and bloom together, or at least as reaching the glow
"through the emission colour". It reaches only the filament. The emission
colour is the LUT's straight RGB, and the gathered coverages
(`emitCoverGathered`, and `gatheredSeg` from `segCoverGathered`) are built
from `arcW` and `bellSum` without alpha, so neither factor of `emitGlow`
carries it. (V19's per-piece correction reads `coverageAt`, which leaves
alpha out the same way, so V18 is unchanged by it.)

Measured with a 600 x 360 rect at (200, 150), one colour stop, hue rotation 0,
`colorTransitionDuration` 0, on Mesa llvmpipe; brightest channel at x = 500:

| config | alpha | on the line | 6 px out | 20 px out | 60 px out | centre |
| ------ | ----- | ----------- | -------- | --------- | --------- | ------ |
| filament only (`glowRadius` 0) | 1.0 | 244 | 38 | 8 | 8 | 8 |
| | 0.5 | 234 | 25 | 8 | 8 | 8 |
| | 0.0 | 8 | 8 | 8 | 8 | 8 |
| glow only (`lineWidth` 0, `glowRadius` 20, bloom 1.0) | 1.0 | 203 | 201 | 192 | 175 | 169 |
| | 0.5 | 203 | 201 | 192 | 175 | 169 |
| | 0.0 | 203 | 201 | 192 | 175 | 169 |

`main` (`1b5cf94`) gives the same table to the level, so this is not a
regression of the resolution-scale branch: it dates from V14, which moved the
glow onto the gathered coverage. (V14's own pass checked alpha against the
pointwise coverage the glow then used - the "checked and found correct" list
in the first pass - which is why that check passed.)

**Documented, not fixed.** `config.h`, the shader comment and V14's note now
say what the code does. The fix is a design call rather than a patch: either
carry alpha into the gather (a third emission-table row, or alpha folded into
row 0's weight, which changes what `arcW` means to the colour normalisation),
at the cost V14 measured for a third fetch, or accept that alpha is a
filament control and say so in the API. Until then, dim the glow along part of
the ring with arcs or an arc's `intensity`.

---

### V29. A segment move leaves a one-level stale texel that only an arc re-bake clears - OPEN

*From the Thirty-third pass.*

**Found** by `exact` on `sequence --sequences 100 --frames 150 --only 70`
(seed 1), frame 45, with V28's fix applied: 1 level on 1 frame pixel and 38
shared-buffer texels, the only `exact` failure in 15,000 frames. Cleared by
bypassing `GLOW_COVER` or `REBUILD` - both of which re-bake every piece of the
coverage table, arc channels included.

The frame: 960 x 540, a 189.7 x 70.4 rect with sharp corners, scale 0.75, no
arcs at all, one segment (length 0.206, two stops) beside a preserved one
(length 0.045), glow radius 38.6, bloom 1.5, intensity 3, outside glow. The
segment steps 0.871 -> 0.841; nothing else moves. The next frame moves the rect
and re-bakes everything, so here it lasted one frame - but a segment that
moved once and then held would keep it.

**What it is not**, each tested in the scratch worktree: the segment piece
mask (`GetGlowCoverDirtySegmentPieces`) - mirrored by hand, the old and new
bells reach every piece but the zero-length corner at 0.5, and dirtying all
four corners, or all eight segment pieces, still fails. **What clears it**:
setting `dirtyArcPieces` to every piece whenever a segment piece is dirty. So
on this config the table's ARC channels (.r / .g), or something that bake
writes, depend on the segments, which the per-type split (I42) rests on never
happening. `pieceCover` in `neon-glow-cover.frag` integrates the two kinds
independently, so the dependency is likely on the CPU side or in the
segment-table path (pass 0s / 0f, `segDirect`); not pinned down. Only 1 level, but
the split is documented exact and verified byte for byte against a full bake,
so this is a hole in that verification, not a tolerance.

### V32. The lens flare is never dithered - OPEN

*From the Thirty-fourth pass.*

**Found** by reading: `lens-flare.frag` writes its colour straight out at both
resolution paths, and below 1.0 into `mScaledBuffer`, which is RGBA8 (the
`Framebuffer::Resize` default). The flare is the smoothest light the library
draws - a soft glow, ghosts and rays - so it is the layer most exposed to the
mechanism R7 fixed for the neon and V27 for the spotlight: slow gradients
crossing an 8-bit level only every few px draw contour rings, and below 1.0 an
RGBA8 buffer rounds the tail to plateaus that the blit's bilinear read joins
into diamond-shaped bands. **Not measured.** The cure, if it shows, is the
other two layers': a half-float buffer read undithered, and the noise added
once per destination pixel on the write to the caller's target.

### I13. `pow()` takes a negative base in the ghost ring term - OPEN

*From the Fourth pass.*

`circle()` in `lens-flare.frag` evaluates

```glsl
c1 = max(FLARE_RING_BIAS - pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) + sl, 0.0) * 3.0;
```

where `l = length(...) + size * FLARE_RING_L_BIAS` and `size` is
`LensFlareConfig::ghostSize`. With `FLARE_RING_L_BIAS` 0.5 and
`FLARE_RING_SHIFT` 0.3, any `ghostSize` below 0.6 lets `l - FLARE_RING_SHIFT`
go negative near the ghost centre, and `pow` of a negative base is **undefined
in GLSL** - not merely inaccurate. What comes back is a driver's choice: a NaN
that then propagates through the accumulated `color`, or a silently wrong
finite value.

Not reachable from either demo: both `SliderWithInput("Ghost Size##Lens", ...)`
in `DebugUI` and its `demo-capi` twin start at 1.0. It is reachable from the C
ABI, where `el_effect_set_lens_flare_ghost_size` stores the float it is given
with no clamp, as does `LensFlareConfig` itself.

Captured at `ghostSize` 0.5 and 0.0 on this machine's GPU, the frames are
well-formed - so this driver returns something finite - which is exactly why it
is recorded rather than assumed benign. The CPU side is already guarded:
`GetGhostRingFloor` clamps its own `pow` base to zero, so the gate it derives
stays correct in that range and simply stays open. The shader expression is
untouched.

Closing it means either clamping in the shader (`max(l - FLARE_RING_SHIFT,
0.0)`, which is exact wherever the current expression is defined and makes the
term defined everywhere else) or clamping `ghostSize` at the config boundary,
which changes what the C ABI accepts. The first is the smaller change; the
second is the one that also protects the bloom term's `pow` exponent. Neither
was taken, because both are behaviour decisions rather than repairs.

**Re-found on 2026-10-20** by the
Thirty-sixth pass, which first
recorded it again as a new item before finding this one. It adds one
measurement: a one-texel shader evaluating `pow(x - 0.3, 1.0 / 40.0)` on an
Intel UHD Graphics 630 returns 0.961 at x = 0.1 and 0.891 at x = 0.29 - the
result for the absolute value of the base - and no NaN. So that driver draws a
ring term near a small ghost's centre where another may draw nothing or a NaN:
the per-GPU difference `BakeGhostTable` was built to remove. The decision above
still stands; the shader clamp moves no pixel at any `ghostSize` of 0.6 or more.

### I18. The visibility floor is divided by a lamp count the blend path cannot honour - OPEN

*From the Sixth pass.*

`SPOT_VISIBILITY_FLOOR` is documented as *"HALF an 8-bit step"*, and
`buildStrips` divides it by the number of enabled lamps before solving, so that
*"the whole rig's clipped remainder stays inside one half step however many
lamps overlap: N lamps each under floor/N sum to under floor."*

The arithmetic is right and the guarantee is real, but it is finer than the
render path's own precision, so nothing downstream can observe it. All lamps go
out in one `glDrawArrays`, but they are still separate fragments: each one's
premultiplied add is blended into the RGBA8 target and rounded to eight bits
before the next arrives. Eight lamps that each contribute 0.4 of a step to a
pixel therefore write zero eight times, where an exact float sum would have
reached 3.2 steps.

Measured on an eight-lamp fan at 1280x720, against a CPU evaluation of
`spotlight.frag` that sums in float and quantises once:

| rendering | channels below the model, all at value 1 | max deviation |
| --------- | --------------------------------------- | ------------- |
| each lamp alone, eight runs | 0, 1, 1, 1, 1, 2, 2, 5 | 1 |
| all eight together, one run | 36,853 | 3 |

The per-lamp rows are the strip bound doing its job. The 36,853 is entirely the
eight-bit accumulation, and dividing the floor did not prevent any of it.

What the division does cost is fragments. Headroom grows by `ln(N)` and the
solved half-width goes as its square root, so at eight lamps every strip is
about 15% wider than a shared-floor solve would make it - paid to guarantee a
sub-half-step total that the blend then discards. Whether to drop the division,
keep it and say what it does and does not cover, or move the whole rig into a
higher-precision accumulation buffer is a design call, which is why this is
recorded rather than changed. Note the third option is not free here: this layer
composites straight onto the caller's target at `resolutionScale` 1.0, which is
the default and the path the measurements above use.

Related, and already recorded: **R7** measures the same eight-bit ceiling from
the other side, on the neon halo and bloom.

### I82. The output dither is added where the glow is 0 - OPEN

*From the Thirty-fourth pass.*

**Found** by reading the three writes R7 dithers - `neon-blit.frag`, the edge
ring (`neon.frag` with `uBlitOwnsCut` 0) and the ring field's composite
(`neon-ring-field.frag`): each adds `neonDither`'s noise to its
colour unconditionally, where the glow is an exact 0 as well. Three
consequences:

- On an 8-bit UNORM target, which is what R7 was measured on, nothing shows:
  the noise is under half a level and a premultiplied write of it over an exact
  level rounds back. On a 10-bit, half-float or sRGB host target it is noise
  added to the HOST's pixels wherever the blit draws, which reaches out to the
  glow's fade margin.
- The blend cannot skip the empty pixels. The spotlight's blit measured that
  writing a dithered near-zero rather than an exact 0 over every empty pixel
  was part of its first version's 1.26-1.33x (V27).
- The proofs that `GetGlowInnerReach`'s interior hole and `GetCornerSkip`'s
  skipped arcs change no pixel predate R7. They bound what is left under half
  an 8-bit level, which rounded to 0 without the dither; with it a pixel the
  glow would draw at 0.3 of a level rounds up about 30% of the time, so the
  hole and the skip move such pixels by 1 level, stochastically. The comment
  in `setupRingGeometry` that the texels past `mGlowInnerReach` are "stored as
  0" holds for the RGBA8 fallback only; in half float they hold the small
  value.

The fix for the first two is a dither that leaves an exact 0 alone (gated on
the colour, as the spotlight's blit gates on an empty texel); the third is a
statement in `glow-inner-reach.md` that the bound is "within the dither"
rather than "no pixel".

### I83. What the library leaves changed in the host's GL state is not documented - OPEN

*From the Thirty-fourth pass.*

**Found** by reading the wrappers. The library documents what it ASSUMES of
the caller's state - no face culling, a full colour and alpha write mask,
`GL_FUNC_ADD`, depth off - and `EdgeLightingEffect::Render` no longer forces
any of it, but what a frame LEAVES is not listed anywhere: its last vertex
array and `GL_ARRAY_BUFFER` bound (`VertexArray::DrawArrays` binds and does
not unbind), program 0, its textures on units 0 to 7 with the active unit
moved, the uniform-buffer binding points 0 to 3, and `GL_BLEND` enabled with
`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` - every renderer ends on that, enabled
whatever the host had. Two assumptions are missing from the list too: pixel
UNPACK state - `BaseLUT::Upload` hands `glTexImage2D` / `glTexSubImage2D` a
client pointer, which a bound `GL_PIXEL_UNPACK_BUFFER` turns into an offset
into it - and the read framebuffer, which `RenderTargetState` restores through
`GL_FRAMEBUFFER` and so rebinds to the draw framebuffer. A host whose GL layer
caches state, as DALi's does, draws its next frame on stale assumptions. The
fix is both lists in `BaseRenderer` and `EdgeLightingEffect::Render`'s docs,
and an opt-in save / restore scope beside the existing `GLUtils` guards.

### I84. The embedded shader text is 85% comments - OPEN

*From the Thirty-fourth pass.*

**Measured** on `build/lib/generated/shaders.h` with a regular expression that
drops `//` and `/* */` comments and blank lines: 692 KB of shader source
across the library's programs, 105 KB without comments. `NEON_FRAG_SRC` is
150 KB, 32 KB without; `neon-tuning.h` alone injects about 50 KB of prose into
each of the seven neon shader sources, ten programs between them - the emission
pre-pass is 54 KB of source for 5 KB of code. All of it is compiled into the
library and handed to the driver's compiler, whose first-frame cost several
items above went to lengths to cut. Stripping comments in `lib/CMakeLists.txt`
before `configure_file` - GLSL has no string literals, so it is safe on the
shader files; the tuning headers' `#define` lines survive it - would remove
both costs, and one more if the `PLATFORM_WINDOWS` branch is ever finished:
MSVC refuses a string literal over 16 KB (C2026).

### I86. The C ABI logs on paths a host calls every frame - OPEN

*From the Thirty-fifth pass.*

**Found** by reading `log-util.h` and the C ABI, and measured where it could
be on this machine:

- `capi-internal.h` compiles `LOG_D` out only under `PLATFORM_MACOS` or
  `PLATFORM_WINDOWS`. On `PLATFORM_LINUX` - the Tizen build - it stays live, and
  74 of the 82 `el_effect_get_*` getters log a formatted line through
  `std::cout` on every call. `demo-capi`'s UI code calls 63 distinct getters,
  55 of them logging ones, as it lays out each frame; that demo does not build on Linux, but a C# host on
  Tizen reading its UI state through P/Invoke every frame is the same shape.
- `el_animation_update` and `el_animation_apply` log at INFO on EVERY call, on
  every platform. Measured: one animation driven through the pair for 60
  frames printed 120 lines.
- `Util::Print` has no level filter and no platform sink: every level goes to
  `std::cout`, which on a Tizen device is not where its log reader looks.

Each line is a `std::string` built by `FormatString` plus an `iostream` write,
on the frame path. The fix is a level threshold in `log-util.h` (compile time
at least), the two per-frame INFO lines moved to DEBUG, and `LOG_D` compiled
out of the C ABI on every platform rather than two.

**Retracted.** Read first as "every C ABI getter spams the log", which a probe
on this Mac contradicted: 87 calls of a logging getter printed nothing, because
of the `PLATFORM_MACOS` branch above. The claim holds only for Linux, as given.

### I88. The spotlight overwrites its strip buffer in place on every animated frame - OPEN

*From the Thirty-fifth pass.*

**Found** by reading `SpotlightRenderer::buildStrips`, which runs on every
config change - every frame under a lamp animation - and writes the strips
with `glBufferSubData` into the ceiling-sized store `ensureBuffer` allocated
once. The neon made the opposite call for its meshes on purpose
(`NeonRenderer::setupGeometry`): `glBufferData` lets the driver orphan a store
the GPU may still be reading and hand back fresh memory, while "a SubData into
that same store is what risks an implicit sync". On a tiler, where the
previous frame's draw is often still in flight when the next frame's CPU work
runs, that is the stall the neon avoided. **Not measured.** The two layers
should follow one rule, chosen by a measurement on the target: orphan with
`glBufferData` (the ceiling is 34 KB), or round-robin two stores.

### I90. A modulator sequence that contains itself hangs every evaluation - OPEN

*From the Thirty-sixth pass.*

**Found** by reading `Sequence` in `modulator.h`: `Append` takes any
`ModulatorPtr`, and `Evaluate` calls each stage's `Evaluate`. It is the one
modulator a stage can be added to after it is built - `Multiplier`, `Adder` and
`Remap` take their inputs at construction, so they cannot close a loop - and
`el_modulator_sequence_append` forwards to it with no check.

**Measured** through the C ABI: `el_modulator_sequence_append(a, a, 1.0)`
returns `EL_SUCCESS`, and so do the two appends of a mutual pair (b into a, a
into b). In both cases `el_modulator_evaluate(a, 1.5, &v)` did not return: the
probes were still running after 5 s and were killed. Where the cycle is the
first stage the time never drops; the Release build spun rather than
overflowing its stack, most likely because the call compiles to a tail call
(not checked in the disassembly). When a finite stage
comes first, the recursion only goes as deep as the time takes to fall below a
stage's duration and returns - which is how a first probe read 0.5 and missed
it. An animation driven by such a sequence hangs `el_effect_update` the same
way, and the cycle's `shared_ptr`s keep each other alive after every handle is
destroyed.

**The fix** is to refuse, in `Sequence::Append` (and so at the C ABI, with
`EL_ERROR_INVALID_PARAMETER`), a stage that already reaches the sequence. That
needs a way to walk a modulator's inputs - a virtual that lists them, or a
`Reaches(const Modulator *)` - since `Multiplier`, `Adder` and `Remap` can sit
between the two.

### I91. A second `el_effect_init` replaces the effect and drops its animations and clock - OPEN

*From the Thirty-sixth pass.*

**Found** by reading `InitLayers` in `el-effect.cpp`, which every
`el_effect_init*` goes through: it assigns a new `EdgeLightingEffect` to
`effect->impl` whether or not there is one already. The renderers are rebuilt,
which is what a host re-initialising wants, but the attached animations and the
clock go with the old effect.

**Measured** with a hidden GLFW window: an effect initialised, an intensity
pulse attached and the clock paused read "attached 1, clock playing 0"; after
a second `el_effect_init_with_renderers` - which returned `EL_SUCCESS` - it
read "attached 0, clock playing 1". Nothing is logged, and `el-effect.h` says
nothing about calling init twice. A host that re-initialises after a context
loss (the case where calling init again is most likely) keeps animation
handles it believes are attached, which the effect no longer advances. The
staging config survives, since it lives on the handle.

**The fix** is a decision for the owner: refuse a second init with an error,
or carry the animation list and the clock over to the new effect. Either way
the header should say which.

### I92. An animation attached to two effects advances twice a frame - OPEN

*From the Thirty-sixth pass.*

**Found** by reading `AnimationManager::Attach`, which refuses a duplicate in
its own list but cannot know about another effect's, and
`EdgeLightingEffect::Update`, which advances every attached animation by its
own clock's delta. An animation is shared by `shared_ptr`, so attaching one to
two effects is legal in both APIs.

**Measured** with two effects and no renderers: one `IntensityPulse` attached
to both and 60 frames of `Update(1 / 60)` on each read an elapsed time of
2.000 s. A host with two windows driving one preset would see it run at double
speed, and its two effects would also disagree about where it is whenever their
clocks differ, since each one's delta moves it. Nothing in `edge-lighting.h`,
`animation.h` or `el-effect.h` mentions it. **The fix** is either to document
that an animation belongs to one effect, or to refuse the second attach - which
needs the animation to record its owner.

## What is left

The second pass's R1 to R6 have all landed, and so have the third pass's V8,
I9, I10 and I11. I3's structural half - the last thing on this list that was
open rather than declined - closed with the neon unification, which deleted the
fork it followed from. The fifth pass's I15 landed with it. The seventh through
tenth passes are one item each and all four are fixed, as are the eleventh's one,
the twelfth's four and the fourteenth's V15; its I25 is documented rather than
fixed, and the fifteenth's V16 is open. The sixteenth pass fixed I26, I27, I29
and I30, and documented I28 and V17. The seventeenth fixed I31 and I32. The
eighteenth opened V18, and the nineteenth fixed V19 and V20 and opened V21, since fixed too. The twentieth fixed I33 and I34, the twenty-first I39 to I46 (I39 closing I2), the twenty-second I47, I48 and I49, the twenty-third I50, I51 and I52, the twenty-fourth I53, I54, I55, I56 and I57, the twenty-fifth I58, the twenty-sixth V23 and R7, the twenty-seventh V24, the twenty-eighth I59, I60, I61 and I62, the twenty-ninth V25, the thirtieth V26, the thirty-first I63, and the thirty-second V27. The thirty-third opened V28, V29 and I76, all found by `neon-scale-check sequence`; V28 and I76 are fixed since, V29 open. The thirty-fourth, a full-tree review, fixed V30, V31, I77, I78 and I79 and opened V32 and I80 to I84. The thirty-fifth, a second read the same day, opened V33, V34 and I85 to I89; V33, V34, I85, I87 and I89 are fixed since, as are the thirty-fourth's I80 and I81. The thirty-sixth, a third read, opened I90 to I92 and added a measurement to I13. The thirty-seventh fixed V35, found by `sequence`. Three items from the
first pass - V5, I5 and I8 - remain deliberately open, each with the reasoning recorded next to
the code rather than only here, plus V9 from the third (I12's remainder closed
with the 2026-10-09 docs cleanup), I13 from the fourth, and I18 from the sixth:

| item | state | why |
| ---- | ----- | --- |
| V4 | fixed | the premise was wrong: `ad` never needed softening, one infinite-line term was being evaluated where four finite-segment ones belong |
| V10 | fixed | V4's own unmodelled corner: the straights ran past the tangent point, so a phantom emitter lit the outside of every rounded corner |
| V12 | fixed | V10's own unmodelled centre: the developed arc ran at rate `r`, which is right only on the arc, so it creased and under-counted at each arc's centre of curvature |
| V14 | fixed | V4's third half: the halo/bloom FIELD lost its medial-axis crease, but the nearest-point coverage that SCALES it kept one, so any partly lit perimeter cut the glow to a hard-edged polygon |
| V13 | fixed | V4's other half: the sampling floor was a fixed half width, so at a soft falloff - where sigma multiplies a 64-sigma tail - it doubled the filament to buy five levels of peak |
| V5 | residual, documented | closing it means plumbing pixel-space feathers into the pre-pass for an effect nobody has reported; read V9 alongside it, which measures the other half of the same mechanism |
| I2 | fixed by I39 | declined first for its staleness risk; I39 keys the table on what its pass binds, which needs no list of config fields |
| I5 | documented | the alternative is a breaking renderer-API change for an unmeasured cost |
| I8 | audited, no UI written | the C ABI itself is complete; what is missing is `demo-capi` coverage, ranked in the section above |
| R7 | fixed | the dark tail's one-level plateaus drew contour rings; the three writes to the caller's framebuffer now add a +/- half-level dither, and the reduced buffer is half float so the blit has something left to dither. Widest plateau 17 -> 7 px outside, 32 -> 10 inside. +0.01 ms a still frame at 1080p (likely lost framebuffer compression), the reduced buffer's memory doubled |
| [V9](#v9-an-arcs-own-gradient-quantises-to-the-gather-grid-and-a-reduced-sample-count-makes-it-visible---open) | open | the honest fix is a design decision (interpolate the arc colour between adjacent samples in the consumer), not a patch; the three options are ranked in the section |
| I12 | fixed | the live shader comment was corrected first; the two design documents that still named the removed LUT functions were removed on 2026-10-09 |
| [I13](#i13-pow-takes-a-negative-base-in-the-ghost-ring-term---open) | open | undefined `pow` reachable only through the C ABI; both cures change what the boundary accepts or what the term computes below `ghostSize` 0.6, so it is a behaviour decision rather than a repair |
| [I18](#i18-the-visibility-floor-is-divided-by-a-lamp-count-the-blend-path-cannot-honour---open) | open | the division guarantees something the 8-bit blend discards, and the three ways out - drop it, document its limit, or accumulate at higher precision - are a design call, not a fix |
| V12a | fixed | per-lamp shouldering cannot bound a SUM, so the blend does: the lamps screen instead of adding. Putting the shoulder on the sum was built and measured 2.5x to 9.3x slower, with a crease where differently coloured beams cross |
| V15 | fixed | the reduced-resolution neon drew every edge near the line into a buffer that cannot hold one; the cutoffs moved into the blit and the line into a full-resolution edge ring |
| I25 | documented | below 1.0 the scaled path has a fixed cost (composite, clears, edge ring), so a layer already cheap at 1.0 can render slower; capping the ring for soft filaments is decision 7 in `neon-resolution-scale-plan.md` |
| [V16](#v16-a-crisp-thin-line-with-no-glow-misses-the-edge-rings-2255-bar---open) | open | a crisp, thin line with no glow reads up to 5 levels off at its corners below 1.0 (8 at `cornerRadius` 120) on the frames that shade the ring directly - every frame of an animation; steady frames, which composite the ring's field, are within 1 since V24. Confirmed on the M2 Pro (addendum 2): the scale moves the ring's mesh, so `vPos` at a corner pixel shifts by thousandths of a pixel, which a sub-pixel shoulder magnifies; covered by any glow |
| I28 | documented | the opaque fill's own cutoffs (default off) and the outward `Cutoff::softness` move existing hosts' pictures without a compile error; `upgrade-notes.md` has the translation |
| V17 | documented | small rects lose most of the reduced scale's quality (20 x 17: 53 levels at 0.25), as before the edge ring; keep them at 1.0 |
| I31 | fixed | below 1.0 the gather loop ran at every reduced texel; it now runs once on a grid set by its own smoothness, 3.4-4.3x faster at 0.5 on most scenes |
| [V18](#v18-colour-stop-alpha-does-not-dim-the-halo-or-the-bloom---open) | open | colour-stop alpha dims only the filament; the halo and bloom ignore it (measured, `main` too); the cure is a design call (a third gather row or a redefined arc weight) |
| V19 | fixed | V14's other half: the gathered coverage was right far from the line and too wide on it, so a narrow halo drew a thin line along a stretch no arc covers (13-68 levels at `glowRadius` 1, now 0); each piece now blends toward its foot's coverage by how much of its own kernel lies past the nearest arc end |
| V20 | fixed | V14's last limit, uncovered by V19: every piece was scaled by the coverage gathered around the FRAGMENT, which dips on a dark line, so on a small rect with strong bloom an unlit stretch read 3-6 levels darker on the line; each piece now reads its own coverage from a table baked per config change (pass 0b) |
| V21 | fixed | V20's table convolved the coverage along a straight line through each piece's foot, past the piece's ends, so light spilled round corners (2-42 levels off an exact per-piece reference); now a table per piece - each straight by projection, each corner by polar position, behind the centre a blend of both developments, each band shared by a straight and a corner in proportion to their lengths - within 2 levels of a brute-force reference on every scene but 62 pixels of one (3). 1.0 MB against V20's 2, an animated bake half as costly, and a faster `Initialize`; the bake is compiled on the first frame that needs it, and the corner block runs one arc at a time, which is what kept a fully lit ring's cost from rising |
| I32 | fixed | the edge ring's gather attachments tripled the scaled path's memory; now 2.15 MB at 1080p and 0.5 for a full-screen rect, less for smaller ones |
| I33 | fixed | the 1 MB glow coverage table was allocated in `Initialize` although a uniformly lit ring - the default - and a disabled layer never read it; it is now allocated on the first frame that bakes it and released with the layer |
| I34 | fixed | the glow coverage table re-baked on every config change, so an intensity, colour or other-layer animation paid pass 0b every frame; it is now gated on its own inputs (light blocks, width, height, corner radius, winding, glow radius) |
| I39 | fixed | the emission table re-baked on every config change; it is now keyed on what its pass binds - two uniforms by value, three LUTs and two light blocks by upload count - so no config field list can drift from the shader |
| I40 | fixed | the gather and the reduced-scale shading re-ran on frames where nothing moved; skipped then (`mOffscreenCurrent`), byte-identical, 3.84x geometric mean on still frames at 0.5 on an M2 Pro, moving frames unchanged |
| I41 | fixed | without segments `uGatherSeg` was bound to texture 0, and Apple's driver logged it on every split or scaled run; attachment 0 stands in, as for I33 |
| I42 | fixed | any arc or segment change re-baked the whole glow coverage table; each piece's texels depend on that piece alone, so only the pieces a changed light reaches are re-baked - exact (texel for texel), 1.29x on the band's travelling-segment frames on an M2 Pro |
| I43 | fixed | the glow coverage table was RGBA16F with two channels of zeros on a config without segments, and was held for the life of the layer; now RG16F there (0.5 MB) and released after 5 s unread |
| I44 | changed | the split's gate was the AMD's calibration and kept the loop inline where an M2 Pro split 1.1-4.75x faster; now the M2's values (every rect splits at 1.0), `bounded_band` 3.5-3.7x, at up to 16 / 33 MB of gather buffer for a tiny rect with a frame-filling glow |
| I45 | fixed | a uniformly lit ring with opaque stops computed `perimeterPosition` and a gradient alpha read nothing used; skipped (`uPerimeterUnread`), the default frame 1.11x at 1080p, byte-identical where it engages |
| I46 | fixed | pass 1 at 1.0 re-shaded a glow whose only moving part is its hue; now a hue-invariant field baked once a config holds and composited per frame - 3.3x (hue) / 6.3x (still) geometric mean at 1080p, within 1/255, +3.3 MB for the default rect |
| I47 | fixed | the neon cleared its cached offscreen passes and its field on any composited config change, so an animation of another layer (the lens flare's sun) cost it 4.9x (hue) / 15x (still) at 1.0; now cleared only on a change to `config.neon` or `config.geometry`, byte-identical |
| I48 | changed | the bloom's and the corner development's atans were the driver's; now a minimax polynomial (1.7e-6 rad), 1.07-1.11x on animated frames at 1.0 and 0.5 on an AMD 5300M, within 1 level on at most 0.016% of channels; unmeasured on the M2 |
| I49 | changed | three byte-identical SDF functions and the tone map were hand-copied between shaders, "uniformly lit" was decided twice, and the inline-gather path no shipped config built was still compiled in; one copy of each, one CPU predicate, the inline path and its gate removed for a `GATHER_MAX_SCALE` of 0.5 (20 -> 8 MB, 3.3x, up to 9 levels on a 40 x 24 rect; no check image moves) |
| I50 | changed | below 1.0 a rotating hue re-shaded pass 1b every frame; the hue-invariant field now serves it too, baked on the reduced buffer's grid and composited in pass 1b's place - 2.00x / 1.56x / 1.23x on hue frames at 0.75 / 0.5 / 0.25 (AMD 5300M, 1080p), +0.84 MB at 0.5, within 1 level; the field's programs now built and first drawn on a config's first frame, which removes the bake frame's ~140 ms stall |
| I51 | changed | the field's mask channel (RG16F) served only scale 1.0; removed - at 1.0 a one-sided glow or a cutoff now shades directly (`glow_inside` / `card_outside` 4-8x slower there, toward exact by 1-2 levels), nothing below 1.0 moves |
| I52 | changed | below 1.0 the edge ring was re-shaded on every frame; it has a field of its own, packed into four strips at full resolution, composited in pass 2c's place - 1.4-1.9x still, 1.2-1.6x hue below 1.0 (AMD 5300M), ~0.27 MB, within 1 level |
| I53 | changed | the ring's field refused a one-sided glow and cutoffs; its composite now recomputes them from the pixel's position, as the ring's shading applies them after its tone map - masked still frames 1.2-2.7x below 1.0 (M2), the production band 0.051 -> 0.029 ms at 0.5, within 1 level |
| I54 | changed | an arc that moved re-baked every glow coverage piece either support touched - nearly the whole table under an arc wipe; now only the bands its ends swept (bitwise, plus the antipode rule for one fixed end), texel-exact over 6,706 random edits - arc-wipe frames 1.06x at 1.0 to 1.15x at 0.25 (M2), `segments` up to 1.74x |
| I55 | changed | the gather re-ran on every config change; now skipped while its inputs hold and this frame's quad lies inside the last one drawn, read through that region - intensity-pulse frames 1.19x at 1.0, 1.31x at 0.5, 1.43x at 0.25 (M2), within 1 level; and pass 1b, which inherited its unblended state from pass 1a's `glDisable`, now sets it itself |
| I56 | changed | an intensity animation invalidated both fields every frame; an intensity-only change now keeps them, scaled by I / I0 while it moves below the bake, re-baked above it and exactly when it stops - intensity-pulse frames 2.31x at 0.5, 1.60x at 0.25 (M2); held frames byte-identical, moving ones within 2 levels on the default look, 6 on a 1.5 bloom over a 0.4-1.0 pulse (owner-accepted) |
| I57 | changed | the direct path at 1.0 removed: 1.0 takes the scaled path with a full-size buffer, `uBlitOwnsCut` replaces the scale test, `resolutionScale` defaults to 0.5 - every frame below 1.0 byte-identical and no slower; 1.0 within 1 level, 1.67x on still frames, 0.88x hue and 0.62x intensity there (M2), up to 8.3 MB more at full screen |
| V23 | fixed | the bloom's pedestal ended it on a crease at `reach`, so a large rect's centre showed a hard-edged dark rectangle and the outer rim bunched its last levels; each piece's bloom now fades out by its own distance from 0.5 `reach`, both sides - at most 6 levels in the outer half of the tail, arc-wipe frames 0.97x, still frames unchanged. the page's images regenerated with R7 |
| V24 | fixed | the ring's field baked the ring mesh into transposed side strips, where the fill rule hands an edge on a pixel centre the other way, so ~1% of configs composited an unbaked texel - a 1 px dark seam between the ring and the blit; the bake now draws each strip's whole box, direct-vs-field pixels over 8 levels 303 -> 2 on 400 configs |
| I59 | changed | the glow coverage table was 1024 columns for every rect; its layout is now a parameter stored with the table and its width follows the rect and the halo width (256-1024, calibrated on 250 configs) - 0.25-0.69 MB for a default glow, 1024 still for a thin one on a large rect; within 2 levels, 99.998% within 1; 8 segments on the band 0.84x |
| I60 | changed | the segments' bells were integrated at the arcs' resolution; they have a narrow table of their own (pass 0s, 4 columns per sigma from 128) copied into the main one by a fill pass (0f), `neon.frag` unchanged - within 2 levels, 99.992% within 1; 8 segments on the band 0.87 -> 0.56 ms (0.64x), 960 x 540 0.83x |
| I61 | changed | each bell was integrated, and its moves tracked, out to 5 sigma; 4.24 now (`GLOW_COVER_BELL_REACH`), at most 1 level on up to 1.6% of lit pixels, segment-table bake 0.75 -> 0.58 timer units; the plan's 8-node rule was measured (3 levels) and not taken |
| I62 | changed, then reverted | pass 1b and its field bake computed the filament the edge ring always redraws; skipped there (`uBlitOwnsCut`), every final frame byte-identical, the intermediate-buffer figures without the line. Reverted 2026-10-19: the reduced buffer is the neon's shared buffer and has to hold the line - frames byte-identical, figures with the line again |
| V25 | fixed | the filament read one coverage at the fragment's nearest perimeter point, which jumps across a corner's diagonal, so an arc ending on a sharp corner (an arc wipe from 0) was cut along it on the inside half only; the filament is now a max over the outline's pieces, each at its own distance and its own nearest point - uniformly lit rings byte-identical, `check` unchanged |
| V26 | fixed | every edge ramp (the one-sided cut, the cutoffs' floor, the blit's, the ring field's, the opaque fill's coverage) was sized by `fwidth(d)`, which reads 2 on a sharp corner's vertex pixel when the quad straddles both edges - that pixel came out at 0.84 (the fill's 0.75) on whichever corners the quad parity hit; `sdRoundBoxFwidth` takes the analytic gradient instead - corners agree within the dither, two-sided scenes byte-identical |
| I63 | changed | the field bake (`neon.frag` + `NEON_FIELD_BAKE`) was sent `uGlowSideSoftness`, whose only reader follows the bake's return, so Tizen's compiler removed it and the upload logged "not found"; the bake is no longer sent it - frames byte-identical, the other neon programs audited for the same |
| V27 | fixed | below `resolutionScale` 1.0 the spotlight dithered into an RGBA8 buffer the blit then filtered, so the dark tail came back as blocky contours and texel-wide mottle; the buffer is half float and undithered, and the spotlight's own blit dithers per destination pixel - tail pixels off by over half a level 3,586 -> 49 and 6,472 -> 0 at 0.25, 1.0 byte-identical, scaled frames 1.04-1.08x (AMD 5300M) |
| V28 | fixed | the arc block's abutment flags depend on the perimeter but were repacked only when the arcs or segments changed, so a resized rect kept the old ones - up to 3 levels; the gate is gone: the blocks are packed every frame and `UniformBuffer::SetData` skips an unchanged upload - `sequence` 24 failing frames -> 0 |
| [V29](#v29-a-segment-move-leaves-a-one-level-stale-texel-that-only-an-arc-re-bake-clears---open) | open | a segment step on a sharp-cornered, arc-less rect left a 1-level stale texel that only re-baking the coverage table's ARC pieces clears - the per-type split (I42) is not exact there; narrowed by elimination (thirty-fourth pass) to the bake's colour write mask in direct mode - likely a driver variant per mask - with an experiment to settle it, untested |
| I76 | fixed | the glow field's composite repeats pass 1b's discards, so the shared buffer ends where pass 1b's does |
| V30 | fixed | the hue, the flare's rays and the droplets' field took their phase as `time * rate`, which races whenever the rate moves - the `HueRotationEaseReverse` preset moved the hue 0.12 / 0.44 turn a frame started 10 / 60 s into the clock against the 0.013 its rate allows; each is now a `PhaseAccumulator`, the rate integrated over the clock, bit-identical to the old phase while a rate holds (every check image, `clock` frame and `generate` PNG unchanged); a rate set to 0 now holds the phase where it stopped |
| V31 | fixed | the droplets' rebuild gate missed `glowSide`, which their band's extent reads, so a side change kept the old ring - 2,914 of 14,117 lit px for INSIDE -> OUTSIDE; now gated on the extent itself, all six transitions byte-identical to a fresh renderer |
| I77 | fixed | an animation callback that attached or detached changed the vector the manager (and `AnimationGroup`) was walking - heap-use-after-free or container-overflow under ASan in 8 of 9 cases, and a callback clearing itself destroyed its own `std::function`; both now walk an allocation-free snapshot and invoke a copy of the callback - all 9 clean under ASan and UBSan |
| I78 | fixed | a zero-duration animation's elapsed time was a float - 9.375 s per 10 s after a day, frozen past 6.07 days, the clock's old bug one layer up; elapsed, `ApplyAt` and `Modulator::Evaluate` are doubles, exact over 365 days |
| I79 | fixed | eleven C ABI entry points dereferenced the null effect a created, uninitialised handle holds (SIGSEGV); they return `EL_ERROR_INVALID_HANDLE` - or an empty answer for the two queries - before init |
| [V32](#v32-the-lens-flare-is-never-dithered---open) | open | the lens flare writes undithered, into RGBA8 below 1.0 - the mechanism R7 and V27 fixed elsewhere, on the smoothest layer; not measured |
| I80 | fixed | the C ABI refuses non-finite float arguments and non-positive preset durations |
| I81 | fixed | uniform lookups key on a string_view into names stored once; no allocation per call |
| [I82](#i82-the-output-dither-is-added-where-the-glow-is-0---open) | open | the output dither is added where the glow is an exact 0: noise over the host's pixels on a non-8-bit target, no blend skip, and the "changes no pixel" proofs of the interior hole and the corner skip predate it |
| [I83](#i83-what-the-library-leaves-changed-in-the-hosts-gl-state-is-not-documented---open) | open | the GL state a frame leaves changed (VAO and array buffer, program 0, texture units 0-7, uniform-buffer bindings 0-3, blending on) is undocumented, and unpack state and the read framebuffer are assumed without being listed |
| [I84](#i84-the-embedded-shader-text-is-85-comments---open) | open | 692 KB of embedded shader source is 85% comments (`neon-tuning.h`'s ~50 KB of prose in each of seven neon sources); a configure-time strip would cut library size and driver lexing, and clear MSVC's 16 KB literal limit |
| V33 | fixed | an HSV or HSL blend toward an achromatic stop took its hue as 0 and swept toward red; an achromatic endpoint (saturation 0, or HSV value 0) now takes the other stop's hue - blue -> white HSV mid (1.0, 0.5, 1.0) -> (0.5, 0.5, 1.0); RGB unchanged |
| V34 | fixed | the colour cross-fade restarted on every bake, so a stop edited each frame lagged (23% of a one-second drag shown at its end); a fade retargeted in flight now chases the new target exponentially at a time constant of a third of the duration - lag 0.77 -> 0.09; a change from rest keeps the smoothstep |
| I85 | fixed | `CaptureUtil::ReadRegion` tested `GL_SAMPLES`, the DRAW framebuffer's, so a single-sample capture read after `End` was refused under a multisampled window; the pre-check is gone (glReadPixels' own INVALID_OPERATION is named instead) and a host pack buffer is unbound for the read and restored |
| [I86](#i86-the-c-abi-logs-on-paths-a-host-calls-every-frame---open) | open | on Linux / Tizen `LOG_D` stays live in the C ABI, so 74 of 82 getters log on every call; `el_animation_update` / `apply` log at INFO on every call everywhere (120 lines per 60 frames); no level filter or platform sink |
| I87 | fixed | V30's fix left seven descriptions of `uHuePhase` as `fract(time * rate)`; all seven now describe the integrated phase (`PhaseAccumulator`) |
| [I88](#i88-the-spotlight-overwrites-its-strip-buffer-in-place-on-every-animated-frame---open) | open | the spotlight overwrites its strip VBO with `glBufferSubData` every animated frame, where the neon orphans with `glBufferData` to avoid an implicit sync; not measured |
| I89 | fixed | `el_animation_destroy` on an attached animation left it running with its C callback still holding the host's `userData`; destroy now clears both callbacks (use-after-free under ASan before, clean after); the orphaned animation is still removable only by `el_effect_detach_all_animations`, as the header now says |
| [I90](#i90-a-modulator-sequence-that-contains-itself-hangs-every-evaluation---open) | open | `Sequence::Append` (and `el_modulator_sequence_append`) accepts a stage that reaches the sequence; self and mutual cycles both return success and then `el_modulator_evaluate` never returns (killed after 5 s), and the cycle leaks |
| [I91](#i91-a-second-eleffectinit-replaces-the-effect-and-drops-its-animations-and-clock---open) | open | a second `el_effect_init*` builds a new effect and returns success, silently dropping the attached animations and the clock state (attached 1 -> 0, paused -> playing); undocumented |
| [I92](#i92-an-animation-attached-to-two-effects-advances-twice-a-frame---open) | open | an animation attached to two effects is advanced by both (60 frames at 1/60 on each read 2.000 s elapsed); undocumented and unguarded |
| V35 | fixed | the glow field's bake (pass 1f) made pass 1b's culls itself, through its own region's projection, so its `vPos` differed from pass 1b's in the last bits: on a texel right on a cull's boundary it culled where pass 1b kept, and pass 1c - which repeats the culls from pass 1b's own `vPos` (I76) - composited its 0, a shared-buffer texel 200 levels under pass 1b's guard band on a rounded corner's outside cutoff, the frame within 1 (`sequence` seed 1, sequence 13, frames 69-71); the bake now culls nothing and the composites make every cull on the frame's own grid - `sequence` 3 failing frames -> 0 (on 100 x 150, `normal`'s only failures in 15,000 frames -> none), the `cover` set and the 210 page images byte-identical |

One item that is deliberately NOT on this list, so nobody adds it: `Texture`'s
virtual destructor, measured in I9. It costs every LUT a vptr for a dispatch
nothing uses, but it lives in a shared GL wrapper with a blast radius well
outside the LUTs. It is recorded at the point where someone would trip over it
rather than tracked as a defect here.

If any of these comes back, the fastest way to reproduce a VISUAL item is the
harness described at the top of this document - the configs for every case are
given inline with each finding. The implementation items in the third pass need
a different harness, described in
How the implementation items were verified:
it drives the wrapper classes directly and reads GL state back, because none of
those items changes a pixel.
