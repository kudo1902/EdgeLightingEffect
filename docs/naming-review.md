# Naming review

A pass over every identifier in `lib/`, `demo/` and `demo-capi/` against
[`AGENTS.md`](../AGENTS.md), plus a semantic pass for names that mislead or that
pair inconsistently with each other.

Mechanical conformance is high enough that the interesting findings are all in
the second half. The rule violations below are five small ones; the semantic
issues are where the cost actually is, because a name that describes a mechanism
the code no longer has will send the next reader down the wrong path.

**Status:** N3, S3 and S8 are fixed, along with the `AGENTS.md` gap. S1 was applied
and then reverted by decision - see there. `demo/`, `demo-capi/` and the C ABI
were explicitly left out of that pass, so N1, N2, N4 and N5 stand open by scope
rather than by judgement. N6 was found and fixed later, outside the original
pass, and added the free-function rule to `AGENTS.md`; N7 is the tail it left
open.

## Conformance summary

Checked by script over 47 headers and 21 translation units (`stb-image`
excluded as vendored).

| rule | result |
| ---- | ------ |
| files `kebab-case` | clean |
| header guards present, `#define` matching, stem correct | clean |
| enum values `ALL_CAPS` | clean |
| public methods `PascalCase`, private `camelCase` | clean |
| member `mFoo`, global `gFoo` | 2 violations (N1, `demo-capi` only) |
| struct / enum `typedef` self-alias | 2 violations (N2, `demo-capi` only) |
| separate `private:` sections for methods and members | 2 violations (N3) |
| C ABI: 212 `el_*` functions, 16 `_e` enums, 3 `_handle_t`, 99 `EL_*` | clean |

The C ABI is worth calling out: 330 exported identifiers, zero deviations. It
is the most consistently named surface in the project.

---

## Rule violations

### N1. `demo-capi` uniform-location members use the GLSL `u` prefix

```cpp
// demo-capi/src/background-quad.h:78
GLuint mProg = 0;
GLuint mVao  = 0;
GLuint mVbo  = 0;
GLint  uCheckerSize = -1, uColorA = -1, uColorB = -1;   // <- members, not uniforms
```

Same in [`demo-capi/src/image-quad.h`](../demo-capi/src/image-quad.h) with
`uViewport`, `uRect`, `uTex`. They sit in the same `private:` block as
correctly-prefixed members, so the class contradicts itself.

The `u` prefix is the convention for *GLSL uniforms*; these are the host-side
`glGetUniformLocation` results. `mCheckerSizeLoc` / `mViewportLoc` says both
things - it is a member, and it holds a location rather than a value.

`demo/` does not have this problem: it goes through
`EdgeLighting::ShaderProgram`, which caches locations internally.
`demo-capi/` cannot see `lib/include/`, so it hand-rolls them - the cost of the
fork being a fork (see I8 in [`review-findings.md`](review-findings.md)).

### N2. Two structs missing the `typedef` self-alias

- `AnimEntry` in [`demo-capi/src/debug-ui.h`](../demo-capi/src/debug-ui.h)
- `SampledStop` in [`demo-capi/src/border-color-picker.h`](../demo-capi/src/border-color-picker.h)

Both are local helper structs, so nothing breaks; they are simply the only two
in the tree that skip the alias `AGENTS.md` calls mandatory.

### N3. `private:` sections mix methods and members - FIXED

`AGENTS.md` is explicit that a class gets two `private:` labels, methods first,
then member variables.

A first scan reported nine classes. That number was wrong: the detector matched
assignments like `mFbo = 0;` **inside** a method body as if they were member
declarations, so `Framebuffer`, `Animation` and five of the animation subclasses
were false positives - all of them already correct. Re-run with brace tracking,
`lib/` has exactly two:

- `EdgeLightingEffect` ([`edge-lighting.h`](../lib/include/core/edge-lighting.h))
- `ArcWipe` ([`neon-animations.h`](../lib/include/animation/neon-animations.h))

Worth noting against myself twice over: I added `mInitialized` to the
`EdgeLightingEffect` block during the I6 fix and propagated the violation, and
then over-reported the scale of it here. Both are now split; `DebugUI` is the
only remaining case and lives in `demo/`.

### N4. C ABI header guards drop the project prefix

