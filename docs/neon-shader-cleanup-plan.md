# Neon shaders: cleanup plan

How to take the neon layer's shaders from thirteen program slots, four
compile-time variants of `neon.frag` and several hand-copied functions down to
the set that earns its place, without changing what the layer draws. Written
2026-10-07 against `1488648` plus the uncommitted `NEON_UNIFORM_COVER` twins,
from measurements on an **AMD Radeon Pro 5300M** at 1920 x 1080; the Apple M2
Pro figures quoted come from [`neon-perf-plan.md`](neon-perf-plan.md) and
[`review-findings.md`](review-findings.md).

**The principle.** The neon's arithmetic is already one render logic for every
lighting case: an absent arc or segment enters every sum as an exact 0, which
is why an empty `arcs` list draws nothing with no special case anywhere. What
differs between "uniform ring", "partly lit" and "segments" is a layer of
shortcuts on top - passes skipped, programs specialised, buffers narrowed -
each added for a measured gain. The cleanup keeps the shortcuts that pay for
themselves, removes the ones that do not, and puts every piece of shared code
in one place.

## 1. What there is today

### 1.1 Program slots

All but the first two are built lazily, on the first frame that needs them; a
slot never built costs nothing.

| slot | source and defines | draws | built |
| ---- | ------------------ | ----- | ----- |
| `mEmissionShader` | `neon-emission.frag` | pass 0, the emission table | `Initialize` |
| `mBlackRectShader` | `black-rect.frag` | pass 2a, the opaque fill | `Initialize` |
| `mGlowCoverShader` | `neon-glow-cover.frag` (+ `NEON_GLOW_COVER_BAKE` in `neon-pieces.glsl`) | pass 0b, the coverage table | first partly lit frame |
| `mNeonGatherShader` | `neon-gather.frag` | pass 1a, the gather | first frame at any scale |
| `mNeonShader` | `neon.frag` | pass 1 with the gather inline, at 1.0 | **never with the shipped constants** (I44) |
| `mNeonShadeShader` | `neon.frag` + `NEON_READS_GATHER` | pass 1b, below 1.0 | first scaled frame |
| `mNeonRingShader` | `neon.frag` + `NEON_READS_GATHER` | pass 2c below 1.0; pass 1 at 1.0 | first frame |
| `mNeonUniformShader`, `mNeonShadeUniformShader`, `mNeonRingUniformShader` | the three above + `NEON_UNIFORM_COVER` | the same, on a uniformly lit ring | uncommitted |
| `mBlitShader` | `neon-blit.frag` | pass 2b, below 1.0 | first scaled frame |
| `mNeonFieldShader` | `neon.frag` + `NEON_READS_GATHER` + `NEON_FIELD_BAKE` | pass 1f, the field | first settled frame at 1.0 |
| `mFieldCompositeShader` | `neon-field.frag` | pass 1c | the same |

What a host compiles: at 1.0 with a uniform ring (the default) six programs;
partly lit seven; a ring switching between the two eight; below 1.0 six or
seven, up to nine with the twins.

### 1.2 Duplicated code

| what | copies | state |
| ---- | ------ | ----- |
| `sdRoundBox` | `neon.frag`, `neon-blit.frag`, `black-rect.frag` | byte-identical |
| `bandOuterDistance` | the same three | byte-identical |
| `bandInnerDistance` | `neon.frag`, `black-rect.frag` | byte-identical |
| the tone map (hue-preserving Reinhard + gamma) | `neon.frag`, `neon-field.frag` | identical expressions ("verbatim") |
| the one-sided cut and the two cutoff ramps | `neon.frag`, `neon-blit.frag` ("one edge, written twice") | same expressions; the AA width differs only by `uResolutionScale` |

GLSL has no `#include`; the project's rule ([`CLAUDE.md`](../CLAUDE.md)) is that
a chunk shared by several shaders goes in a `.glsl` file injected through
`shaders.h.in`, not copied. These five predate or escaped it.

### 1.3 Two predicates for one decision

"The ring is lit uniformly" is decided twice: on the CPU by
`IsGlowCoverUnread` (`length >= 1 - 5e-7`), which skips the coverage table's
bake and allocation, and in `neon.frag` by `uniformCover`
(`length >= 1 - 1e-6`), which zeroes the glow fix. The CPU test is kept "a hair
stricter" so the shader never trusts a stale table, and both carry a comment
saying to change them together. `uPerimeterUnread` (I45) is already a CPU
decision passed as a uniform; `uniformCover` is not.

### 1.4 Size

