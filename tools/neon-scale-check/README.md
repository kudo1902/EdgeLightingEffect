# neon-scale-check

The harness behind
[`docs/pages/neon-resolution-scale-comparison.html`](../../docs/pages/neon-resolution-scale-comparison.html),
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
[`docs/design/neon-resolution-scale-plan.md`](../../docs/design/neon-resolution-scale-plan.md),
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

The tool uses only public headers (`core/`, `util/capture-util.h`, and for
`partition` `gl/shader-program.h` plus the generated `shaders.h`), taken from
`EL_ROOT` and `EL_ROOT/build` so they match the library.

## `check`

```bash
./build/tools/neon-scale-check/neon-scale-check check
```

Every image the tool measures is the STEADY frame - the second after the
config is set, each over a fresh clear - because at scale 1.0 the neon draws
the first frame after a config change directly and its hue-invariant field
takes over from the next (I46 in `docs/records/review-findings.md`).

Prints one row per scene and exits 1 if anything is out of bounds (marked `!`):

| what | bound | measured (Apple M2 Pro, the page's machine since 2026-10-06) |
| ---- | ----- | -------------------------------- |
| scale 1.0 against the committed `docs/images/neon-resolution-scale/<scene>_s1000.png` | max 2 | 0 |
| each reduced scale against its own 1.0 render, max error | 3 | 1-2 |
| `small_rect` at 0.25 / 0.125 (20 x 12 buffer texels at 0.125) | 5 / 12 | 3 / 10 |
| the moving hairline's worst centroid error, every scale | 0.1 px | 0.02-0.07 |

The committed images were regenerated on the Apple M2 Pro on 2026-10-06, from
the tree with the hue-invariant field at 1.0 (I46), so the 1.0 column reads 0
there; until then they were an AMD Radeon Pro 5300M's, and the M2 read 2 on
`hairline` and `card_outside` - no headroom left for a 1/255 change at 1.0. A
GPU other than the M2 should expect 1-2 on that column. The hairline's
0.069 px (at 0.35) is the split gather's: the ring reads its colour and
coverage from a grid about 8 px apart rather than 2-4 px apart; re-measure on
the GPU before tightening that bound.

Each bound is the measured value plus one level for GPU-to-GPU variance (the
first version of the page was rendered on an Apple M2 Pro, and its 1.0 images
match this machine's within 1-2 levels). It also passes on Mesa's llvmpipe
software rasteriser, which is how it can run on a Linux box with no GPU: the 1.0
column drifts at most 2 there, and every reduced scale stays inside its bound. Built against the pre-plan library
(`542dad4`) the check fails on 54 values, with the hairline wandering
+/-0.53 px at 0.25 - which is what it is for.

A failure on the 1.0 column means the scale-1.0 output changed. If that
was intended, regenerate the page (below) so the committed images describe it.
A failure on a reduced scale means the scaled path lost quality the edge ring
had; `--images-dir DIR` points the 1.0 comparison elsewhere.

## `partition`

```bash
./build/tools/neon-scale-check/neon-scale-check partition [--configs N] [--seed S]
```

Below scale 1.0 the frame is composited by two passes over areas that must
tile: the blit (pass 2b) draws everything that can be lit outside a ring
around the outline, and the edge ring (pass 2c) draws the ring. Both blend
premultiplied-over, so a pixel drawn by both composites twice (a bright seam)
and a pixel drawn by neither shows the background (a dark crack). `check` sees
that only on its twelve scenes; this tests it across N random configs, 1000 by
default.

Each config draws at a random frame size (an odd one included), with a random
rect size, fractional position (partly off screen too), corner radius and
winding, line, glow reach, glow side, both cutoffs and resolution scale. The
command watches the frame through `PassRecorder`
([`tools/common/pass-recorder.h`](../common/pass-recorder.h)), replays the
blit's and the ring's own triangles - their vertex arrays and recorded `uMVP`,
through the library's `neon.vert` - additively into a coverage target, and
fails on:

- any pixel covered more than once;
- any uncovered pixel with the ring within two pixels on one side and the blit
  on the other. An uncovered strip with the ring on both sides is legitimate:
  the ring's hole, left undrawn where nothing past an inside cutoff can be lit.

It prints the first five failing configs in full and exits 1. A seed names the
same configs on every platform (the random floats come from `mt19937`'s raw
bits, not `std::uniform_real_distribution`), so a failure reproduces anywhere
with the same `--seed` and `--configs`. It also fails when the ring was never
drawn, since then nothing was tested: a library without the edge ring, or a
shader uniform renamed under `PassRecorder`, which names passes by them.

Measured on the Apple M2 Pro: seeds 1, 2 and 3 pass, 3000 configs and about a
billion covered pixels with no overlap and no gap, about 9 s per 1000 configs.
Against a scratch build that moved the ring's edges by 1 px, it failed 156 of
200 configs both ways (the rest put the moved edge off screen). A ring that
still tiles but is too narrow for the line passes here - that is a quality
failure, and `check` catches it.

What it rests on: the two vertex arrays are emitted from the same floats
(`setupRingGeometry`), drawn through one full-res transform `Render` builds for
both, by programs that share `neon.vert`'s `invariant gl_Position`. Every edge
is axis-aligned and the transform has no rotation, so collinear edges agree
exactly even where they meet at T-junctions. Run this after touching any of
those, and on every GPU the scaled path ships to: the last of them is a
property of the rasteriser, which only running on it can confirm.

## `shared-buffer`

```bash
./build/tools/neon-scale-check/neon-scale-check shared-buffer [--images DIR]
```

The neon's shared buffer (`SharedBufferId::NEON`, a `SharedBuffer`):
`mScaled`, ready on every frame that draws the glow, with the filament in it
although the blit never shows it. The command reads it the way an internal
renderer does - probe renderers, one registered after the neon and one moved
before it, each taking the buffer with `SharedBufferRegistry::Find` in its `Render` -
on every scene at every scale, still and with the hue rotating (the clock moved
across each config change, so the field's composite fills the buffer: 66 of
the 72 hue cells; `bounded_band` is too thin a band for the field). It fails
when:

- the probe is handed no ready buffer, or one that is not the whole frame's
  viewport at the scale (`floor(viewport * scale)` texels);
- the probe before the neon sees another image than the one after it - the
  effect runs every layer's offscreen phase before any composites;
- a still frame drawn with nothing changed moves the buffer's version;
- on a scene with no cut, cutoff or fill, the buffer blitted back through its
  viewport carries under 0.9 of the frame's light ON THE LINE (luma above the clear,
  over the pixels within the filament's half-width of the edge);
- the hue variant never reaches the field's composite, or the buffer is ready
  while the neon is off, or not ready once it is on again;
- after a rect that moves and shrinks at one scale, the buffer differs by a
  single texel from the moved rect's drawn on a fresh effect. Both on effects
  whose hue rate never moved: the hue's phase is the rate integrated over the
  clock (`PhaseAccumulator`), so an effect that ran the hue variant holds its
  hue where the rotation stopped, and differs from a fresh one everywhere.

The last is the windowed clear (`SharedBuffer::BeginWrite`: this write's
window and the last write's). The scenes never reach it - every cell changes
the scale, which reallocates the buffer and clears it whole - and no frame
shows a stale texel, since the blit reads only where the write drew, so
neither `check` nor the `cover` set can catch one. A scratch build that cleared
only this write's window left 2790-218880 stale texels (scales 0.125-1.0, up
to 47/255) and passed everything else; this check fails it at every scale.

The light is measured on the line because the halo and the bloom carry most of
a frame's: a scratch build that never shaded the filament into the buffer
still came back at 0.93-0.995 of the whole frame, and at 0.58-0.89 on the
line: every unmasked scene failed but `overdrive`, whose line the bloom drives
to white either way. With the filament, 0.94-1.24 on the line (1.24 is the
hairline at a low scale, which the sampling floor widens). Measured on the AMD
Radeon Pro 5300M, 2026-10-19: `PASS`, about 18 s. `--images` writes each buffer
over black, top row first, as `DIR/<scene>_<variant>_<scale>.png`.

It does not compare frames. That the filament in the buffer moves no pixel of
the frame rests on the edge ring covering the filament's reach (the blit never
reads a texel it lights), and is checked the way any change that may move
pixels is: `generate --set cover` and `generate --images` from a build before
and after, then `diff A B --max 0` and a byte compare of the page images. Run
it, and that, after touching `filamentLit` in `neon.frag`, `SharedBuffer`, the
shared buffer's `MarkReady` / `MarkNotReady` in `NeonRenderer::RenderOffscreen`,
the glow window (`GetSubWindow`), or the edge ring's width (`GetRingWidth`).

## `clock`

```bash
./build/tools/neon-scale-check/neon-scale-check clock [--frames K] [--dump DIR]
```

The effect's clock at a start time an edge device reaches by running for days:
870400 s (~10.07 days), past 2^19 s, where the float clock the library used to
keep stepped by 1/60 s did not move at all. It fails when:

- `Clock`, set to that time, does not advance by the sum of 600 frame deltas
  of 1/60 s to within a microsecond;
- `GetCyclePhase` at that time does not step by `rate / 60` of a cycle every
  frame, to within 1e-6;
- K frames (default 30) of every layer whose shader reads a phase of time -
  the neon with its hue rotating, the droplets, the lens flare with its rays
  spinning and the debug layer's gradient strip - drawn by an effect whose
  clock starts there differ by more than one level from the same frames drawn
  by an effect starting at 0. The start time is a whole number of cycles of
  every phase involved (hue 0.5 rev/s, rays 0.25 rev/s, the droplets'
  `DROPLET_TIME_PERIOD` at speed 1), so the two runs are owed the same frames;
- the last of those frames equals the first: the effect stood still.

It also prints how far a float accumulator moves over the same 600 frames (0 s
there). On an Apple M2 Pro the late run matches the early one byte for byte
and its first and last frames differ by up to 154 levels; the library before the
double clock fails all three (clock 0 s, frames up to 174 levels off, first
and last frame within 1 level). It builds against an older checkout's headers
too (the phase step is skipped where there is no `GetCyclePhase`), which is
how that was measured. `--dump DIR` writes the run from 0 as
`DIR/clock_NNN.png`, for `diff` against another build's - what a change does to
frames at an ordinary uptime.

## `sequence`

```bash
./build/tools/neon-scale-check/neon-scale-check sequence [--sequences N] [--frames F] [--seed S] [--only K] [--isolate] [--dump DIR]
```

A differential check of the neon's skips and reuses. `NeonRenderer` keeps
about a dozen keyed caches - the emission table (`isEmissionTableStale`), the
glow coverage table's per-piece dirty masks, the offscreen reuse
(`mOffscreen.reusable`), the gather's held region (`GatherInputs`,
`AnnulusCovers`), the two fields' `LazyBake` schedules, the windowed clear of
the shared buffer, and the gates in `OnConfigChanged` that decide what to
rebuild - and each is correct only while its key holds every input its pass
reads. `check`, `partition` and `shared-buffer` draw steady frames of fixed
scenes, where no key is ever tested; this mode drives the renderer through
what a host's frame loop does to it.