| file | guard |
| ---- | ----- |
| `lib/include/**/*.h` (34 files) | `_EDGE_LIGHTING_<STEM>_H_` |
| `demo/src/*.h` (6) | `_EDGE_LIGHTING_DEMO_<STEM>_H_` |
| `demo-capi/src/*.h` (7) | `_EDGE_LIGHTING_CAPI_DEMO_<STEM>_H_` |
| `lib/capi/*.h` (6) | `_EL_EFFECT_H_`, `_EL_TYPES_H_`, `_CAPI_INTERNAL_H_`, ... |

These are the **public installed headers**, so they are the ones most exposed to
a collision in a consumer's translation unit, and `_CAPI_INTERNAL_H_` in
particular is generic enough to hit one. `_EL_EFFECT_H_` would be safer as
`_EDGE_LIGHTING_CAPI_EL_EFFECT_H_`, or at least `_EL_CAPI_EFFECT_H_`.

Separately, and project-wide rather than a `capi` issue: an identifier that
begins with an underscore followed by a capital letter is reserved to the
implementation in both C and C++. Every guard in the project is technically in
reserved space. In practice no toolchain collides here, and changing it is a
47-file sweep, so this is recorded as a standards nit rather than a
recommendation.

### N6. `capi-internal.h`'s helpers were `camelCase` - FIXED

The conformance table's C ABI row counts the 212 exported `el_*` functions and
the rest of the `EL_*` surface. It does not reach the *internal* helpers in
[`capi-internal.h`](../lib/capi/capi-internal.h), which are ordinary C++ free
functions in no namespace and were never audited by either pass.

All eight of them were `camelCase` - `mapExceptionToResult`, `toEasing`,
`toWaveform`, `toEndAction`, `fromEndAction`, `toPlaybackMode`,
`fromPlaybackMode`, `toAnimatableField` - against a tree that uses verb-first
`PascalCase` everywhere else a free function appears: `AcquireSegment` /
`FindPreservedSegment` / `ReleaseSegment` in `segment-utils.h`, the
`ReadField` / `WriteField` family in `field-access.h`, and
`GetClampedResolutionScale` in the neon renderer, which was renamed *into* that
convention by the unification (see `neon-unification-plan.md`).

`AGENTS.md` was the reason this survived: its Functions / Methods section covered
public, private and protected *methods* and said nothing about free functions,
so there was no rule to check them against.

Fixed by renaming all nine (the eight above plus `ResolveConfigSource`, added
by the active-config work) plus `IsValidIndex` in `el-effect.cpp`, 46 call sites
across four files, and the eight anonymous-namespace helpers in
`field-access.cpp` that the same work moved out of `field-bound-animation.cpp`.
Purely internal: the export table is unchanged at 238 symbols, none of these
among them, so no host is affected.

`AGENTS.md` now carries the free-function rule, so the gap is closed and the
next pass can check this class mechanically.

The rule went in three times, each pass catching what the last one's wording let
through.

1. **`PascalCase` only.** The rename obeyed the case and left three noun-phrase
   names - `SegmentSlot`, `ColorStopSlot`, `ArcSlot` - which read at a call site
   as objects rather than calls. Now `FindSegmentSlot` / `FindColorStopSlot` /
   `FindArcSlot`: `Find` is the tree's verb for a lookup that can fail
   (`FindPreservedSegment`) and the exact contrast with `EnsureSegmentSlot` in
   `neon-animations.h`, which grows to reach the slot.
2. **Verb-first spelled out, conversions exempted.** `To*` / `From*` was blessed
   as an established shape on the strength of `ToByte` in `color-utils.h`.
3. **Exemption withdrawn, and the shape reconsidered.** A conversion has the
   same defect as any other noun phrase: `To*` names the destination type, not
   the operation. A first fix put `Map` in front of all seven, which fixed the
   grammar and left the worse half in place - direction was still relative to
   whichever type the name mentioned, so `MapFromPlaybackMode` could not be read
   without knowing that the named type is always the C++ one.

   The seven collapsed instead into two overload sets, `ConvertFromCapi` and
   `ConvertToCapi`, resolved on the argument type. The axis the whole file turns
   on is named once rather than seven times, and the direction is absolute. Safe
   because every argument type is distinct - unscoped C enums on one side, all
   `enum class` on the other. `Native` was rejected for the pole: at an FFI
   boundary the host would call the C side native, so it points the wrong way.

   `MapExceptionToResult` keeps `Map` and stays out of the overload set: many
   exception types onto one error code is a classification with information
   loss, not a change of representation. Predicates (`Is*`, `Has*`) remain the
   only shape that counts as verb-first without a verb.

