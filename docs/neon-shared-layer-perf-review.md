# The shared layer's performance review

Measured 2026-10-17 on an Apple M2 Pro, after the four steps of
[`neon-shared-layer-plan.md`](neon-shared-layer-plan.md) and the C++ review
that followed them (`29e30af`). It covers:

- what sharing the neon's picture costs, by who reads it and by frame type;
- where that cost goes, pass by pass;
- what it costs in memory and on the first frames;
- the three changes this review made;
- what is left, ranked.

[`neon-shared-buffer.md`](neon-shared-buffer.md) explains the mechanism this
page measures.

---

## 1. The bottom line

- **A host that reads nothing pays nothing.** Moving frames cost what they
  cost on `main` (0.97-1.00), and still frames a little less (0.95-0.98).
  With the default rotating hue, the first frame is 36.0 ms, against 40.6 at
  `29e30af` and 35.9 before the plan.
- **A reader of the parts pays 0.016-0.033 ms a moving frame** (1.04-1.36x,
  the large ratios only on frames that are small to begin with). That is the
  ring going through its atlas: one more render pass.
- **A reader of the shared buffer pays 0.04-0.19 ms a moving frame**
  (1.12-2.06x). At view 0.5 nearly all of it is fixed cost - two render
  passes and the buffer's clear and store - and almost none of it shading:
  with pass 1s drawing nothing at all, a hue frame costs 0.208 ms, against
  0.209 with it.
- **Against the re-shaded buffer it replaced** (section 5.3):
  - at scale 1 the assembled buffer is now 3-12% cheaper;
  - at 0.5 it is 1.10-1.16x dearer on hue and intensity frames, down from
    1.28x at `29e30af`. The extra cost is the atlas's render pass, which the
    old buffer skipped by shading a thin line at half resolution, up to 82
    levels wrong.