`neon.frag` is 1826 lines: 1252 comment, 489 code. Most of the comment is the
measured history behind each line - valuable, but most of it is also in
`review-findings.md` and the plan documents.

## 2. Goals and limits

- **No visible change.** Every step is byte-identical, or within 1 level on
  a stated share of pixels, measured against the library before it on the same
  GPU (not against the comparison page's committed images, which are the M2's
  and leave the AMD no headroom at 1.0).
- **Not slower, not bigger**, on both GPUs, in all five `neon-scale-check
  time` modes.
- **Out of scope**: merging the shading and ring programs (one program drawing
  an offscreen target unblended and the caller's blended in one frame measured
  93-624x slower on the AMD driver - `ensurePathPrograms`); removing the split
  gather or the field; the debug overlays' shaders, which belong to
  `DebugRenderer`.

## 3. The steps

In order. Each is one commit, verified on its own (section 4).

### Step 0. Revert the `NEON_UNIFORM_COVER` twins (uncommitted) - S

**Why.** Measured against `1488648`, three interleaved rounds, geometric mean
over the twelve scenes: 1.05x (1.0) to 1.10x (0.25) on intensity-pulse frames,
1.00x in every other mode - for three more program slots, three program bits,
`ensureShadingProgram`, `PickShadingProgram`, a `readsGlowCover` parameter and
two `#ifndef` blocks in `neon.frag`. Only frames whose config animates over a
uniformly lit ring gain, and the general program already draws them correctly.

**How.** Remove the twins' hunks: in `neon.frag` the variant note at the top,
the `#ifdef NEON_UNIFORM_COVER` around `uniformCover` and the three `#ifndef`
blocks; in `neon-renderer.h` / `.cpp` the three slots, their program bits,
`ensureShadingProgram`, `PickShadingProgram`, the `uniformCover` parameters of
`ensurePathPrograms`, `renderNeonPass` and `renderRingPass`, and
`uploadNeonUniforms`' `readsGlowCover`; the twins' sentences in `CLAUDE.md`'s
per-path programs paragraph and in the header's class comment. Not a whole-file
revert: the same uncommitted set holds I48's comment and doc edits
(`bloomSegment`'s note in `neon.frag`, `neon-pieces.glsl`, `CLAUDE.md`,
`review-findings.md`, `neon-perf-plan.md`), which stay.

**Exact**: back to `1488648`. **Slots after**: 10.

### Step 1. One copy of the shared code - M

Two new chunks, injected like `neon-common.glsl`:

- `lib/shaders/neon-sdf.glsl` (`@NEON_SDF@`): `sdRoundBox`,
  `bandOuterDistance`, `bandInnerDistance`, and `vec3 glowEdgeFactors(...)`
  returning the one-sided cut and the two cutoff ramps as three factors from
  `d`, the rect-local position, the half-size, the corner radius, the AA width
  and the side / softness / cutoff values - every input a PARAMETER, no uniform
  read, since `black-rect.frag` gets the chunk too and declares none of the
  glow's uniforms. Into `neon.frag`, `neon-blit.frag` and `black-rect.frag`.
- `lib/shaders/neon-grade.glsl` (`@NEON_GRADE@`): `vec3 neonToneMap(vec3)`, the
  tone map. Into `neon.frag` and `neon-field.frag`.

`glowEdgeFactors` returns the factors rather than applying them so each caller
keeps its own multiplication order (`neon.frag` multiplies the result by each
in turn; the blit builds one `cut` and multiplies once). Reassociating would
move pixels by a level; this way nothing does. Each shader keeps computing its
own AA width - `fwidth(d) * uResolutionScale` in `neon.frag`, `fwidth(d)` in the
blit - which is the one real difference between the two edges.

Files: the three shaders, the two new chunks, `shaders.h.in` (the sources that
take them), `lib/CMakeLists.txt` (`CMAKE_CONFIGURE_DEPENDS` and the
`file(READ ...)` list - three places, per `CLAUDE.md`). Never write an
`@NAME@` placeholder in a comment in `shaders.h.in`.

**Exact**: byte-identical, every image. If a driver reorders anything and a
pixel moves, inline that function back rather than accept it - this step is
only worth doing for free.

*Done, byte-identical (I49) - without `glowEdgeFactors`.* In `neon.frag` the
mask's inputs (`sideBack`, `sideSoft`, the cutoffs' mids and halves) are
derived early and shared with the discards and the quad fade, so a shared
function would have held only the three `smoothstep`s, and it needs the
tuning header's `GLOW_SIDE_*` constants, which `black-rect.frag` does not get.
Not worth a chunk. Both chunks declare their own `precision highp float`: they
land ahead of each file's own precision line, and an ES fragment shader has no
default float precision.