`ToByte` in [`color-utils.h`](../lib/include/util/color-utils.h) was the last
name left under the old shape, and it went the same way. Not to `ConvertToByte`:
its own doc comment opens "Quantise a [0, 1] colour channel to 8 bits", every
caller describes itself as quantising through it, and the comment's whole
subject is the rounding bias that makes it lossy. `Convert` would have named the
narrowing and dropped the part that matters, so it is **`QuantiseToByte`** - the
verb the surrounding prose was already using for it. Eight call sites across
four headers; the two comments that then read "quantises through
QuantiseToByte" were reworded.

**One correction to an earlier draft of this entry**, which claimed the tree's
practice here was unanimous. It is not, and the rule as written is a decision
rather than a transcription. Header-declared free functions *are* unanimously
`PascalCase` (`segment-utils.h`, `field-access.h`). File-local ones in anonymous
namespaces are split: `GetClampedResolutionScale` / `GetClampedNumSamples`
(`neon-renderer.cpp`) and `IsStripVisible` (`debug-renderer.cpp`) are
`PascalCase`, while the helpers in N7 below are `camelCase`. The rule follows
the `PascalCase` side because that is where the deliberate renames went - the
neon unification moved two helpers *into* it on purpose (see
`neon-unification-plan.md`) - and because linkage is not visible at a call site,
so making the name shape depend on it hides rather than informs.

---

### N5. `#endif` guard comments are inconsistent in `demo-capi`

Five of the seven headers end with a bare `#endif`; `gl-mini.h` and
`ui-controls.h` carry the guard name, as every `lib/` and `demo/` header does.

---

### N7. Seven anonymous-namespace helpers in `util/` are still `camelCase` - OPEN

The free-function rule added to `AGENTS.md` for N6 leaves these behind:

| file | helpers |
| ---- | ------- |
| [`lib/src/util/capture-util.cpp`](../lib/src/util/capture-util.cpp) | `flipRows` |
| [`lib/src/util/contour-tracer.cpp`](../lib/src/util/contour-tracer.cpp) | `resolveSaddle`, `lerpAlpha`, `edgePos`, `chainSegments`, `resampleUniform`, `prevKey` |

All seven are file-local, none is reachable from another translation unit, and
renaming them is a contained sweep of two files with no header or ABI effect.
They are recorded rather than fixed because the change that surfaced them
(the active-config C ABI work) had no reason to be in `util/` at all, and a
rename sweep riding along in an unrelated commit is exactly what makes a diff
hard to review.

Worth doing as its own change. Until then this is the only place the rule and
the code disagree, so a script checking N6's rule will report these seven and
nothing else.

## Semantic issues

Ordered by how misleading the name is, not by churn. Churn is noted per item
because several of these reach the frozen C ABI.

### S1. Three different word orders for "the half-res variant" - RESOLVED

| | neon | lens flare |
| - | ---- | ---------- |
| renderer class | ~~`NeonOptimizedRenderer`~~ | `LensFlareOptimizedRenderer` |
| config struct | ~~`Optimized`**`Neon`**`Config`~~ | `LensFlare`**`Optimized`**`Config` |
| config field | ~~`optimizedNeon`~~ | `optimizedLensFlare` |

The renderers agreed with each other. The config fields agreed with each other.
The config *structs* did not - `OptimizedNeonConfig` was the only one that led
with the qualifier, disagreeing with both its own renderer and its counterpart
struct.

**Resolved by deletion, not by renaming.** The neon column no longer exists:
`NeonOptimizedRenderer` and `OptimizedNeonConfig` were folded into
`NeonRenderer` / `NeonConfig`, where the half-res path is the
`resolutionScale` field rather than a separate type. The three word orders were
a symptom of naming a fork three times; there is no fork to name.

The lens-flare column is untouched and still internally consistent, so nothing
is left to trade off. The C ABI constraint that made the original rename
unattractive also went away in a direction nobody had considered: the
`el_effect_*_optimized_*` functions still exist, frozen as required, but they
are now shims onto `NeonConfig`, so the frozen ABI name no longer pins any C++
identifier.

If the ABI is ever revised (see S2), the `el_effect_*_optimized_*` group is
the remaining piece: the names now describe a renderer that does not exist, and
`el_effect_set_neon_resolution_scale` and friends would say what they actually
do. Not urgent - they work, and they are documented as deprecated in
`el-effect.h`.

