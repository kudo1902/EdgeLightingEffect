# neon-scale-check

The harness behind
[`docs/neon-resolution-scale-comparison.html`](../../docs/neon-resolution-scale-comparison.html),
and the regression check for `NeonConfig::resolutionScale`. It renders the
neon layer offscreen for twelve fixed scenes at six scales, measures every
reduced scale against the same scene's 1.0 render, and either reports pass /
fail (`check`) or writes the data and images the comparison page shows
(`generate`).

It is a tool, not part of the library or the demos, and it is off in the
default build. It exists because the repo has no test target, and the edge
ring's guarantees - scale 1.0 unchanged, every scale within a couple of levels
of it, a moving line that stays on its edge - had no other check. Twice it
lived only in a session scratch directory and was lost, and the second rebuild
had to recover the scenes from the old page's pixels; see
[`docs/neon-resolution-scale-plan.md`](../../docs/neon-resolution-scale-plan.md),
decision 4.

## Build

In-tree, against the library this checkout builds:

```bash
cmake -S . -B build -G Ninja -DEDGE_LIGHTING_BUILD_TOOLS=ON
cmake --build build --target neon-scale-check
```

Standalone, against another checkout's library - how to measure a commit from
before this tool existed. That checkout must already be built in its own
`build/`:

```bash
cmake -S tools/neon-scale-check -B build/neon-scale-check-other -G Ninja -DEL_ROOT=/path/to/other/checkout
cmake --build build/neon-scale-check-other
```

The tool uses only public headers (`core/`, `util/capture-util.h`), taken from
`EL_ROOT` so they match the library; they have not changed since `542dad4`.

## `check`

```bash
./build/tools/neon-scale-check/neon-scale-check check
```

Prints one row per scene and exits 1 if anything is out of bounds (marked `!`):

| what | bound | measured (AMD Radeon Pro 5300M) |
| ---- | ----- | -------------------------------- |
| scale 1.0 against the committed `docs/images/neon-resolution-scale/<scene>_s1000.png` | max 2 | 0 |
| each reduced scale against its own 1.0 render, max error | 3 | 1-2 |
| `small_rect` at 0.25 / 0.125 (20 x 12 buffer texels at 0.125) | 5 / 12 | 4 / 11 |
| the moving hairline's worst centroid error, every scale | 0.1 px | 0.02-0.05 |

Each bound is the measured value plus one level for GPU-to-GPU variance (the
first version of the page was rendered on an Apple M2 Pro, and its 1.0 images
match this machine's within 1-2 levels). It also passes on Mesa's llvmpipe
software rasteriser, which is how it can run on a Linux box with no GPU: the 1.0
column drifts at most 2 there, and every reduced scale stays inside its bound. Built against the pre-plan library
(`542dad4`) the check fails on 54 values, with the hairline wandering
+/-0.53 px at 0.25 - which is what it is for.

A failure on the 1.0 column means the direct path's output changed. If that
was intended, regenerate the page (below) so the committed images describe it.
A failure on a reduced scale means the scaled path lost quality the edge ring
had; `--images-dir DIR` points the 1.0 comparison elsewhere.

## `generate`

```bash
./build/tools/neon-scale-check/neon-scale-check generate OUTDIR --label head --images
```

Writes `OUTDIR/<label>.json` - per scene and scale the page's metrics, the
cross-section column and the GPU time, plus the motion sweep - and with
`--images` every PNG the page shows into `OUTDIR/images/`. `--no-timing` skips
the timing, which is the slow part. Renders are deterministic: two runs of one
build agree on every metric.

## `time`

```bash
./build/tools/neon-scale-check/neon-scale-check time OUT.json --label head --size 1920x1080
```

Timing only: every scene at every scale, each on a freshly initialised effect
(minimum over 5 runs of 40 frames between `glFinish` calls), plus how long each
of those 72 effects took to construct and initialise - the shader compiles. At
a size other than 1280 x 720 the scenes' layout scales with the frame (rect
position, size, corner radius) while the neon's own px parameters do not, as a
host's would not on a bigger display.

For a before / after comparison, build the tool once per library (standalone
mode, above), run `time` for every build in rounds with the build order
rotated each round, and take the MEDIAN per figure over the rounds the builds
ran in together. Not the minimum: it was measurably biased toward whichever
build ran first on a cooler GPU (0.770 ms against 0.806 and 0.802 for one
figure). That is how
[`docs/neon-resolution-scale-perf-comparison.md`](../../docs/neon-resolution-scale-perf-comparison.md)
was made. (The comparison page's own timings are still merged by minimum - see
step 3 below - because they come from interleaved runs of only two builds; read
them as the page's figures, not as a before / after measurement.)

## Regenerating the comparison page

1. Build the tool in-tree, and once more standalone against the build the page
   compares with (for the edge ring, `542dad4`, the last commit before the
   plan): `git archive 542dad4 | tar -x -C /tmp/el-before`, build that tree in
   `/tmp/el-before/build`, then configure the tool with
   `-DEL_ROOT=/tmp/el-before`.
2. Run `generate` three times per build, INTERLEAVED (after, before, before,
   after, after, before), each into its own directory; one `--images` run of
   the in-tree build.
3. `python3 tools/neon-scale-check/update-page.py --after a1/head.json a2/head.json a3/head.json --before b1/before.json b2/before.json b3/before.json`
   It merges the runs (metrics must agree; timing takes the minimum) and
   rewrites only the page's `const DATA = ...;` line.
4. Copy `OUTDIR/images/*.png` over `docs/images/neon-resolution-scale/`.
5. Re-read the page's NOTES, findings and prose. They quote numbers by hand,
   and `update-page.py` does not touch them.

Timings belong to the GPU they ran on (the page names it); read ratios, not
milliseconds.

## Scenes and metrics

The scenes are in [`src/scenes.h`](src/scenes.h), with the crop and
cross-section column the page uses for each. They are the page's scenes
exactly - recovered by matching its stored images - so changing one changes
what every stored number means.

The metrics are in [`src/harness.cpp`](src/harness.cpp) and match the page's
method section:

- per-pixel error: the largest of the three channel differences;
- lit: a pixel whose brightest channel is above 24 (16 over the clear, rgb(5, 5, 8)) in either image;
- p99: the 99th percentile of the error over lit pixels (index `floor(0.99 * (n - 1))`); mean and "over 8" also over lit pixels;
- PSNR: over every channel of the whole frame;
- light vs 1.0: total Rec. 709 luma above the clear's, as a ratio;
- cross-section: rounded Rec. 709 luma down one column, 73 rows;
- motion sweep: the rect moves down 1/8 px for 64 steps; on the top edge's
  centre column, every row within 12 px of the true edge, minus that window's
  lowest luma, gives a centroid (minus the true edge) and a width at half
  height;
- heatmap: errors 0-64 onto a fixed ramp, 0 and 1 black.

These reproduce the first version of the page exactly when run on its stored
images, except the motion sweep, whose floor is new (its hairline wobble at
0.25, +/-0.53 px, still reproduces).