### Step 2. One predicate for "the ring is lit uniformly" - S

Replace `neon.frag`'s own `uniformCover` test with a uniform,
`uUniformCover`, set from `IsGlowCoverUnread` in `uploadNeonUniforms` - as
`uPerimeterUnread` already is. The "a hair stricter, change the two together"
coupling goes, and so does reading `uArcs[0]` for it.

**Not quite exact**, in one sliver: for an arc length in [1 - 1e-6, 1 - 5e-7)
the shader now reads the coverage table (baked, since the CPU called that ring
partly lit) instead of assuming it uniform - which is the more correct of the
two. Everywhere else identical. Verify with an arc wipe through full length
(the `arc-wipe` walk) as well as the still images.

### Step 3. Remove the inline-gather path - M, owner's decision

**Why.** Since I44 every rect splits the gather at 1.0, so `mNeonShader`, the
plain `neon.frag` and the gate that chooses it are never built or taken with
the shipped constants. They survive as the fallback "for a GPU that needs the
gate back". This session measured that GPU - the AMD the old gate was tuned
on - with the M2's gate against its own: 0.98-1.02x in every animated mode,
better still frames, and one loss, `bounded_band` under an intensity pulse at
0.84x (0.247 -> 0.295 ms).

**What goes**: `mNeonShader`, `PROGRAM_PLAIN`, `CanSplitGatherAtFullRes`,
`SplitsGatherAtFullRes`, `FULL_RES_SPLIT_GATHER_MAX_SCALE`,
`FULL_RES_SPLIT_MIN_AREA_PX`, `renderNeonPass`'s inline branch and its
`readsGather` parameter, `bindGatherInputs` for a shading program, the
`NEON_READS_GATHER` define itself (`neon.frag` then always reads the gather),
and the `#ifndef NEON_READS_GATHER` half of `neon-common.glsl`, whose loop moves
into `neon-gather.frag`, its only remaining user.

**What it needs in return**: a cap on the split's gather scale at 1.0 (one
constant, 0.5 - what the scaled path at 0.5 already gathers at). Without the
inline fallback, a rect small enough to gather near full resolution holds a
gather buffer up to its glow quad at full resolution: 16 MB at 1080p for a
40 x 24 rect with `glowRadius` 20, 33 MB with segments (I44). At 0.5 that is a
quarter. The check suite's `small_rect` at 0.5 reads 1 level off 1.0, which is
what the cap should cost it; measure it.

**Not exact** for small rects only (the cap); every other config already
splits and is unchanged. **Slots after**: 9. `neon.frag` variants after: the
shading (built twice, shade and ring) and the field bake.

*Done (I49), with the cap at 0.5 on the owner's choice.* Every check image
byte-identical. Measured on the rects the cap binds on, 1080p, scale 1.0:
a 40 x 24 rect with `glowRadius` 20 went 20.0 -> 8.1 MB of textures (33.2 ->
9.4 with a segment) and 3.25 -> 0.99 ms with the hue rotating, for up to 9
levels on ~280 channels near its line; 64 x 36 up to 5, 128 x 72 up to 1. The
estimate above ("1 level") held only for `small_rect`, which the cap does not
reach. A cap of 0.75 read 13.0 MB / 1.92 ms / 3 levels on the same rect.

The decision: dropping the tuned-back fallback means a future GPU whose
offscreen pass is dear cannot be rescued by a constant. The AMD was that GPU,
and on today's tree it does not need rescuing.

### Step 4. One gather loop body - S, keep only if measured free

`gatherPerimeter` has two copies of its loop: one reading both emission rows
(segments), one reading only row 0. Row 1 is exactly zero without segments,
so a single body reading both is the same arithmetic plus one `texelFetch` per
sample. The gather runs every frame the hue rotates; time `hue` and
`intensity` on both GPUs and adopt only at 1.00x. Exact either way (adding
exact zeros).

*Measured and set aside.* Byte-identical, as expected, and slower: 0.88x
geometric mean on hue frames at 1.0 (0.72x on the worst scene), 0.94-0.98x
below it, 0.97-1.00x under an intensity pulse; on the tiny rects the gather
buffer is largest for, 0.98 -> 1.24 ms (40 x 24, `glowRadius` 20) and 0.29 ->
0.35 ms (64 x 36). The extra fetch per sample still costs on the coarse grid.
The two bodies stay, with this measurement in their comment.

### Step 5. A field for segments - L, optional