It runs N seeded random config sequences (16 by default) of F frames (90),
each through effects that differ only in `NeonRenderer::SetBypass`
(`NeonBypass` in `renderer/neon-renderer.h` - a verification hook, not a
config field, with no C ABI mirror; 0, the default, is the renderer as
shipped):

| run | bypassed | bound against `forced` |
| --- | -------- | ---------------------- |
| `forced` | everything (`NeonBypass::ALL`): every pass every frame, every pass 0b piece, every `OnConfigChanged` gate open, the shared buffer cleared whole, no field | - (the reference) |
| `exact` | the reuses documented approximate (`APPROXIMATE`: the gather's held region, both fields) | byte-identical: every frame and every shared-buffer texel |
| `normal` | nothing | 2 levels on any channel of the frame or the shared buffer (I55's held region within 1, a field's composite within 1); 6 on a frame whose intensity moved (I56's gain, up to 6 at bloom 1.5, the generator's ceiling) |

A sequence starts from a random page scene, viewport, scale, rect, arcs and
segments, and each frame either holds (stretches of 1-6 unchanged frames,
where the skips take over) or steps whatever animations are running - an
arc's length or start, a segment's position and length, intensity, bloom,
glow radius, the rect moving (whole and fractional px) or resizing - and
applies a few random changes: hue on and off, scale, viewport (an odd size
included), layer and `opaqueOnly` toggles, glow side, cutoffs, fill mode and
its cutoffs, colour stops (some below full alpha), transition time, blend
space, arcs and segments added, removed or edited, preserved segments, line
width, filament falloff, corner radius, winding, sample count, LUT size, the
rect jumping to a frame edge. One field per change wherever it can be: a gate
that misses one field is hidden on any frame another field it does read moved
with it. Each frame is drawn by every effect at the same clock (`Update(1/60)`)
over transparent black, and compared with its alpha; each effect's shared
buffer is read by a probe renderer after the neon and compared as floats.

Each failing frame prints the run, the sequence and frame, the worst level and
the pixel count, the frame's events and its config; the exit code is 1 if
`exact` or `normal` failed. Sequence K is seeded `S + K`, so `--only K`
replays it alone. `--isolate` adds `normal` and `exact` again, each with ONE
more skip bypassed, and every failing frame lists which of those pass it: the
skips whose bypass clears the failure. ("Only that skip active" does not work:
the skips depend on each other - the gather's key holds the emission table's
version, the offscreen reuse needs that table current - so a skip alone among
bypassed ones is never reached.) `--dump DIR` writes each failing frame and
`forced`'s, and both shared buffers, as PNG.

Tested against a scratch build with two planted key omissions - `lineWidth`
taken out of `geometryDirty` (the "outer glow dropped at glowRadius 0" bug)
and the rect's centre out of `GatherInputs` - on the default 16 x 90: `exact`
failed 26 frames, every one cleared by `rebuild`, and `normal` 14 levels off,
cleared by `gather` (and by `emission`, whose bypass moves the gather's key
every frame). On this tree it found V28 and I76 in
[`docs/records/review-findings.md`](../../docs/records/review-findings.md) on the default run,
and with V28's one-line fix applied, V29 in a 100 x 150 run (`exact` failed 1
frame of 15,000; `normal` only on I76). Apple M2 Pro: the default run takes
about 14 s; 100 x 150 with `--isolate` (16 effects per frame) took most of an
hour.

