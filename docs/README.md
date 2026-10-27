# Documentation

Everything here describes the library under [`lib/`](../lib/). When a document
and a header under `lib/include/` disagree, the header wins.

The folders sort the documents by what they are for:

| folder | holds | read it when |
| ------ | ----- | ------------ |
| [`guides/`](guides/) | how the library works today, and what every field does | you use, extend or upgrade it |
| [`pages/`](pages/) | HTML pages with diagrams and renders - open them in a browser | you want the picture before the prose |
| [`design/`](design/) | plans that have been built (or are proposed, and say so), and the measured studies behind specific decisions | you are about to change the code they describe |
| [`perf/`](perf/) | the lens flare's performance review, with its measurements | you are about to change the flare for speed |
| [`records/`](records/) | the progress log, the review findings and the naming audit | you want to know where the work stands or why something is the way it is |
| [`images/`](images/) | every figure, shared by all of the above; written by `tools/` | - |

The plans in `design/` are **historical**: each one says at its top that it has
been built, and what it describes may have moved since. They are
kept for the reasoning and the measurements, not as instructions. A plan not
built yet, or built in part, says so at its top instead.

## Start here

1. [`guides/implementation.md`](guides/implementation.md) - how the library is
   put together on the C++ side, and how a frame runs.
2. [`pages/neon-renderer-reference.html`](pages/neon-renderer-reference.html) -
   the neon renderer's whole mechanism, with flow charts of a frame.
3. [`records/progress-log.md`](records/progress-log.md) - dated entries, newest
   first: what changed, what was measured, what is open.

New to graphics? Read
[`guides/neon-onboarding-guide.md`](guides/neon-onboarding-guide.md) first: it
covers the GPU concepts the neon relies on, then every config field, GL object,
pass and shader stage, with renders from the library itself.

## guides/

| document | what it is |
| -------- | ---------- |
| [`implementation.md`](guides/implementation.md) | The library's structure and the per-frame flow. Start here. |
| [`neon-onboarding-guide.md`](guides/neon-onboarding-guide.md) | The long on-ramp: GPU concepts, then the neon field by field, pass by pass, shader by shader. Ends with pitfalls, recipes and exercises. |
| [`effect-reference.md`](guides/effect-reference.md) | What each neon parameter does to the picture, with recipes. |
| [`config-reference.md`](guides/config-reference.md) | Every field of `lib/include/core/config.h`: type, unit, default, range. The lookup table; `effect-reference.md` is the explanation. |
| [`spotlight-renderer.md`](guides/spotlight-renderer.md) | The spotlight layer: every `SpotLight` field, the app-coordinate convention, the measured cost of its resolution scale. |
| [`upgrade-notes.md`](guides/upgrade-notes.md) | What a host moving from `main` has to change, silent changes first. Read before calling an upgrade drop-in. |

## pages/

| page | what it is |
| ---- | ---------- |
| [`cpp-design.html`](pages/cpp-design.html) | The C++ side's design and implementation: modules, ownership, one frame through the effect, the renderer contract, change detection, `NeonRenderer`'s anatomy and pass schedule, its skip keys, shared buffers, the GL wrappers and the C ABI. Read before changing the C++. |
| [`neon-renderer-reference.html`](pages/neon-renderer-reference.html) | Full mechanism reference: every uniform, constant, derivation and gating rule, with flow charts of a frame. Read before changing a shader. |
| [`neon-shader-outputs.html`](pages/neon-shader-outputs.html) | One frame at scale 0.5, pass by pass: what each neon program wrote, and why each is its own program. |
| [`neon-resolution-scale-comparison.html`](pages/neon-resolution-scale-comparison.html) | Twelve scenes at six resolution scales against their 1.0 renders. Its images are what `neon-scale-check check` diffs against. |

## design/

Built plans:

