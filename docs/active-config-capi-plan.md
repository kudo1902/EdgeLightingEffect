# Read the active config from the C ABI

**Status: done.** All five parts landed. This is kept as the record of what the
change involved and what was verified about it.

It closed the one gap the animation C ABI left open: a C host could attach and
play an animation, and could not observe a single value it produced.

The shape, in one line: **one source-parameterised read family, not a parallel
set of `_active_` getters.** Twelve new exports carrying an
`el_config_source_e`, addressed by the enums the animation API already uses.
Purely additive - no existing signature changes, so no host is forced to
recompile.

## What landed, and where it deviated

Twelve exports, as planned: seven field readers plus `el_effect_read_count`
over `el_container_e`, and the `el_config_source_e` / `el_container_e` enums
they take. `demo-capi/` compiled unchanged against the new header before Part 4
was written, which is the no-ABI-regression check.

Deviations from the plan as written, all deliberate:

- **`ReadField` returns `bool` like every other reader**, not a total `float`.
  The plan kept it total so `CaptureBaseline` stayed a one-liner; uniformity
  won and `CaptureBaseline` costs two extra lines. The payoff is that an
  unknown enum value arriving from C has a failure signal on *every* reader
  rather than all but one.
- **Readers do not log, writers do.** A writer whose slot was released under it
  cannot fix itself, so it says so; a reader probing a container the host has
  not sized yet is ordinary. The C layer calls these once per field per frame,
  so a logging reader would be a per-frame error stream in normal use.
- **Negative indices are rejected explicitly** rather than wrapped into a huge
  `size_t` and left to fail the bounds check. Same outcome, but the log says
  what happened.
- **Part 1 also annotated `el_animation_reset`**, which writes staging and
  carries the same persistence trap as `el_animation_apply` two lines below it.
- **Part 4 removed `demo-capi`'s plain `slider` lambda**, dead once all six of
  its call sites moved to the animated one. Clang does not warn about an unused
  lambda object.
- **The internal helper is `ResolveConfigSource`, not `resolveConfigSource`.**
  It first shipped matching its eight `camelCase` neighbours in
  `capi-internal.h`; the tree's convention for free functions is verb-first
  `PascalCase`, and those neighbours turned out to be unaudited violations
  rather than a local style. All nine were renamed, plus `IsValidIndex` in
  `el-effect.cpp`. Recorded as N6 in `naming-review.md`; the export table is
  untouched.

Two things found while doing it, neither a defect but both worth knowing:

- **`neon.arcs` defaults to one entry (`{Arc{}}`) while `segmentBoosts`
  defaults to none.** So a fresh handle reads 1 for `EL_CONTAINER_ARCS` and 0
  for `EL_CONTAINER_SEGMENTS`. The first verification run asserted both were
  empty and failed on the arcs.
- **`libedge-lighting-c.dylib` owns and exports the GLAD instance**
  (`_gladLoadGLLoader`, `glad_gl*` as external common symbols). A host that
  links its own copy of `glad.c` alongside the dylib gets a second, private,
  never-initialised instance and segfaults on a null function pointer inside
  `el_effect_init`, with no diagnostic. `demo-capi/` is linked such that the
  two coalesce, so nothing in-tree hits it. Undocumented in the header; worth a
  line there, which this change did not add.

## Context

### The three configs

An effect handle reaches three distinct `Config` objects. They are not three
views of one thing; they answer three different questions.

```
el_effect_handle_impl              EdgeLightingEffect
|-- config      = STAGING          |-- mBaseConfig    = BASE
`-- impl -------------------->     |-- mActiveConfig  = ACTIVE
                                   `-- mScratchConfig   (build buffer)
```

| | Question it answers | Written by | Read by |
| --- | --- | --- | --- |
| staging | what has the host typed, not yet submitted? | the 65 `el_effect_set_*` | the 67 `el_effect_get_*` |
| base | what did the host last author? | `SetConfig` | `GetConfig()` |
| active | what is actually on screen? | `refreshActiveConfig()` | every renderer |

Note the split: **base and active are a library concept, staging is a C ABI
concept only.** A C++ host calling `SetConfig` writes base directly and has no
staging at all, which is why `demo/` and `demo-capi/` behave differently in the
one place this plan is about.