Run it after adding an input to any offscreen pass, after touching a skip key
or a gate in `OnConfigChanged`, or a `LazyBake` schedule. A new skip belongs in
`NeonBypass` - its bypass in the renderer, its name in `sequence.cpp`'s
`BypassName` - or this mode cannot see it. Not covered: the LUTs' own
self-guards (`GradientRingLUT::Bake` and the atlases decide for themselves),
and the hysteresis choices (the coverage table's width, the segment table's
direct mode), which depend on history by design and evolve identically in
every run, since all of them see the same sequence.

## `flare-reuse`

```bash
./build/tools/neon-scale-check/neon-scale-check flare-reuse [--no-time] [--dump DIR]
```

A differential check of the lens flare's one reuse: below scale 1.0,
`LensFlareRenderer::RenderOffscreen` skips the draw into its scaled buffer when
nothing the pass reads has moved (`FlareInputs`, `mScaledCache`) and `Render`
blits what the buffer holds. Two effects - the neon and the flare - draw one
scripted 75-frame sequence, one with the reuse and one with it bypassed
(`LensFlareRenderer::SetBypass(true)`): still frames, the sun moving, the rays
spinning, the clock paused while they spin, then the rate back to 0, intensity
and ghost colour changes, the rect moving (the sun rides its perimeter), a
viewport change and back, the layer off and on, the scale through 0.25, 1.0
and back, and the sun jumping away and back. Every frame, read back with its
alpha, must match byte for byte; it exits non-zero otherwise (`--dump DIR`
writes both frames of a mismatch). Then, unless `--no-time`, it times still
frames of the flare alone at scale 0.5 - `Update(1/60)` + `Render` + `glFinish`
per frame, min of 5 runs of the mean of 40 - both ways at 1920x1080 and
3840x2160. Run it after adding anything `drawFlare` uploads or binds: an input
that is not a function of `Config::lensFlare` belongs in `FlareInputs`, and the
script should move it. Dropping the sun's position from the key fails the four
rect-move frames and the three still frames after them, which is the check
that the script reaches the key.

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
of those 72 effects took to construct and initialise (`initMs`) and its first
frame took, `glFinish` to `glFinish` (`firstFrameMs`) - the shader compiles,
which both drivers this project measures on finish on a program's first draw. At
a size other than 1280 x 720 the scenes' layout scales with the frame (rect
position, size, corner radius) while the neon's own px parameters do not, as a
host's would not on a bigger display.

