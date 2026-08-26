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
rather than by judgement.

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

### N5. `#endif` guard comments are inconsistent in `demo-capi`

Five of the seven headers end with a bare `#endif`; `gl-mini.h` and
`ui-controls.h` carry the guard name, as every `lib/` and `demo/` header does.

---

## Semantic issues

Ordered by how misleading the name is, not by churn. Churn is noted per item
because several of these reach the frozen C ABI.

### S1. Three different word orders for "the half-res variant" - DECLINED

| | neon | lens flare |
| - | ---- | ---------- |
| renderer class | `NeonOptimizedRenderer` | `LensFlareOptimizedRenderer` |
| config struct | `Optimized`**`Neon`**`Config` | `LensFlare`**`Optimized`**`Config` |
| config field | `optimizedNeon` | `optimizedLensFlare` |

The renderers agree with each other. The config fields agree with each other.
The config *structs* do not - `OptimizedNeonConfig` is the only one that leads
with the qualifier, and it disagrees with both its own renderer and its
counterpart struct.

**Declined.** The rename to `NeonOptimizedConfig` was applied and then reverted
on the maintainer's call; `OptimizedNeonConfig` stands.

The reasoning for leaving it: the rename could only ever fix half the
inconsistency. The C ABI mirrors the config *field* as
`el_effect_set_optimized_neon_*`, so `optimizedNeon` is frozen. Renaming the
struct therefore aligns it with `LensFlareOptimizedConfig` and the two renderer
classes, but at the cost of making the struct disagree with its own field - one
inconsistency traded for another, in a type name that appears in three places
and that no caller outside `lib/` ever writes.

If the ABI is revised (see S2), the whole family - `NeonOptimizedRenderer`,
`NeonOptimizedConfig`, `neonOptimized`, `el_effect_set_neon_optimized_*` -
should be aligned in one pass. Piecemeal is what produced the three word orders
in the first place.

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
  (`mIsPlaying`, `mHasBakedLUT`, `mHasRun`), and `mLoop` mirrors its own
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