Each transition has exactly one call site:

- staging -> base: `SetConfig(effect->config)` in `el_effect_update`
  (`lib/capi/el-effect.cpp:1830`)
- base -> active: `refreshActiveConfig()`, which copies base and applies every
  attached animation's overlay (`lib/src/core/edge-lighting.cpp:111`)
- base -> staging: `el_effect_capture` (`lib/capi/el-effect.cpp:1815`), the
  manual re-sync

Staging and base differ by *timing* only. A setter writes staging and returns;
`SET_AND_LOG` (`lib/capi/capi-internal.h:151`) does not call `SetConfig`. They
diverge from the moment a setter runs until the next `el_effect_update`.

Base and active differ by *derivation*. Active is never authored. It is
discarded and rebuilt from base every frame. It can also be structurally
larger than base, which matters below.

### The gap

All 67 `el_effect_get_*` read staging. Nothing in `lib/capi/` calls
`GetActiveConfig()`. A C host has no way to read what is on screen.

This is not theoretical. `el_effect_attach_animation` and the whole of
`el-animation.h` are exported, including `FieldBoundAnimation` binding. So the
ABI can start an animation and cannot report its output.

`demo/` shows what that costs. `DebugUI::Build` reads `effect.GetActiveConfig()`
and feeds `AnimatedSlider` (`demo/src/debug-ui.cpp:446`), so a slider knob
tracks the animation while idle and edits the base while dragged.
`demo-capi/` has no equivalent, because it cannot. That is part of the drift
recorded as I8 in `review-findings.md`.

### Three constraints on any fix

1. **Active values must never reach staging.** Staging is pushed through
   `SetConfig` on every `el_effect_update`. Anything written into staging
   becomes authored base on the next frame. Writing active there would bake
   overlays into the base permanently.
2. **Animations grow vectors.** `SegmentTravel` and `SegmentBounce` auto-grow
   `segmentBoosts` (`lib/include/animation/neon-animations.h:346`) and
   `ArcWipe` pushes `arcs[0]` (`:493`). Active can hold entries base does not,
   so scalar readers alone are not enough: the counts have to be readable per
   source too.
3. **The field resolver is private.** `readScalar`
   (`lib/src/animation/field-bound-animation.cpp:108`) does exactly the lookup
   this needs but sits in an anonymous namespace. There is no indexed read
   helper at all - `CaptureBaseline` snapshots whole vectors instead.

## The shape

### Why a source parameter

The alternative was a parallel `el_effect_get_active_*` family. A source
parameter is better on two counts. It is one read path rather than two that
must be kept in step, and it makes the staging/base distinction - which exists
today and is invisible from C - addressable rather than implicit.

### Why three values, not two

A two-value BASE/ACTIVE enum forces a choice about which storage "base" names,
and whichever is chosen the other becomes unreachable except through
`el_effect_capture`, which overwrites pending staging edits to get there. The
third value costs nothing and names the storage hosts already depend on.

```c
typedef enum el_config_source_e
{
    EL_CONFIG_SOURCE_STAGING = 0, /**< Pending edits; what el_effect_get_* read. */
    EL_CONFIG_SOURCE_BASE    = 1, /**< Last committed authored values. */
    EL_CONFIG_SOURCE_ACTIVE  = 2  /**< Base + animation overlays; what renderers draw. */
} el_config_source_e;
```

### Why the existing 67 getters are left alone

Retrofitting the parameter onto `el_effect_get_intensity(effect, float*)` keeps
the symbol name and changes the arity. A C++ host gets a compile error. A
P-Invoke / ctypes / cgo host - the entire reason this ABI exists - links fine
and corrupts its stack. That is the same failure mode the renumbering of
`el_renderer_flags_e` earned a warning paragraph for in `el-types.h:290`, and
it is a bad trade across 67 functions for a feature roughly 20 fields need.
The 65 setters would also stay two-argument, leaving get/set signatures
asymmetric.

### `get` versus `read`

The new family is named `el_effect_read_*`, not `el_effect_get_*`. The rule,
which belongs in the `el-effect.h` file block:

- **`get`** - reads staging, one function per named scalar. The existing
  surface. Unchanged.