`--mode MODE` picks what changes between the timed frames:

| mode | each frame |
| ---- | ---------- |
| `still` (default) | nothing: `Render()` after `Render()` |
| `hue` | `hueRotationRate` 0.5 (the library's default) and `Update(1/60)` |
| `intensity` | `intensity` pulsing by +/-10% through `SetConfig`, then `Update(1/60)` |
| `arc-wipe` | `arcs[0].length` sweeping 0.3-0.9, the same way |
| `segment-travel` | `segmentBoosts[0]` moving 0.003 of the perimeter a frame (one is added where a scene has none), the same way |
| `resize` | the rect's width and height swinging +/-15% out of phase, through `SetConfig` - every frame re-bakes (and may reallocate) what the rect's size sets |
| `lights` | `--arcs N` arcs and `--segments M` segments (defaults 8 / 0) IN PLACE OF the scene's own lights, every one changing length on its own phase, and the hue rotating at 0.5 - the frames of a host animating several lights at once. `--arcs 0` is one arc over the whole ring, still |

`still` is what every figure before the option measured, and it cannot see any
work the neon does only on a frame where something moved - re-baking the
emission table or the glow coverage table, or anything a renderer skips on an
unchanged frame. The animated modes put that back, as a host's frame loop
does, with `SetConfig` and `Update` inside the timed region, so their figures
include the library's CPU path. A change that saves work on unchanged frames
should be measured with `still` AND the animated modes, to show both what it
saves and that it costs the moving frames nothing. The JSON records the mode.

`--set cover` times the `cover` scenes (below) instead of the page's, each at
its own frame size, as its frame 0 configures it; `--scene a,b` and
`--scales 1,0.5` narrow either set. Every figure is also printed, one line per
scene and scale.

**The CPU share of a moving frame is overstated.** `time` never swaps, so the
driver submits mid-frame whenever a pass renders into a texture an earlier,
unsubmitted frame read: once per hue frame, about five times per
segment-travel frame. The frame totals are still right - those frames are
GPU-bound either way (0.26 ms per intensity frame with and without a
`glFlush` per frame, Apple M2 Pro) - but a host that swaps submits once per
frame, and its `Render` measured 0.015-0.018 ms of CPU on a still frame and
0.04-0.07 ms on a moving one.

`--gpu` adds a `GL_TIME_ELAPSED` query round each `Render` (median of 120
frames, each flushed as a swap would), and `--passes` each pass's share, from
a query round every draw (`PassRecorder`, named as the guide names the
passes). **Read both as ratios.** On an AMD Radeon Pro 5300M under macOS GL the
queries read 3.6-4.0x the wall-clock time of the same frames - 120 frames the
timer put at 4.0 ms each finished in 125 ms, `glFinish` to `glFinish` -
steadily across configs, so ratios between configs and passes hold while the
milliseconds do not. The default figure, wall-clock between `glFinish` calls,
is the frame's real cost. (The per-pass queries also serialise the draws, so
the passes add up to more than the frame.)