| document | what it is |
| -------- | ---------- |
| [`neon-unification-plan.md`](design/neon-unification-plan.md) | How the half-res neon fork and the flare fork were folded into one renderer each, and how the merges were verified byte for byte. |
| [`neon-resolution-scale-plan.md`](design/neon-resolution-scale-plan.md) | The edge ring: why the reduced-resolution neon re-shades a thin ring at full resolution, built in six steps. |
| [`neon-glow-cover-resolution-plan.md`](design/neon-glow-cover-resolution-plan.md) | The glow coverage table sized per light type, and what it measured. |
| [`neon-shader-cleanup-plan.md`](design/neon-shader-cleanup-plan.md) | The neon's program slots and shader variants before the cleanup, and why each was kept or cut. |
| [`spotlight-renderer-plan.md`](design/spotlight-renderer-plan.md) | The spotlight's design and the offscreen verification behind it. |
| [`render-target-format-check-plan.md`](design/render-target-format-check-plan.md) | Asking the driver which render-target formats it can draw into before allocating: a probe, cached per effect in one `RenderTargetFormats` that the startup report, every format walk and the half-float fields ask, and `neon-scale-check formats`, which refuses formats so the 8-bit fallbacks run. Built but for step 6 (the device); section 10 is what was measured. |

Studies of specific mechanisms:

| document | what it is |
| -------- | ---------- |
| [`emission-prepass.md`](design/emission-prepass.md) | The per-sample emission table: why it exists, what it is keyed on, and its before / after. |
| [`neon-frag-notes.md`](design/neon-frag-notes.md) | The measured history behind `neon.frag`'s lines, one section per block in shader order. |
| [`corner-crease-and-filament-nyquist.md`](design/corner-crease-and-filament-nyquist.md) | The analytic emission's corner and filament defects and their fixes. Read before touching the halo / bloom or filament blocks. |
| [`glow-side-comparison.md`](design/glow-side-comparison.md) | Edges as coverage on the graded output, and the one-sided cut moved into the blit. |
| [`glow-inner-reach.md`](design/glow-inner-reach.md) | The glow quad's interior hole with no cutoff set, and the proof it changes no pixel. |

## perf/

| document | what it is |
| -------- | ---------- |
| [`lens-flare-perf-review.md`](perf/lens-flare-perf-review.md) | Why the flare was the most expensive layer, and the five changes that halved it. Read before touching `lens-flare.frag`'s ghost loop. |

What the neon costs and which knobs move it is in section 3.9 of
[`guides/effect-reference.md`](guides/effect-reference.md); how to measure it
is the `time` section of
[`tools/neon-scale-check/README.md`](../tools/neon-scale-check/README.md).

## records/

| document | what it is |
| -------- | ---------- |
| [`progress-log.md`](records/progress-log.md) | Dated entries, newest first. |
| [`review-findings.md`](records/review-findings.md) | The open defects and rough edges in full, with repros, the state table, and a one-line index of every item ever found (the closed items' full text is in git - see Removed below). Check here before assuming a behaviour is intended. |
| [`naming-review.md`](records/naming-review.md) | Identifier audit against `AGENTS.md`. Read before renaming anything. |

## Adding a document

Put it in the folder its purpose names, link it from this page, and link
images as `../images/<topic>/...`. Run the link check the restructure used
(every relative link and every `docs/...` path in the tree must resolve) after
moving or renaming anything here.

## Removed

On 2026-10-20 these were deleted, and the docs and comments that cite them still
name them: the neon performance reviews and plans `neon-perf-review.md`,
`neon-perf-plan.md`, `neon-reduced-scale-plan.md`,
`neon-animation-perf-analysis.md` and `neon-resolution-scale-perf-comparison.md`
(measurements from 2026-09-07 to 2026-10-09, all predating the shared buffer),
the two shorter HTML retellings `neon-renderer-overview.html` and
`neon-renderer-explained.html`, and the closed review items with their 26
figures. Any of them is one command away:

```bash
git show f153e1c:docs/neon-perf-review.md
```

(`f153e1c` predates the folders, so every name sits directly under `docs/`, and
`f153e1c:docs/review-findings.md` is the whole findings document, closed items
included.)