- **`read`** - reads any source, addressed by field enum.

This also avoids a collision: `el_effect_get_segment_boost_count` already
exists and reads staging, so the source-parameterised count needs its own name
regardless.

Equivalence to document: `el_effect_get_intensity(e, &v)` is
`el_effect_read_field(e, EL_CONFIG_SOURCE_STAGING, EL_FIELD_NEON_INTENSITY, &v)`.

## Part 1 - Doc corrections (independent of everything else)

Two header comments are wrong today, and both describe exactly the distinction
this plan makes addressable. Fix them first; neither depends on any code below.

- `lib/capi/el-effect.h:20-21` says every setter "immediately calls `SetConfig`
  on the underlying effect". It does not. `SET_AND_LOG` writes staging and
  returns; `el_effect_update` is the only `SetConfig` call site. `CLAUDE.md`
  and `docs/implementation.md:235` already describe this correctly.
- `lib/capi/el-animation.h:135` says `el_animation_apply` is "called
  automatically by `el_effect_update` for attached animations". It is not.
  `el_effect_update` calls `impl->Update(dt)`, which applies attached
  animations into `mScratchConfig` and thence active, leaving base clean.
  Manual `el_animation_apply` writes into **staging**
  (`lib/capi/el-animation.cpp:403`), which the next `el_effect_update` commits
  into base, baking the overlay in permanently. Two composition paths with
  opposite persistence semantics, currently described as one.

## Part 2 - Promote the field resolvers (`lib/`)

New `lib/include/animation/field-access.h` + `lib/src/animation/field-access.cpp`.

Move `writeScalar` / `writeSegment` / `writeArc` / `writeArcStop` and
`readScalar` out of the anonymous namespace in
`lib/src/animation/field-bound-animation.cpp` and into the new pair, renamed to
the tree's free-helper convention (verb-first `PascalCase`, as
`GetClampedResolutionScale` established):

```cpp
float ReadField(const Config &cfg, AnimatableField field);
float ReadSegmentField(const Config &cfg, size_t index, SegmentField field);
float ReadPreservedSegmentField(const Config &cfg, uint32_t id, SegmentField field);
float ReadArcField(const Config &cfg, size_t index, ArcField field);
float ReadSegmentStopField(const Config &cfg, size_t segIdx, size_t stopIdx, ColorStopField field);
float ReadPreservedSegmentStopField(const Config &cfg, uint32_t id, size_t stopIdx, ColorStopField field);
float ReadArcStopField(const Config &cfg, size_t arcIdx, size_t stopIdx, ColorStopField field);
// plus the existing Write* family, moved as-is
```

The six indexed readers are new code. Mirror the bounds handling of the
matching `write*` function exactly - those switches are the authority on what
an out-of-range index does, and a reader that disagrees with its writer is a
bug waiting for an animation to auto-grow a vector. Each reader needs an
out-of-range answer for the C layer to turn into `EL_ERROR_INVALID_PARAMETER`;
returning a `bool` with the value through an out-parameter is cleaner than a
sentinel float.

`field-access.h` includes `animation/field-bound-animation.h` for the four
field enums and `core/config.h` for `Config`.
`field-bound-animation.cpp` then includes `field-access.h` and drops its
anonymous namespace, so there is one switch per field family rather than two.
If the include direction grates later, the enums can move to their own header
with `field-bound-animation.h` including it - zero churn for existing
includers. Not worth doing pre-emptively.

This part is useful on its own: it gives the C++ side the same field-addressed
read the C side is getting.

## Part 3 - C ABI surface (`lib/capi/`)

`el-types.h`: add `el_config_source_e` as above.

`capi-internal.h`: a `toConfigSource` helper resolving a handle plus a source
to `const EdgeLighting::Config &`:

```
EL_CONFIG_SOURCE_STAGING -> effect->config
EL_CONFIG_SOURCE_BASE    -> effect->impl->GetConfig()
EL_CONFIG_SOURCE_ACTIVE  -> effect->impl->GetActiveConfig()
```

An unknown source value is `EL_ERROR_INVALID_PARAMETER`. This is the only place
that knows the mapping, so every reader below is three lines over it.