### S2. "Optimized" does not say what the optimisation is

Both variants are half-resolution renders blitted back up. "Optimized" is the
kind of qualifier that ages badly: it says a change was made, not what it was,
and it gives no hint that enabling both a renderer and its "optimized" twin
draws the effect twice (which `CLAUDE.md` has to warn about in prose).

`NeonHalfResRenderer` / `halfResNeon` would be self-describing, and the
double-draw hazard would read as obviously redundant at the call site.

**Not recommended now:** the C ABI exports 4 `el_effect_*_optimized_*`
functions plus `EL_RENDERER_*` flags. Worth doing only if the ABI is broken for
another reason.

### S3. `EARLY_OUT_RADIUS_FACTOR` names a mechanism that was removed - FIXED

There is no early-out any more. The per-fragment `discard` it was named for is
gone; the shaders say so themselves:

> `// (Far early-out lives on the CPU: the draw quad is sized to rect + earlyOut...`

The constant now does two things: it sizes the draw quad in `setupGeometry`, and
the shaders recompute the same expression to place the bloom pedestal. The code
around it has already moved on to the right word - `setupGeometry` calls its
local `glowReach`, the shaders call theirs `reach` - and only the constant, plus
the comments that echo it, still say "early out".

**Fixed:** now `GLOW_REACH_RADIUS_FACTOR`, matching the `glowReach` / `reach`
locals it feeds and the `QUAD_FADE_START_FRAC` naming beside it. One definition,
two shader uses, two `.cpp` uses; no ABI impact, since it is a tuning constant.

The comments that echoed the old name went with it - ten sites where `earlyOut`
was used as if it named a live quantity now say `glowReach`. Four uses of
"early-out" survive on purpose, because they describe actual early returns: the
`opaqueOnly` return in `NeonRenderer`, the `bell < 0.005` skip in the pre-pass,
and the two lines in `neon-tuning.h` that explain what the constant used to be
calibrated for.

### S4. `SegmentBoost::boost` also names a removed mechanism

Its own doc comment admits it:

> The `boost` sets its peak brightness (**was a multiplier under the old
> multiplicative model**; now the segment's absolute amplitude in the additive
> compose).

So the field is an absolute peak brightness wearing the name of the multiplier
it replaced, and the type `SegmentBoost` is a travelling coloured light named
after that same multiplier. `peak` or `amplitude` for the field; the type is
really just a segment.

**Not recommended now:** `el_effect_set_segment_boost` and friends are ABI. The
field doc is at least honest about the history, which is why this ranks below
S3 despite being the same category of error.

### S5. `PreservedSegment` and `preservedSegmentBoosts` disagree

| pool | type | config field |
| ---- | ---- | ------------ |
| transient | `SegmentBoost` | `segmentBoosts` |
| preserved | `PreservedSegment` | `preservedSegmentBoosts` |

The type drops "Boost" and the field keeps it, for the two halves of one pool
that `SegmentUtils::FillEffectiveSegments` merges. Whichever way S4 is resolved,
these two should match each other.

### S6. `filamentFalloff` runs the opposite way to its name

```
/// Lower values give a smoother, softer roll-off; higher values sharpen the edge.
float filamentFalloff = 1.0f;
```

A higher "falloff" produces *less* gradual falloff - a flatter top and a sharper
shoulder. The quantity is the generalized-Gaussian exponent; the shader is
explicit that `N = 2 * uFilamentFalloff`.

`filamentShape` (what it controls) or `filamentExponent` (what it is) both read
correctly in both directions. **Medium churn:** ABI (`el_effect_set_filament_falloff`),
a `FieldBoundAnimation` field id, and a demo slider.

### S7. `Cutoff::size` is a distance, not a size

```
size - distance in pixels from the rect edge to the cutoff boundary
```

The doc has to open by correcting the field name. `distance` or `extent` says
it directly. Same ABI caveat as S6.

---

### S8. `GeometryUtils` did not follow the project's own `Get` rule - FIXED

Surveying all six util namespaces turns up a rule nobody wrote down but almost
everything follows:

> **`Get*` when the function derives a value from a described thing; a bare verb
> or conversion name otherwise.**