Not a cleanup, but the one change that would put segments on the same frame
path as everything else. Today a still frame with segments shades the whole
quad every frame: 1.206 ms against 0.073 for the uniform ring (`segments` vs
`default` at 1.0). With segments the output is
`mask * tonemap(col * Fa + segHue * Fs)` - still linear in both hues - so the
bake writes `Fs` beside `Fa`, and the composite reads the gather's second
attachment. Eligibility keeps one condition: a stop-less segment reads the
ring's alpha, so under a rotating hue the ring must be opaque, as for the arcs
today. Memory doubles for segment configs (about 7 MB for the default rect at
1080p). A travelling segment changes the config every frame and never uses it.

### Step 6. Comment diet - M, last, comments only

Cut `neon.frag` (and the other large comment blocks) to what a reader needs
at the line: the invariant, the load-bearing warning, the I-number. The
measured history moves to the document that owns it, and is linked. Rules:

- **Move, never delete.** Anything not already in `review-findings.md` or a
  plan document goes there first.
- **Keep every "do not" warning** (the uniform-cover branch, the four corner
  calls, `invariant gl_Position`, the shade / ring split, the factorisation
  invariant) verbatim.
- **A comments-only diff.** Verify by stripping comments from both sides and
  comparing what is left, then byte-identical images.

*Done for `neon.frag` only, on the owner's choice of scope.* 33 blocks
condensed by a script that replaced each whole `//` block and appended the
original, verbatim, to [`neon-frag-notes.md`](neon-frag-notes.md) under the name of
what it explains: 1,746 -> 1,154 lines, 1,207 -> 615 comment lines. Proven
comments-only - the comment-stripped code is identical line for line, and
every removed comment line is in the notes - then built, `check` and
`partition` passing, every image byte-identical. The four warnings kept word
for word: `uniformCover`'s "Do NOT turn this into an `if (!uniformCover)`",
`addCornerPiece`'s "Keep the four calls whole", the arc pedestal's "Do NOT
collapse this further", and the reversed-edge `smoothstep` rule; the first
two now say their measurement predates the inline loop's removal.

## 4. Verification, every step

1. Release build of the library and tools; `neon-scale-check check` and
   `partition --seed 1` and `--seed 7`.
2. Image A/B against the library before the step, same GPU:
   `neon-scale-check generate DIR --images --no-timing` from both, then a
   per-channel diff of every image (all twelve scenes, six scales, the
   hairline sweep). State the result as max level and share of channels.
3. `neon-scale-check time` in all five modes at 1920 x 1080, three
   interleaved rounds with the order rotated, median per figure. Both GPUs -
   and log `GL_RENDERER`: the AMD machine can switch to its Intel UHD 630 per
   process. Two separately built binaries differ by up to ~10% on hue frames
   at 1.0 with no code change between them (seen twice this session), so read
   that column with care and confirm a surprise there with a rerun.
4. Memory for step 3: live texture bytes for the tiny rect, before and after.
5. If any pixel moved: rerun `neon-guide-figures` and diff
   `docs/images/neon-onboarding/`; regenerate the comparison page's images on
   the M2 if the 1.0 column moved.
6. Docs: the `CLAUDE.md` paragraphs the step touches, a `review-findings.md`
   entry, and this document's status table.

## 5. End state

| | today (with the twins) | after steps 0-3 |
| - | ---------------------- | --------------- |
| program slots | 13 | 9 |
| compile-time variants of `neon.frag` | 4 (`READS_GATHER`, `FIELD_BAKE`, `UNIFORM_COVER`, none) | 1 (`FIELD_BAKE`) |
| programs a default host at 1.0 compiles | 6 | 6 |
| hand-copied functions | 5 | 0 |
| predicates for "uniformly lit" | 2 (CPU, shader) | 1 (CPU) |

The default host compiles the same six either way; what goes is the code and
the paths no shipped configuration takes.

## 6. Status

| step | state |
| ---- | ----- |
| 0. Revert the twins | **done** |
| 1. Shared SDF and grade chunks | **done**, byte-identical (I49); the edge-mask function dropped |
| 2. One uniform-cover predicate | **done**, byte-identical (I49) |
| 3. Remove the inline-gather path | **done** (I49), `GATHER_MAX_SCALE` 0.5 |
| 4. One gather loop body | **set aside**: 0.88x on hue frames at 1.0 |
| 5. A field for segments | **declined** by the owner: the memory |
| 6. Comment diet | **done** for `neon.frag` (owner's scope): 1,746 -> 1,154 lines, comments 1,207 -> 615, code identical, images byte-identical; the history is in [`neon-frag-notes.md`](neon-frag-notes.md) |