`el-effect.h` / `el-effect.cpp`: seven field readers, arities differing because
the addressing genuinely differs.

```c
el_effect_read_field(effect, source, el_config_field_e, float *out);
el_effect_read_segment_field(effect, source, int32_t index, el_segment_field_e, float *out);
el_effect_read_preserved_segment_field(effect, source, uint32_t id, el_segment_field_e, float *out);
el_effect_read_arc_field(effect, source, int32_t index, el_arc_field_e, float *out);
el_effect_read_segment_stop_field(effect, source, int32_t segIndex, int32_t stopIndex, el_color_stop_field_e, float *out);
el_effect_read_preserved_segment_stop_field(effect, source, uint32_t id, int32_t stopIndex, el_color_stop_field_e, float *out);
el_effect_read_arc_stop_field(effect, source, int32_t arcIndex, int32_t stopIndex, el_color_stop_field_e, float *out);
```

Preserved segments are addressed by stable `uint32_t id`, everything else by
index. That asymmetry is not this plan's - it mirrors
`el_animation_add_preserved_segment_field`, and the whole point of the
preserved pool is that ids survive what indices do not.

Then **one** count reader rather than six, because unlike the field readers
these are homogeneous: all return `int32_t`, all differ only in which container
is being measured.

```c
typedef enum el_container_e
{
    EL_CONTAINER_SEGMENTS = 0,
    EL_CONTAINER_PRESERVED_SEGMENTS = 1,
    EL_CONTAINER_ARCS = 2,
    EL_CONTAINER_SEGMENT_STOPS = 3,          /* parent = segment index */
    EL_CONTAINER_PRESERVED_SEGMENT_STOPS = 4,/* parent = preserved id */
    EL_CONTAINER_ARC_STOPS = 5               /* parent = arc index */
} el_container_e;

el_effect_read_count(effect, source, el_container_e container, uint32_t parent, int32_t *out);
```

`parent` is ignored for the three top-level containers. Twelve exports total.

Conventions this has to honour: out-of-range index or id is
`EL_ERROR_INVALID_PARAMETER`, matching the existing indexed getters; `out*` is
always validated non-null; no C++ exception escapes; `LOG_D` on the read path,
not `LOG_I`, since a UI will call these every frame. No existing `EL_API` is
reordered or renumbered, and no mirrored enum shifts, so the `static_assert`
wall in `capi-internal.h` needs entries for the two new enums only if a C++
mirror is added for them - `el_config_source_e` and `el_container_e` are ABI
concepts with no C++ counterpart, so they get none.

**Reads are live, with no snapshot call.** No second `Config` in the handle, no
state to forget to refresh. Both demos build their UI before `el_effect_update`,
so a live read already yields the previous frame's composited values, which is
exactly what `demo/` reads today. The one caveat, which belongs in the header:
interleaving reads with `el_effect_update` returns values from different frames.

Passing `EL_CONFIG_SOURCE_ACTIVE` for a field nothing animates returns the base
value rather than an error. That is correct - `mActiveConfig` is a complete
`Config` - and it is why the parameter is meaningful for every field uniformly.

## Part 4 - Prove it through `demo-capi/`

Port `AnimatedSlider` from `demo/src/debug-ui.cpp:120` into
`demo-capi/src/debug-ui.cpp`, reading `EL_CONFIG_SOURCE_ACTIVE` for the knob
and writing the setter for the edit, exactly as `demo/` reads active and writes
`cfg`.

This is the part that matters. `demo-capi/` exists to prove the ABI is
self-sufficient for a real UI-shaped host, its include path deliberately
excludes `lib/include/`, and this is currently the largest thing it cannot do.
If the port needs anything not in Part 3, Part 3 is wrong. Closes part of I8.

## Part 5 - Docs

- `el-effect.h` file block: the `get` versus `read` rule, the three sources and
  what each means, the cross-frame caveat, and the equivalence line.
- `docs/implementation.md:235`: the staging paragraph gains the read path.
- `docs/architecture-design.md:539`: the note that sliders read
  `GetActiveConfig` gains its C equivalent.
- `CLAUDE.md`: one line in the doc list for this file, and the C ABI section's
  staging paragraph gains a sentence on the read family.