| namespace | `Get*` | other |
| --------- | ------ | ----- |
| `PathUtils` | `GetPathAABB`, `GetPathLength`, `GetPointOnPath` | - |
| `GLUtils` | `GetCap`, `GetExtensions` | `CheckExtension`, `CheckGLError`, `LogCaps`, `LogExtensions`, `LogRendererInfo` |
| `ColorUtils` | - | `SampleRing`, `SampleSpan`, `SortStops`, `BlendStops`, `RgbToHsv`, ... |
| `SegmentUtils` | - | `AcquireSegment`, `FillEffectiveSegments`, `FindPreservedSegment`, `ReleaseSegment` |
| `CaptureUtil` | - | `ReadRegion`, `ReadFramebuffer`, `WritePNG`, `TimestampedPath` |

`ColorUtils` and `SegmentUtils` have no `Get*` because they contain no
derive-a-value function - every entry is a verb (sample, sort, blend, acquire)
or a conversion (`RgbToHsv`). They are not counter-examples to the rule; they
simply never trigger it.

Against that, `GeometryUtils` had exactly one violation, and it was mine:
`EffectiveCornerRadius` derives a scalar property from a geometry description,
which is the same shape as `GetPathLength(path)`, but carried no prefix.

**Fixed:**

| before | after |
| ------ | ----- |
| `EffectiveCornerRadius` | `GetEffectiveCornerRadius` |
| `Detail::GetPointOnRectCW` | `Detail::GetPointOnRectangleCW` |
| `Detail::GetPointOnRectCCW` | `Detail::GetPointOnRectangleCCW` |

The second pair is a separate slip: the public entry point spelled it
`Rectangle` and the two helpers it delegates to spelled it `Rect`.

`AppToLocal` and `Detail::SafeFrac` keep their bare names correctly - a
coordinate conversion and a guarded division are not "get a property of a
thing".

Recorded because I got this backwards once already: asked whether
`EffectiveCornerRadius` should take a `Get`, I said no, reasoning from
`GeometryUtils` alone, where the split was 2-2 and looked arbitrary. Widening to
all six namespaces made the rule obvious and the answer the opposite. Worth a
line in `AGENTS.md` if it is ever formalised.

## What is already right

Worth recording so it does not get "tidied" later:

- **`SampleRing` / `SampleSpan`** ([`color-utils.h`](../lib/include/util/color-utils.h))
  is the model for a pair. Both are marked, both are named for the data shape
  rather than for the operation, and neither is the unmarked default - which is
  what makes reaching for the wrong one visible at the call site. The previous
  `SampleStops` / `SampleStopsClamped` split is what allowed V6 in
  [`review-findings.md`](review-findings.md).
- **The `On` prefix** is used consistently and only for callbacks
  (`OnConfigChanged`, `OnDurationChanged`, `OnResize`, `OnKey`).
- **Boolean members** carry `Is` / `Has` where the accessor does
  (`mIsPlaying`, `mHasBaked`, `mHasRun`), and `mLoop` mirrors its own
  `SetLoop` rather than being an inconsistency.
- **The C ABI**, as above.

## A gap in `AGENTS.md` itself

The document specifies `public` and `private` method casing but says nothing
about `protected`. The codebase has a clear de-facto rule - protected virtuals
use `PascalCase` (`RestoreBaseline`, `ApplyAt`, `OnDurationChanged`) - and that
is the right call, because a protected virtual is subclass-facing API rather
than an implementation detail. But because the rule is unwritten, any checker
run against `AGENTS.md` reports all 36 of them as violations, which is how they
surfaced in this review.

Worth one line in `AGENTS.md`: *protected methods follow the public rule
(`PascalCase`) - they are API for subclasses.*

---

## What is left

| item | state |
| ---- | ----- |
| N3, S3, S8, `AGENTS.md` gap | fixed |
| S1 | declined - half-fix only while the ABI freezes the field name |
| N1, N2, N5 | open - `demo-capi` only, out of scope for the `lib/` pass |
| N4 | open - `lib/capi/`, and the reserved-identifier half is a 47-file sweep |
| S5 | open - the field rename reaches `lib/capi/`, so it cannot be done `lib`-only |
| S2, S4, S6, S7 | recorded for whenever the C ABI is next revised; none worth doing alone |

Verified after the fixes: clean rebuild of all four artifacts, and the probe
suite from [`review-findings.md`](review-findings.md) unchanged - base vs
optimized still mean |diff| 0.161 / max 7, the corner-wedge and radius-clamp
cases still byte-identical.