For a before / after comparison, build the tool once per library (standalone
mode, above), run `time` for every build in rounds with the build order
rotated each round, and take the MEDIAN per figure over the rounds the builds
ran in together. Not the minimum: it was measurably biased toward whichever
build ran first on a cooler GPU (0.770 ms against 0.806 and 0.802 for one
figure). That is how
`neon-resolution-scale-perf-comparison.md`
was made. (The comparison page's own timings are still merged by minimum - see
step 3 below - because they come from interleaved runs of only two builds; read
them as the page's figures, not as a before / after measurement.)

## `generate --set cover` and `diff`

```bash
./build/tools/neon-scale-check/neon-scale-check generate OUTDIR --set cover [--scene a,b]
./build/tools/neon-scale-check/neon-scale-check diff BEFORE AFTER [--max 2] [--within1 99.9]
```

The `cover` set ([`src/cover.h`](src/cover.h)) is the measure for a change to
the glow coverage table
([`docs/design/neon-glow-cover-resolution-plan.md`](../../docs/design/neon-glow-cover-resolution-plan.md)).
Its scenes MOVE: arcs and segments change length every frame, some segments
travel, so every frame is the first after a config change - shaded directly,
after a re-bake of whatever pieces of the table it reached - which is the path
a table change shows on, and one the page's steady frames never take. Each
scene has its own frame size (1920 x 1080, and one 3840 x 2160 frame for a
thin glow on a TV-sized rect). `generate` writes six frames per scene at 1.0,
0.5 and 0.25, each an RGBA PNG over transparent black so the layer's coverage
alpha is kept, plus `gpu.txt`; about two minutes and 440 MB for the whole set.

`diff` compares every PNG the two directories share: per file the largest RGB
and alpha difference, p99.9, and the share of lit pixels (any RGB channel at
1 or more in either image) off by 1, 2, 3-4, 5-8 and more levels. It exits 1
when a file exceeds `--max` or has fewer than `--within1` percent of its lit
pixels within 1 level - the plan's criterion for a step that may move pixels -
and warns when the two were rendered on different GPUs (`gpu.txt`), since a
switch alone moves pixels by a level. A byte-identical change reads 0 pixels
differing.

## Regenerating the comparison page

1. Build the tool in-tree, and once more standalone against the build the page
   compares with (for the edge ring, `542dad4`, the last commit before the
   plan): `git archive 542dad4 | tar -x -C /tmp/el-before`, build that tree in
   `/tmp/el-before/build`, then configure the tool with
   `-DEL_ROOT=/tmp/el-before`.
2. Run `generate --mode hue` three times per build, INTERLEAVED (after,
   before, before, after, after, before), each into its own directory; one
   `--images --no-timing` run of the in-tree build. Hue mode because a still
   frame now reuses its passes at every scale - and at 1.0 the hue-invariant
   field - so still timings no longer show what the scale costs.
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