- The `animation-manager-capi-deferred` memory was stale: it said the manager
  and attach surface were C++-only. `el_effect_attach_animation` and friends
  had already shipped. Corrected.

## Verification

No test target exists, so this was build plus three assertion programs, the
same way the unification's ABI shims were verified. All three pass; none is
checked in, since there is nowhere for them to live yet.

**1. Both demos, the static lib and the dylib build clean**, ASCII-only, no
em-dashes:

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

**2. The move in Part 2 changed no behaviour.** A structural diff against the
pre-change tree: all 18 field-to-leaf mappings identical in both `WriteField`
and `ReadField`, read and write agreeing with each other on all 18, and all six
moved write bodies byte-identical modulo the rename. Then 36 runtime assertions
against the static lib - scalar round-trips, all 18 leaves distinct, an unknown
enum leaving `out` untouched, and every reader refusing exactly where its writer
refuses (empty pools, one past the end, released preserved ids).

**3. C-only against the dylib, no GL** (34 assertions), including
`edge-lighting-capi.h` and nothing from `lib/include/` - the same guard
`demo-capi/` is. `get_*` and `read_*(STAGING)` return identical values; null
handle gives `EL_ERROR_INVALID_HANDLE`; null out, unknown source, unknown field,
unknown container and negative index each give `EL_ERROR_INVALID_PARAMETER` with
`out` untouched; container counts track the setters. Also confirms STAGING is
readable before `el_effect_init` while BASE and ACTIVE are refused, rather than
crashing.

**4. C-only with a hidden GLFW context** - the assertions that need a live
effect, and the ones that actually prove the feature:

- Set intensity, and before `el_effect_update` STAGING alone has the new value;
  after it, all three sources agree.
- With an `IntensityPulse` attached and playing, BASE holds at 2.0 across 60
  frames while ACTIVE varies by more than 0.5. Detach, run one update, and
  ACTIVE returns to BASE - the revert-to-base contract, checkable from C for
  the first time.
- With a `SegmentTravel` attached and `segmentBoosts` empty in base,
  `EL_CONTAINER_SEGMENTS` reads 0 for BASE and non-zero for ACTIVE, and index 0
  reads from ACTIVE while being refused on BASE. This is the assertion that
  fails if the count reader is ever dropped as redundant.
- `el_effect_capture` overwrites the pending staging edit with BASE and leaves
  ACTIVE alone.

**5. The Part 4 port.** `demo-capi/` compiles against the C ABI alone - its
include path is still only `lib/capi` and the externals, and the file contains
no `EdgeLighting::` - and runs for seconds with zero `[ERROR]` lines and no
per-frame read failures. A headless model of the widget's exact read path
against a live pulse then checks the two properties it depends on: idle, the
knob varies while the authored value holds across 90 frames; dragging, the knob
pins to base, a drag's write becomes the new authored value, and the overlay
still applies on top of it.

**6. No ABI regression.** `demo-capi/` compiled unchanged against the new header
before Part 4 touched it. Had it not, something in Part 3 would have altered an
existing signature.

## Known scope limit

This covers the animatable scalar leaves and the container counts around them,
which is what `el_config_field_e` and its siblings enumerate. Non-scalar
animated state stays unreadable from C: a colour-stop vector being *resized* by
an animation, or a `blendSpace` changing under one.

No shipped animation writes either, and covering them means either count plus
index readers per container - which is what Part 3 does for stops already, so
the extension is small - or the bulk struct copy-out that was rejected for
being ABI-fragile. Worth deferring until something needs it. The limit is noted
in the header rather than built for.

Part 4 turned that limit up as a concrete instance. `demo/`'s `CutoffRow` reads
`active.neon.insideCutoff` / `outsideCutoff`, and it could not be ported:
`Cutoff::size` and `softness` are not animatable fields, so no field enum
addresses them and there is no ABI path to their active values. The two demos
are therefore at parity on the six animated neon sliders and still differ on the
two cutoff rows, whose capi versions stay plain sliders. If something ever needs
them animated, they need entries in `AnimatableField` and `el_config_field_e`
first, at which point this read family covers them with no new exports - which
is the argument for the field-addressed shape restated.
