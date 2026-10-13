# Neon perf: deep review, 2026-10-13

Measured on an **Apple M2 Pro**, Release builds, the neon layer alone, 1920 x
1080, at `ba3b5d5` plus the working tree (PR #65's comment updates only). The
question: where the neon stands after everything that landed since the last
M2 measurements (`6042866`, 2026-10-07) - I58, R7, V23-V26, I59-I63 and PR #65,
most of it measured only on the AMD Radeon Pro 5300M or not at all - and what
is left to take.

**Status, later the same day:** both defects found here are fixed in the
working tree - I64 as prototype F (section 4.2), V27 as proposed (section 5);
section 4.3 has the measurement of what was built. Everything else below is
still open.

## 1. Summary

- **Still, hue and intensity frames have not moved** since `6042866`: within
  the harness noise on wall-clock, and flat on GPU timers (still 0.034 ms,
  hue 0.080 ms at 0.5). Most frames a host draws are these.
- **Animated frames regressed 5-17% (wall) since `fb6f06f`, +29-36% of GPU
  time at scale 0.25.** Arc-wipe, segment-travel and many-light frames - every
  frame that shades the edge ring directly. The cause is V25's per-piece
  filament walk (`4b8e699`), which on this compiler pays the full loop body for
  all eight pieces. V25's log entry measured "no measurable cost" on the AMD;
  nothing was timed on the M2. PR #65 costs nothing measurable. (I64)
- **An exact fix, now built**: a candidate prefilter so the
  walk runs once per piece that CAN be in reach (one on a straight, at most one
  corner) - byte-identical on 306 animated `cover` frames and 210 page images,
  and it takes back ~75% of the regression (arc wipe 1.08x at 0.5, 1.13x at
  0.25; eight arcs changing length 1.15x at 0.25). Section 4.
- **PR #65 introduced a stale-render bug**: the arc block's abutment amounts
  now depend on the perimeter, but the block is repacked only when the arcs or
  segments change. Resize the rect while two arcs sit within a feather width of
  each other and the frame is up to **137 levels** off a fresh render of the
  same config. One-term fix, now built. (V27, section 5)
- **CPU per frame is the driver's render-pass setup, not library code**: 36-46
  us still, 85-107 us hue, 135-157 us with eight lights moving (M2 P-core); 70-93%
  of every pass is `glDrawArrays` opening a Metal render pass, the library's
  uniform lookups 3-15%. Section 6.
- **Mid-session hitches**: the first partial arc costs a 10-11 ms frame, the
  first segment 6-8 ms, the first opaque fill 2.5-3.7 ms, the first frame ~45
  ms - each a lazily built program. On a TV-class driver expect several times
  that. Section 7.

## 2. Method

- **Builds.** Four Release trees in the session scratchpad, each with its own
  `neon-scale-check`: `6042866` (last M2 baseline), `fb6f06f` (after I59-I62,
  before V25/V26), `a38797a` (after V25, V26, I63; before PR #65) and HEAD.
- **Frame time.** `neon-scale-check time --size 1920x1080` in `still`, `hue`,
  `intensity`, `arc-wipe` and `segment-travel`, all twelve page scenes, six
  scales, three rounds with the build order rotated each round, median per
  figure, then the geometric mean of the per-scene ratios. GPU totals from
  `--gpu` (`GL_TIME_ELAPSED` round each frame, flushed); per-pass from
  `--passes` - read those as a ranking only, Apple's per-draw timers fold
  passes that share a render pass.
- **Attribution.** Variants of HEAD's `neon.frag` built in a scratch copy
  (section 4), each timed against HEAD in three rotated rounds.
- **Exactness.** `generate --set cover` (306 animated frames, every one the
  first after a config change) and `generate --images --no-timing` (210 page
  images) for HEAD and the variant, compared with `diff` and `cmp`.
- **CPU, hitches, the stale-flag repro.** Headless probes linked against each
  build's static library (`SetConfig` / `Update` / `Render` timed separately
  with `glFinish` outside the timed region; `sample` on a loop without
  per-frame `glFinish`).

## 3. Frame cost by build

Wall-clock ms at HEAD, mean over the twelve scenes, and HEAD's cost against
each older build (> 1 means HEAD is slower):

| mode | build | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ---- | ----- | --: | ---: | --: | ---: | ---: | ----: |
| arc-wipe | HEAD ms | 0.920 | 0.631 | 0.423 | 0.342 | 0.313 | 0.299 |
| | vs `fb6f06f` | 1.05x | 1.08x | 1.12x | 1.14x | 1.17x | 1.17x |
| | vs `a38797a` | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| segment-travel | HEAD ms | 1.009 | 0.714 | 0.505 | 0.424 | 0.394 | 0.377 |
| | vs `fb6f06f` | 1.05x | 1.07x | 1.10x | 1.12x | 1.14x | 1.13x |
| | vs `a38797a` | 1.00x | 1.00x | 1.01x | 1.01x | 1.01x | 1.01x |
| hue | HEAD ms | 0.251 | 0.191 | 0.166 | 0.160 | 0.160 | 0.160 |
| | vs `fb6f06f` | 0.99x | 0.99x | 1.00x | 0.99x | 1.00x | 1.01x |
| intensity | HEAD ms | 0.181 | 0.124 | 0.102 | 0.094 | 0.102 | 0.106 |
| | vs `fb6f06f` | 0.97x | 0.94x | 0.94x | 0.91x | 0.99x | 0.97x |

`still` is below the wall-clock harness's resolution (0.05-0.08 ms, +-25%);
on GPU timers, mean over scenes at 0.5 / 0.25: HEAD 0.034 / 0.036 ms,
`fb6f06f` 0.036 / 0.032. Hue on GPU timers: HEAD 0.080 / 0.072, `fb6f06f`
0.089 / 0.075. `6042866` sits with `fb6f06f` on arc-wipe (HEAD 1.01-1.18x
slower, the gap growing as the scale falls) and with HEAD on segment-travel: I59-I62 took ~10% off segment frames
on this GPU, and V25 gave it back.

So the regression is confined to frames that change the config, it is the
same absolute cost at every scale (~0.04-0.05 ms, so the full-resolution
edge ring), and it arrives between `fb6f06f` and `a38797a`.

## 4. I64: the per-piece filament walk in the edge ring

### 4.1 Attribution

V25 replaced one nearest-point read with a max over the outline's eight
pieces (`neon.frag`, the `for (int piece ...)` loop in `main()`). Pass 1b never
runs it (`filamentLit` is false there, I62), so only the edge ring pays, and
only where the ring is shaded directly - every frame of an animation. Each
variant below is HEAD with one change, arc-wipe frames, each timed in its own
session of three rotated rounds against HEAD; ms, mean over eleven scenes
(`bounded_band` left out: one of its GPU timer reads came back 0). HEAD read
the same in all five sessions, within 0.003 ms wall:

| variant | what changed | wall 0.5 | wall 0.25 | GPU 0.5 | GPU 0.25 |
| ------- | ------------ | -------: | --------: | ------: | -------: |
| HEAD | - | 0.440-0.443 | 0.319-0.321 | 0.293-0.300 | 0.166-0.170 |
| `fb6f06f` | before V25 | 0.396-0.397 | 0.274 | 0.247-0.250 | 0.125-0.132 |
| A | `perPiece = false`: the walk collapsed to one pass (wrong image) | 0.394-0.402 | 0.270-0.276 | 0.246-0.258 | 0.125-0.130 |
| C | walk kept, `filamentCover` inside it stubbed (wrong image) | 0.426 | 0.303 | 0.278 | 0.155 |
| B | exact: cheap rejects before any `sqrt` / `atan`, outline geometry hoisted out of the loop | 0.436 | 0.315 | 0.293 | 0.163 |
| E | exact code, loop forced to ONE iteration at run time (wrong image) | 0.402 | 0.276 | 0.252 | 0.129 |
| F | exact: candidate prefilter (4.2) | 0.407 | 0.282 | 0.258 | 0.140 |

- A recovers everything: the walk is the whole regression.
- E, the same compiled code running one iteration instead of eight, recovers
  as much as A. So it is executed work, not register pressure or code size.
- B, which makes the iterations that reach nothing nearly free on paper
  (an `abs` and a compare), recovers almost nothing; C, which removes the
  coverage reads from the body, recovers a third. The early returns are not
  saving the work they guard: the compiler is evidently if-converting the
  loop body, so every iteration pays `filamentPieceDistance`'s `atan`,
  `filamentCore`'s `pow` / `exp2`, `perimeterAt` and `filamentCover`, masked.

### 4.2 The fix (prototype F)

Make the loop's trip count the number of pieces that can be in reach, decided
by tests that cost an `abs` each, and keep the body exactly as it is. Two
facts make the candidate set small and exact:

- a straight's distance is at least the fragment's offset from its LINE, so a
  straight whose line is a reach or more away returns -1 from
  `filamentPieceDistance` anyway (the test is loosened by 1e-4 relative so
  rounding in `length()` can never drop a piece the full test keeps);
- a corner returns -1 outside its own quadrant (`w.x * sg.x <= 0 || w.y *
  sg.y <= 0`), and a fragment lies in at most one corner's quadrant - the one
  its signs pick - so at most one corner is ever a candidate.

```glsl
int cands = 0;   // candidate pieces, 4 bits each
int count = 0;
if (filamentLit) {
    if (!perPiece) {
        count = 1;
    } else {
        float halfW = uRectSize.x * 0.5;
        float halfH = uRectSize.y * 0.5;
        float r     = clamp(uCornerRadius, 0.0, min(halfW, halfH));
        float rj    = filReach * 1.0001;
        if (abs(vPos.y - halfH) < rj) { cands |= 0 << (4 * count); count++; }
        if (abs(vPos.x - halfW) < rj) { cands |= 2 << (4 * count); count++; }
        if (abs(vPos.y + halfH) < rj) { cands |= 4 << (4 * count); count++; }
        if (abs(vPos.x + halfW) < rj) { cands |= 6 << (4 * count); count++; }
        if (r > 0.0 && abs(vPos.x) > halfW - r && abs(vPos.y) > halfH - r) {
            int corner = (vPos.x > 0.0) ? ((vPos.y > 0.0) ? 1 : 3) : ((vPos.y > 0.0) ? 7 : 5);
            cands |= corner << (4 * count);
            count++;
        }
    }
}
for (int k = 0; k < count; k++) {
    int piece = (cands >> (4 * k)) & 15;
    // ... the existing body, unchanged ...
}
```

Still one call site for `filamentCover`, still a loop whose bound comes from
the data. The visiting order changes (straights, then the corner), which a
max does not see. At most five candidates (four straights on a rect thinner
than twice the reach, plus one corner), so 20 bits. Integer shifts and `|`
are GLSL ES 3.00 core.

**Verified byte-identical**: 306 `cover` frames (`diff`: 0 pixels differ) and
210 page images (`cmp`), and `check` passes. `partition` is untouched (no
geometry changes). Not yet run on the AMD or the target.

**Measured** (three rotated rounds; the many-light rows are one run each):

| frame | HEAD | F | `fb6f06f` | F vs HEAD |
| ----- | ---: | -: | --------: | --------: |
| arc-wipe, 0.5, wall | 0.440 | 0.407 | 0.396 | 1.08x |
| arc-wipe, 0.25, wall | 0.319 | 0.282 | 0.274 | 1.13x |
| arc-wipe, 0.25, GPU | 0.166 | 0.140 | 0.125 | 1.19x |
| segment-travel, 0.5, wall | 0.527 | 0.492 | 0.479 | 1.07x |
| segment-travel, 0.25, wall | 0.401 | 0.368 | 0.354 | 1.09x |
| 8 arcs changing length, 0.5 | 0.468 | 0.429 | - | 1.09x |
| 8 arcs changing length, 0.25 | 0.366 | 0.319 | - | 1.15x |
| 8 segments, 0.25 | 0.514 | 0.473 | - | 1.09x |
| 4 arcs + 4 segments, 0.25 | 0.485 | 0.443 | - | 1.10x |

Still and hue frames do not run the walk (they composite the ring's field)
and read the same before and after. What F leaves of the regression (~0.01
ms) is the walk's real work at corners, where two or three pieces are in
reach.

### 4.3 As built

The working tree's `neon.frag` is F with the names cleaned up (`pieces`,
`pieceEnd`, `inReach`), and `filamentPieceDistance` notes that its -1 cases
now pick the candidates. Against the build before it, Release, all twelve
scenes, three rotated rounds (six for the 1.0 check), wall-clock:

| frame | 1.0 | 0.5 | 0.25 |
| ----- | --: | --: | ---: |
| arc wipe, mean ms before -> after | 0.989 -> 0.964 | 0.443 -> 0.410 | 0.321 -> 0.283 |
| arc wipe, geomean speedup | 1.03x | 1.08x | 1.13x |
| arc wipe, GPU geomean | 1.03x | 1.12x | 1.20x |
| segment travel, geomean speedup | 1.00x | 1.05x | 1.07x |
| segment travel, GPU geomean | 1.02x | 1.11x | 1.16x |

Per scene at 0.5 / 0.25, arc wipe: 1.01-1.17x / 1.06-1.22x, none slower; at
1.0 the four scenes a three-round run put at 0.92-0.99x read 1.00-1.03x over
six. Still and hue frames do not run the walk and read the same within the
harness noise (their GPU timer readings swing 0.000-0.065 ms within one
build). Byte-identical on the 306 `cover` frames and the 210 page images;
`check` and `partition` (seeds 1, 7) pass; all targets build.

The lesson for `neon.frag`: an early `continue` / `return` inside a loop is
not a skip on this compiler. Where the work must be skipped, the loop bound
has to say so. This is the same family of finding as `addCornerPiece`'s
"one arc at a time" and the "do not wrap the corrections in `if
(!uniformCover)`" notes already in the shader.

## 5. V27: stale abutment amounts after a geometry change (PR #65)

`PackArcFlags` now turns each endpoint's open gap into an abutment amount over
the feather width, and the feather widths are perimeter fractions, so the arc
block depends on `GetPerimeter(config)`. But `OnConfigChanged` sets
`mLightBlocks.dirty` only on `segmentsDirty || arcsDirty`. A change to width,
height or corner radius with the same arcs leaves the old perimeter's amounts in
the UBO, and the emission table, the coverage table (which re-bakes every piece
on a shape change, from that stale block) and the filament all read them.

Repro (probe in the session scratchpad): two arcs of length 0.3 with a gap,
800 x 400 -> 1600 x 800, steady frame against a fresh effect given the second
config directly:

| gap | HEAD | `a38797a` (before PR #65) |
| --- | ---- | ------------------------- |
| 0.003 | max 137 levels, 155k channels differ | 0 |
| 0.005 | max 85, 65k differ | 0 |
| 0.05 (wider than a feather at both sizes) | 0 | 0 |

The other direction (big -> small) reads 133 levels. It persists until the
arcs next change.

**Fixed** in the working tree: `OnConfigChanged` computes `arcPerimeterDirty`
(`!config.neon.arcs.empty() && GetPerimeter(config) !=
GetPerimeter(mCurrentConfig)`) and adds it to `mLightBlocks.dirty`. The repro
reads 0 levels both ways at every gap. Cost: a resize animation with arcs repacks a 144-byte block per frame; the
emission table re-bakes only when the bytes change, i.e. only while an
endpoint is within a feather of a neighbour.

Also from PR #65, not a defect: an arc whose neighbour moves within a feather
width now changes `.w` every frame, which takes I54's "only moved" path away
from it (`before.w != after.w`), so those frames re-bake both supports. Correct,
and only on the frames two ends are that close.

## 6. CPU per frame

`SetConfig` / `Update` / `Render`, median us per frame on an M2 P-core, the
neon alone, 960 x 540 rect at 1080p, scale 0.5, GPU drained outside the timed
region (two runs, which bracket the figures):

| frame | `SetConfig` | `Update` | `Render` | total | `6042866` total |
| ----- | ----------: | -------: | -------: | ----: | --------------: |
| still | 0 | 0.2 | 36-45 | 37-45 | 36-47 |
| hue | 0 | 0.2 | 89-107 | 89-107 | 86-107 |
| intensity | 3.1 | 0.1 | 68 | 71 | 71 |
| resize | 4.8 | 0.1 | 92-96 | 97-101 | 103-117 |
| 8 arcs changing | 2.7 | 0.1 | 133-141 | 136-144 | 140-141 |
| 8 segments changing | 1.4 | 0.1 | 149-156 | 151-157 | 134 |

Sampled (`sample`, no per-frame `glFinish`), a hue frame's `Render` is five
passes in near-equal parts - emission 29%, gather 24%, field composite 20%,
blit 18%, ring field composite 7% - and inside each, `glDrawArrays` is 70-93%,
almost all of it `GLDContextRec::beginRenderPass` creating a Metal render
encoder. `ShaderProgram::SetUniform` (the by-name hash lookups) is 3-15% of a
pass. The ring field composite, which shares the blit's target, costs a
third of the blit: the price is per render-target switch, not per draw.

What this means:

- The library's own CPU work is small; on Apple GL the count of
  render-target switches per frame is the CPU lever: still 1, hue 4, an
  animated frame 5-7. The one CPU figure that moved since `6042866` is eight
  segments, 134 -> 151-157 us: I60's segment table added two render passes
  (P0s into it, P0f back into the main table) to every segment frame.
- The segment table path binds the main coverage table twice (P0b, then P0s
  into the segment table, then P0f back into the main one). Baking P0s first
  binds it once - exact (P0s and P0b read neither table, P0f reads only the
  segment table) and verified byte-identical on the `cover` set, but not
  measurable on the M2 (prototype G, three rounds, +-0.3%). It is a tile store
  and reload of the table on a tiler; measure it there before taking it.
- Whether the target's driver charges per render pass the way this one does is
  unknown; the figures above are what to compare against there.

## 7. Hitches: lazily built programs and tables

Frame time, `glFinish` to `glFinish`, the frame a config first needs something
(two runs; steady frames read 0.4-0.9 ms here, which is this probe's sync
floor, not the frame):

| the config's first ... | that frame | next |
| ---------------------- | ---------: | ---: |
| frame (full ring, still) | 46-51 ms | 1.1-1.3 |
| hue rotation | 1.6-1.9 | 2.2 |
| partial arc (coverage-bake program, table) | 10.4-11.2 | 1.1 |
| segment (fill program, segment table, RG -> RGBA promotion) | 6.4-8.1 | 1.5-2.2 |
| opaque fill | 2.5-3.7 | 0.5-1.1 |

The first frame was ~20 ms in `neon-perf-plan.md` section 10.4 (2026-10-06);
it is ~45 ms (median of 216 fresh effects in the harness) because the first
frame of a field config now builds the field's programs and bakes it on
purpose, so their own first-draw stalls do not land later. That trade is
sound, but it means every program the neon can use is compiled on some frame
the user sees, and a TV-class driver compiles several times slower than this
one. Two ways to take it off the visible frames, neither built:

- **A program binary cache** (`glGetProgramBinary` / `glProgramBinary`, core
  in GLES 3.0): the host hands the library a directory; programs are loaded
  rather than compiled after the first run. Takes the cold start and every
  mid-session hitch on every run after the first.
- **An opt-in prewarm** (`NeonRenderer::Prewarm()` or a C ABI call): build
  every program and draw each once into a scratch target during a loading
  screen. Covers the first run too; costs the memory of tables a config may
  never need, unless they are released after.

## 8. Checked and set aside

- **V26 (`sdRoundBoxFwidth`)**: the blit computes it only behind its uniform
  branch, and still / hue frames are flat on GPU timers. No cost found.
- **Uniform lookups by name** (`unordered_map<std::string, GLint>` with a
  temporary string per call): 3-15% of a pass's CPU, ~3% overall. Real but
  small; caching locations per program at build time would take it, not worth
  the churn on its own.
- **Prototype B** (cheap rejects, hoisted geometry): exact, recovers ~0.005 ms
  of the walk's 0.045. F supersedes it.
- **Prototype G** (coverage-bake reorder): see section 6.
- **PR #65's per-fragment cost** (`arcTailAbut` / `arcHeadAbut` decode, the
  sliding smoothstep edges): not measurable, HEAD vs `a38797a` 1.00x on every
  mode and scale.

## 9. Recommended order

| # | item | kind | measured / expected | image |
| - | ---- | ---- | ------------------- | ----- |
| 1 | V27: perimeter in the light-block gate - **built** | correctness | removes up to 137-level stale frames after a resize | fixes it |
| 2 | I64: build F - **built** | exact perf | animated frames 1.05-1.13x wall, 1.11-1.20x GPU on the M2 at 0.5 / 0.25 | byte-identical (306 + 210 frames) |
| 3 | Measure on the target (still `neon-reduced-scale-plan.md` step 0) | - | which frames production draws; half-float render support (both fields need it); per-pass time; per-render-pass CPU | - |
| 4 | Program binary cache or prewarm | startup / hitches | ~45 ms first frame, 6-11 ms mid-session here, more on the target | none |
| 5 | Open items that still stand: segment-only gather on segment-only frames (`neon-reduced-scale-plan.md` 4a, exact); the lens flare's `resolutionScale` if production draws it; `neon-animation-perf-analysis.md` section 8 | perf | see those documents | see those documents |
| 6 | Coverage-bake reorder (G) | exact perf | not measurable on the M2; one render pass fewer per segment frame on a tiler | byte-identical |

After F, an animated frame on the M2 at 0.5 is ~0.41 ms wall with one arc
moving and ~0.6 ms with eight segments; the largest passes there are the
shading (P1b, ~0.13 ms GPU at 0.5) and, with segments, the segment table's
bake (P0s, ~0.09-0.13). Both are covered by `neon-animation-perf-analysis.md`
section 8 and `neon-glow-cover-resolution-plan.md`; nothing found here changes
their ranking.