- **This review made three changes**, all inside the existing gates:
  - pass 1s reads the glow once per texel away from the ring (1.05-1.11x at
    0.5);
  - pass 1s reads once per texel at scale 1, with no loop (1.06-1.11x);
  - the ring atlas's programs are built on the first frame only when the
    config can hold still (-4.6 ms on a rotating hue's first frame).

---

## 2. How it was measured

- **Machine and build.** Apple M2 Pro, macOS, `Release`, 1920 x 1080.
- **Tool.** `neon-scale-check time --scales 1,0.5 --size 1920x1080 --mode M`.
  - Five frame types: `still`, `hue`, `intensity`, `arc-wipe`,
    `segment-travel`.
  - The twelve scenes of
    [`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html).
  - Each figure is the best of five runs of 40 frames, CPU and GPU together
    (`glFinish` around the runs, no swap).
- **"View"** is the neon's own `resolutionScale` (the tool's `--scales`).
  **"Shared"** is `compositeScale`, which is clamped to at least the view, so
  "shared 0.5" at view 1 draws the shared buffer at 1.
- **Readers.** A scratch build of the tool declares the reads a host would:
  - `EdgeLightingEffect::AddHostRead("NeonRenderer.Ring")` for a reader of
    the parts;
  - `AddHostRead("NeonRenderer.Composite")` plus a `compositeScale` for a
    reader of the shared buffer.

  Nothing samples them. What is timed is what the neon does for a reader.
- **Rounds.** Every build and reader was run three times, interleaved, in
  alternating order, and each scene's median taken.
  - A figure is the geometric mean, over the twelve scenes, of the ratio of
    two medians, with the smallest and largest scene in brackets. Median
    frame times follow in ms.
  - A ratio below 1 means the first build is faster.
- **Builds.**
  - `main`;
  - `c08639a`, the branch before the plan, where the shared buffer re-shaded
    the neon a second time;
  - `29e30af`, the plan as built and reviewed;
  - "current", this review's changes on top of `29e30af`.
- **Per pass.** `--gpu --passes`: one timer query per pass. On the M2, a
  tiler, a pass that opens a render pass carries that render pass's load and
  store too. So a short pass into a target of its own reads as a fixed
  ~0.02-0.03 ms whatever it draws. Use these to rank passes, not as absolute
  costs.
- **First frames.** A scratch probe times the first five frames one at a time,
  `glFinish` after each, on the `default` scene. That adds a submit-and-wait to
  every frame, so frames 2-5 read 0.5-1.5 ms here rather than the ~0.1 ms of
  a batched run.

---

## 3. A host that reads nothing

The plan's first promise: when nobody shares anything, the neon costs what it
cost before. `29e30af` against `main` and against the branch before the plan:

| frame type | vs `main`, view 1 | vs `main`, view 0.5 | vs `c08639a`, view 1 | vs `c08639a`, view 0.5 |
| ---------- | ----------------- | ------------------- | -------------------- | ---------------------- |
| still | 0.98 (0.80-1.18) | 0.98 (0.82-1.15) | 0.96 (0.82-1.30) | 0.95 (0.81-1.12) |
| hue | 1.00 (0.99-1.03) | 1.00 (0.98-1.02) | 1.00 (0.98-1.02) | 0.98 (0.96-1.00) |
| intensity | 1.00 (0.97-1.01) | 0.99 (0.98-1.02) | 0.99 (0.97-1.02) | 0.96 (0.93-1.00) |
| arc-wipe | 0.97 (0.95-0.99) | 0.98 (0.97-0.99) | 1.00 (0.99-1.00) | 0.99 (0.98-1.00) |
| segment-travel | 0.98 (0.97-1.00) | 0.98 (0.97-0.99) | 1.00 (0.99-1.00) | 0.99 (0.99-1.00) |

Median frames at view 0.5: still 0.066 ms, hue 0.143, intensity 0.080,
arc-wipe 0.374, segment-travel 0.457. The current build is the same on every
timed frame (current against `29e30af`: 1.00 on every moving frame type).

- **Moving frames are unchanged.** A moving frame nobody reads draws the ring
  on the screen exactly as before (`ringAtlasDraw` is false), so nothing new
  runs.
- **Still frames are a little faster.** The ring is drawn into its atlas on
  the first still frame and only copied after that. The spread is wide
  because a still frame is ~0.06 ms, so one scene's noise is a large fraction
  of it.
- **The first frame is not**: see section 7.

---

## 4. A reader of the parts

A reader of `NeonRenderer.Ring` (and so of the glow beside it) sends the ring
through the atlas on moving frames too. It is shaded into the atlas (pass 1t)
and then copied to the screen (pass 2c), instead of shaded on the screen.
Current build, against no reader:

| frame type | view 1 | view 0.5 |
| ---------- | ------ | -------- |
| still | 1.03 (0.87-1.42) | 1.00 (0.79-1.25) |
| hue | 1.10 (1.03-1.23) | 1.16 (1.06-1.24) |
| intensity | 1.17 (1.07-1.49) | 1.36 (1.10-1.50) |
| arc-wipe | 1.05 (1.03-1.11) | 1.06 (1.01-1.16) |
| segment-travel | 1.05 (1.03-1.09) | 1.04 (1.00-1.11) |

In time, that is 0.016-0.033 ms a frame. At view 0.5: hue 0.141 -> 0.171 ms,
intensity 0.080 -> 0.113, arc-wipe 0.373 -> 0.391, segment-travel
0.458 -> 0.490.

- The ratios are large only where the frame is small. An intensity frame at
  view 0.5 is 0.08 ms, so a fixed 0.03 ms reads as 1.36x.
- Still frames cost the same as with no reader: the atlas is drawn once
  either way.
- The 0.03 ms is one more render pass (section 6). The atlas is a target of
  its own, so the tiler loads and stores it. On this GPU that is most of
  pass 1t's timer, whatever the ring costs to shade.

---

## 5. A reader of the shared buffer

A reader of `NeonRenderer.Composite` pays the parts' cost plus pass 1s:
clear the shared buffer, draw the fill at its scale, assemble the light over
the light's area.

### 5.1 What it costs

Current build, against no reader, in one run (hue and intensity):

| frame type | shared 0.5, view 0.5 | shared 1, view 0.5 | shared 1, view 1 |
| ---------- | -------------------- | ------------------ | ---------------- |
| hue | 1.42 (1.22-1.61) | 1.53 (1.26-1.76) | 1.58 (1.20-1.84) |
| intensity | 1.82 (1.59-1.98) | 2.06 (1.84-2.25) | 1.85 (1.48-2.19) |

In time:
- hue: 0.143 -> 0.209 and 0.224 ms at view 0.5, and 0.222 -> 0.372 at
  view 1;
- intensity: 0.080 -> 0.147 and 0.174 at view 0.5, and 0.178 -> 0.337 at
  view 1.

On arc-wipe and segment-travel frames the same 0.04-0.19 ms is a smaller
share of a larger frame: 1.12-1.15x for shared 0.5 at view 0.5, and about
1.18x at view 1.

### 5.2 Where it goes

To split pass 1s's fixed cost from its drawing, a scratch switch makes it
clear its buffer and return. The render pass, the clear and the store still
happen; the fill and the light are not drawn. Hue frames, ms:

| | no reader | parts reader | shared buffer, clear only | shared buffer |
| - | --------- | ------------ | ------------------------- | ------------- |
| shared 0.5, view 0.5 | 0.143 | 0.171 | 0.208 | 0.209 |
| shared 1, view 0.5 | 0.143 | 0.171 | 0.214 | 0.224 |
| shared 1, view 1 | 0.222 | 0.238 | 0.304 | 0.372 |

(The parts-reader column is from the run in section 4.)

- **At view 0.5 the reader's cost is two render passes.** Each adds ~0.03 ms:
  the atlas's (0.028), then the shared buffer's (0.037). Drawing into the
  shared buffer adds 0.001-0.010 ms on top.
- **The buffer's size barely matters on the M2.** A cleared 4 MB buffer
  (0.5) and a 17 MB one (1) differ by 0.006 ms. The store of a mostly
  uniform target is cheap on this GPU. Do not assume that elsewhere.
- **At view 1 the drawing shows.** The frame is GPU-bound there, and
  assembling the light at full resolution adds 0.068 ms over the clear.
- Intensity frames split the same way (0.144 of 0.147 ms at shared 0.5,
  view 0.5).

### 5.3 Against the re-shaded buffer it replaced

The plan expected the assembled buffer to be much cheaper than the old one,
which drew the neon a second time into it (`c08639a`). Current build against
`c08639a`, each with the same reader, in one run:

| frame type | shared 1, view 1 | shared 1, view 0.5 | shared 0.5, view 0.5 |
| ---------- | ---------------- | ------------------ | -------------------- |
| hue | 0.95 (0.85-1.06) | 0.92 (0.73-1.14) | 1.10 (0.86-1.31) |
| intensity | 0.96 (0.85-1.06) | 0.94 (0.68-1.25) | 1.16 (0.91-1.41) |
| arc-wipe | 0.97 (0.75-1.04) | 0.88 (0.70-1.00) | 0.98 (0.86-1.15) |

At `29e30af`, before section 9's changes, the same comparison read 0.96-1.04
at shared 1 and up to 1.28 at shared 0.5.

- **At scale 1 the assembled buffer is now the cheaper one, by 3-12%.** That
  is less than the plan hoped. Re-shading the ring was never most of the old
  buffer's cost: removing it altogether saved 3-12% of the frame. The ring is
  a thin band. Section 5.2 shows what is left: the buffer's render pass,
  which both designs pay.
- **At 0.5 it is still 1.10-1.16x dearer on hue and intensity frames** (0.021
  and 0.028 ms). The old buffer shaded its ring straight into itself at 0.5,
  in one render pass. The assembled one needs the ring atlas first, at full
  resolution, in a render pass of its own: the ~0.03 ms of section 4. That
  pass is what makes it right. The old buffer was up to 82 levels off on
  `hairline` at 0.5 because it shaded a thin line at half resolution.
- **A first frame with a shared reader** is 46-47 ms, against 40 with the
  old buffer. It builds the atlas's programs and the assemble pass's, where
  the old one built a second copy of the screen's. The cost lands once, on the
  frame the reader first asks.

---

## 6. Per pass

GPU timer per pass at view 0.5, median over the twelve scenes (ms). The
caveat of section 2 applies: a pass into a target of its own carries that
target's load and store.

| reader, frame | build | P1t | P1s | P2c |
| ------------- | ----- | --- | --- | --- |
| none, arc-wipe | current | - | - | 0.028 |
| parts, hue | current | 0.030 | - | 0.003 |
| parts, intensity | current | 0.034 | - | 0.005 |
| parts, arc-wipe | current | 0.023 | - | 0.001 |
| shared 0.5, hue | `29e30af` / current | 0.029 / 0.029 | 0.031 / **0.013** | 0.004 / 0.006 |
| shared 0.5, intensity | `29e30af` / current | 0.026 / 0.030 | 0.037 / **0.019** | 0.007 / 0.008 |
| shared 0.5, arc-wipe | `29e30af` / current | 0.022 / 0.012 | 0.022 / **0.004** | 0.000 / 0.002 |
| shared 1, hue | `29e30af` / current | 0.031 / 0.027 | 0.056 / **0.045** | 0.001 / 0.002 |
| shared 1, intensity | `29e30af` / current | 0.029 / 0.029 | 0.062 / **0.040** | 0.000 / 0.001 |
| shared 1, arc-wipe | `29e30af` / current | 0.016 / 0.019 | 0.043 / **0.019** | 0.000 / 0.001 |

- **Pass 1t is ~0.03 ms whatever the frame.** On a hue frame the ring is a
  composite of its field, cheap to shade. On an arc-wipe frame it is shaded in
  full. The timer reads the same: it is measuring the render pass, not the
  ring.
- **The arc-wipe rows show the ring's shading moving.** With no reader it is
  0.028 ms in P2c, on the screen. With a reader the same work is in P1t.
- **Pass 1s fell to 0.004-0.019 ms at 0.5 and 0.019-0.045 at 1** with this
  review's two shader changes (section 9).

---

## 7. The first frames

The first five frames of the `default` scene at view 0.5, ms, median of
five. Frames 3-5 are 0.5-1.3 ms in every build.

| build | rotating hue: frame 1 | frame 2 | still config: frame 1 | frame 2 |
| ----- | --------------------- | ------- | --------------------- | ------- |
| `main` | 32.6 | 1.39 | 32.6 | 0.96 |
| `c08639a` | 35.9 | 0.92 | 35.6 | 1.01 |
| `29e30af` | 41.6 / 40.6 | 1.30 / 1.39 | 41.5 / 40.5 | 1.36 / 1.40 |
| atlas programs built only when needed | 36.9 | 1.42 | 36.5 | **6.19** |
| current | **36.0** | 1.29 | 40.2 | 1.40 |

(`29e30af` is shown from both probe runs. The fourth row was this review's
first attempt; see section 9.3.) View 1 is the same picture, each figure
1-2 ms higher.

- **`29e30af` added ~5 ms to every host's first frame**: it built the ring
  atlas's two programs there. Pass 1t's shading is a link of `neon.frag`,
  and Apple's driver compiles the fragment stage's machine code at link.
- **A rotating hue never holds still while the clock runs.** That is the
  default config (`hueRotationRate` 0.5). For such a host the atlas is never
  drawn unless something reads it, so those 5 ms bought nothing.
- **Building the programs only when needed moves the 5 ms, not removes it,
  for a still config.** Its second frame is its first still frame, so that
  frame went from 1.4 to 6.2 ms: a hitch right after the first picture.
- **Current: build them on the first frame of a config whose hue does not
  rotate** (`hueRotationRate == 0`, the frames that can hold still), and
  otherwise on the first frame the atlas is needed. A rotating hue is back to
  the branch's first frame. A still config pays on frame 1, as at `29e30af`,
  with no hitch on frame 2.
- The ~3 ms between `main` and `c08639a` predates this plan.

---

## 8. Memory

GL texture memory after eight frames, from `layer-check` at 1280 x 720 (the
`neon/` and `shared/` scenes), `29e30af` against `c08639a`. Neither change in
this review allocates anything.

| what | when it exists | cost |
| ---- | -------------- | ---- |
| ring atlas | a still config (kept: every still frame copies it), or a ring reader | default scene: +0.64 MB at view 0.5, +0.59 at 1, +0.72 at 0.25 (the ring is wider at a lower scale). Hairline: +0.35-0.66 |
| ring atlas | a moving config nobody reads | 0: never allocated. An atlas left from a still spell is released after `RING_ATLAS_RELEASE_SECONDS` (5 s) |
| shared buffer | while read | default scene: +1.88 MB at 0.5 (640 x 368 texels), +7.37 MB at 1 (1280 x 720). Its region there is the whole viewport |

At 1080p the shared buffer is 4.1 MB at 0.5 and 16.6 MB at 1 when its region
is the whole viewport, and the atlas is about 1 MB. A host that never holds
still and reads nothing gets no new buffer.

---

## 9. What this review changed

All three were gated by `check`, `partition`, `parts`, `shared` and
`layer-check` (section 11).

### 9.1 Pass 1s below scale 1: one glow read away from the ring

Below scale 1, each shared texel averaged `neonLight` over the 2 x 2 (at 0.5)
caller pixels under it. Each read is a ring test, the edge mask and a
bilinear fetch.

Away from the ring, with no edge mask live, every one of those reads is the
glow alone. The glow is a bilinear upsample of a smooth buffer, so the
average of the reads differs from one read at the square's centre only by
the buffer's second difference across a texel. `neon-assemble.frag` now
takes that one read for a square that is:
- not within a square's width of a ring pixel
  (`neonLightSquareTouchesRing` on the square grown by its own width);
- on a config whose edge mask is not live. The mask's ramps are one
  full-resolution pixel wide, and only the per-pixel average is their box
  filter.

**The margin is load-bearing.** The first version tested the square itself.
It read 2 levels off on `hairline` at 0.5, with 99.852% of lit pixels within
1, and failed `shared` (99.9%). Grown by a square's width it reads max 1.

Effect, against `29e30af`, shared 0.5 at view 0.5:
- frames: hue 0.93, intensity 0.90, arc-wipe 0.95, segment-travel 0.95;
- pass 1s: hue 0.031 -> 0.013 ms, intensity 0.037 -> 0.019, arc-wipe
  0.022 -> 0.004;
- unchanged at view 1, where the shared buffer is drawn at 1.

### 9.2 Pass 1s at scale 1: one read, no loop

At scale 1 a texel's square is exactly one pixel, because the region's
origin is a whole pixel. The loop ran once with weight 1, and 9.1's test
ran before it and was never taken. A uniform branch now returns that one
read first.

It is the loop's value: the same pixel centre and a weight of exactly 1.
`layer-check`'s `shared/` frames at 1 are unchanged (section 11).

| frame type | shared 1, view 1 | shared 1, view 0.5 |
| ---------- | ---------------- | ------------------ |
| against `29e30af`: hue | 0.99 | 0.91 (0.85-1.04) |
| intensity | 0.99 | 0.90 (0.83-1.01) |
| arc-wipe | 0.99 | 0.94 (0.92-0.99) |
| against 9.1 alone: hue | 0.97 | 0.88 (0.81-1.00) |
| intensity | 0.98 | 0.86 (0.78-1.00) |
| arc-wipe | 0.98 | 0.92 (0.89-0.99) |

9.1 alone had made scale 1 up to 1.04x dearer than `29e30af` (its untaken
test). Adding 9.2 makes it 0.90-0.94x.

### 9.3 The ring atlas's programs: built where they can be used

See section 7. The rule is in `RenderOffscreen` next to `ringAtlasNeeded`. The
programs are built:
- on the first frame of a config with `hueRotationRate == 0`;
- or on the first frame the atlas is needed.

A failed build still only sends the ring to the screen directly.

---

## 10. What is left, ranked

1. **Readers that can run the neon's GLSL should read the parts, not the
   shared buffer.** The parts cost 0.016-0.033 ms a moving frame, the shared
   buffer 0.04-0.19. [`neon-shared-buffer.md`](neon-shared-buffer.md) says
   so; a host integration guide should too.
2. **Measure on the target tiler.** On the M2 a reader's cost is render
   passes (section 5.2): one for the atlas, one for the shared buffer, ~0.03
   ms each, plus that buffer's store. That is the most GPU-dependent number
   on this page and it has not been measured on Mali / Tizen. Sharing makes
   the neon's phase 1 one or two passes longer. The screen is still drawn in
   one run.
3. **The shared buffer's two passes could become one only by giving up a
   rule.**
   - Pass 1s reads the atlas pass 1t wrote, so they cannot share a target
     without a feedback loop.
   - Drawing the ring into the shared buffer directly would shade it twice
     on moving frames: the re-shaded design, which section 5.3 shows is no
     cheaper.
   - Nothing to do unless the tiler numbers say otherwise.
4. **The shared buffer's format.** RGBA16F at 8 bytes a texel. On the M2 the
   size barely moves the cost (5.2), so RGBA8 would save memory (2-8 MB at
   1080p), not time. It would also band the slow tails, which the buffer
   carries undithered. That is an owner's decision about the contract, not a
   performance fix.
5. **A still config's first frame is ~5 ms dearer than before the plan**
   (40.2 against 35.6 ms). That is the ring atlas's programs, built where a
   still host can use them on its next frame. Using the screen ring's
   programs instead would break the AB rule (93-624x measured on the AMD).
   It stays.

---

## 11. Gates

On the change's final build:

- `neon-scale-check check`: PASS.
- `partition`: PASS. 1000 configs, the ring copied from the ring atlas in
  477, 0 px overlapping, 0 px of seam gap.
- `parts`: PASS, max 1.
- `shared`: PASS, max 1, and 100% of lit pixels within 1 in every scene at
  0.5, `hairline` included.
- `layer-check diff` against `29e30af`'s report: 7 of 300 frames differ. All
  of them are the views of the shared buffer at 0.5
  (`shared/default@0.5+view`, `shared/default@0.5+step-to-1+view`), which
  9.1 changes within `shared`'s bound. Every frame drawn without a reader,
  and every view of the buffer at 1, is identical.
- `layer-check startup` on the default config (a rotating hue): 22 compiles
  and 19 links, against 23 and 21 at `29e30af`. The ring atlas's shading link
  and its copy are no longer built at startup.
