# Multiple arcs

Design + implementation plan for the "multiple arcs" feature: replace the
single perimeter-gate pair (`NeonConfig::arcStart`, `arcLength`) with an
`arcs` vector, where each entry carries its own start, length, intensity,
and optional colour stops.

## 1. Motivation

Today the renderer supports **one** arc: a single contiguous slice of the
perimeter is "on", the rest is "off", driven by `arcStart` / `arcLength`.
This is enough for a single wipe / trace animation, but the moment you want
two lit sides of a rounded rect (different colours, different intensities)
or a Knight-Rider-style chase with multiple heads, you have to fake it with
segments (which are point-lights, not arc gates).

Arcs and segments overlap in intent but are mechanically different:

| Aspect             | Arc                                        | Segment                                    |
| ------------------ | ------------------------------------------ | ------------------------------------------ |
| Shape              | Contiguous slice of perimeter (gate)       | Gaussian bell around a point               |
| Falloff            | Hard edges (smoothstepped by one sample)   | Smooth Gaussian falloff                    |
| Multiplicity today | 1                                          | up to `MAX_SEGMENT_BOOSTS` (= 8)           |
| Composition        | Multiplies base emission (gate)            | Additive on top of base                    |

The feature aligns arcs with the segments story: up to `MAX_ARCS` (= 8)
independent arcs, each with its own visual identity.

## 2. Design decisions

Decisions locked at design time (see the AskUserQuestion answers in the
conversation that produced this doc):

1. **Per-arc payload**: `start`, `length`, `intensity`, `colorStops` (empty
   = inherit base gradient), `blendSpace`. Full symmetry with `SegmentBoost`
   plus a per-arc intensity multiplier.
2. **Overlap semantics**: **winner-take-all**. Per perimeter sample, compute
   `effectiveMask_i = arcMask_i * arcIntensity_i` for each arc; the arc with
   the largest `effectiveMask` wins and contributes its colour + effective
   mask to the emission. `arcMask` is already smoothstepped at the edges,
   so adjacent arcs of different colours crossfade at the seam - no hard
   flip.
3. **Cap**: 8 arcs (matches `MAX_SEGMENT_BOOSTS` - keeps the UBO small and
   the per-sample gather loop tight).
4. **Backward-compat**: **replace**. `arcStart` / `arcLength` are deleted;
   `arcs` defaults to `{ {0, 1, 1, {}, RGB} }` - a single full-perimeter
   arc, so the default look is unchanged. Existing `ArcWipe` / `OutlineTracer`
   animations retarget to `arcs[0]`, the same way the segment animations
   target `segmentBoosts[0]`.

## 3. Data model

### `EdgeLighting::Arc` (new)

```cpp
typedef struct Arc
{
    float start = 0.0f;     ///< Start of the arc in [0, 1) perimeter position.
    float length = 1.0f;    ///< Fraction of the perimeter lit (0 = off, 1 = full).
    float intensity = 1.0f; ///< Per-arc brightness multiplier (independent of NeonConfig::intensity).
    /// Colour stops laid across the arc's span, head-to-tail. Empty means
    /// "inherit the base gradient" - the arc reads its colour from
    /// NeonConfig::colorStops at each sample it touches.
    std::vector<ColorStop> colorStops;
    /// Blend space for interpolating colorStops. Ignored when empty.
    BlendSpace blendSpace = BlendSpace::RGB;

    bool operator==(const Arc &) const;
    bool operator!=(const Arc &) const;
} Arc;
```

Structural mirror of `SegmentBoost` with `position/length/boost` replaced by
`start/length/intensity`.

### `NeonConfig::arcs` (replaces `arcStart` + `arcLength`)

```cpp
std::vector<Arc> arcs = { {0.0f, 1.0f, 1.0f, {}, BlendSpace::RGB} };
static constexpr int MAX_ARCS_CAP = MAX_ARCS;
```

Default vector holds one full-perimeter arc so the un-configured look is
identical to today.

### `neon-tuning.h`

```c
#define MAX_ARCS 8
```

Same rationale as `MAX_SEGMENT_BOOSTS` - a macro (not `constexpr`) because
this value is text-injected into GLSL, which has no `constexpr` and no `f`
float suffix.

## 4. Shader

### Uniforms

Replace:

```glsl
uniform float uArcStart;
uniform float uArcLength;
```

with:

```glsl
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS]; // (start, length, intensity, hasStops)
};
uniform sampler2D uArcLUT;  // MAX_ARCS rows x 128 texels, one row per arc.
```

Same pattern as `SegmentBlock` + `uSegmentLUT`: std140 stride matches the
CPU-side `ArcBlockData` mirror; `hasStops > 0.5` selects between the atlas
row and the base gradient at sample time.

### Winner-take-all gate

The gather loop's per-sample arc handling changes from a scalar `arcW`
(single-arc gate) to a winner search:

```glsl
float arcInside(float si, float start, float length, float invNumSamples) { ... } // unchanged

// Inside the perimeter-sample loop:
float bestMask = 0.0;
int   bestIdx  = -1;
for (int a = 0; a < uArcCount; a++) {
    vec4 arc = uArcs[a];
    float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z; // mask * intensity
    if (mask > bestMask) {
        bestMask = mask;
        bestIdx  = a;
    }
}
float arcW = bestMask;
vec3  baseColI;
if (bestIdx >= 0) {
    vec4 winner = uArcs[bestIdx];
    if (winner.w > 0.5) {
        float rowY = (float(bestIdx) + 0.5) / float(MAX_ARCS);
        // ti is the same shifting texture coord the existing shader uses.
        baseColI = texture(uArcLUT, vec2(ti, rowY)).rgb;
    } else {
        baseColI = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
    }
} else {
    baseColI = vec3(0.0);
}
// Existing accumulator lines then use arcW / baseColI unchanged.
```

The `bestIdx` dynamic index of `uArcs` is a std140 uniform block - some
mobile compilers dislike dynamic indexing of uniform arrays, but the block
is small (`MAX_ARCS = 8`) so an `unroll` hint or a small switch is a safe
fallback if we hit that in practice.

### Compose changes

Nothing else in the compose changes - `arcW` still gates filament / halo /
bloom, and `baseColI` still feeds the segment gather loop's "inherit base"
branch. Segments continue to compose additively on top of the arc emission.

## 5. Renderers

Both `NeonRenderer` and `NeonOptimizedRenderer` grow:

- `Texture2D mArcLUT` (RGBA8, 128 x `MAX_ARCS`, no wrap - each row is one arc's
  bake).
- `UniformBuffer mArcBlock` (std140 mirror of `ArcBlock`).
- `std::vector<Arc> mBakedArcs` for dirty-check.
- `void rebuildArcLUT(const Config &cfg)`: for each arc with non-empty
  `colorStops`, bake into row `i` of `mArcLUT` via `ColorUtils::SampleSpan`;
  arcs with empty stops leave their row untouched (`hasStops = 0` in the UBO
  makes the shader skip the atlas sample).

Per-frame `Render`:

```cpp
ArcBlockData block = {};
int arcCount = std::min((int)cfg.neon.arcs.size(), MAX_ARCS);
block.count = arcCount;
for (int i = 0; i < arcCount; ++i) {
    const Arc &a = cfg.neon.arcs[i];
    float hasStops = a.colorStops.empty() ? 0.0f : 1.0f;
    block.arcs[i] = glm::vec4(a.start, a.length, a.intensity, hasStops);
}
mArcBlock.SetData(&block, sizeof(block));
mArcBlock.BindBase(ARC_BLOCK_BINDING);

mArcLUT.Bind(2);
mShaderProgram.SetUniform("uArcLUT", 2);
```

`uArcStart` / `uArcLength` uniform sets are removed.

Dirty-check in `OnConfigChanged`: rebuild the atlas whenever `mBakedArcs !=
cfg.neon.arcs`.

## 6. Animations

### Presets

- `OutlineTracer` sweeps `arcs[0].length` from 0 -> 1. `CaptureBaseline`
  snapshots the whole `arcs` vector (matches the "vector auto-grew" case).
- `ArcWipe` writes `arcs[0].start` and `arcs[0].length` per phase, same
  snapshot approach.

Both auto-grow `cfg.neon.arcs` to at least one entry on first write, mirroring
how `SegmentTravel` / `SegmentBounce` handle `segmentBoosts`.

### `FieldBoundAnimation`

New binding types:

- `AddArcField(size_t arcIdx, ArcField field, ModulatorPtr modulator)` where
  `ArcField ∈ {START, LENGTH, INTENSITY}`.
- `AddArcStopField(size_t arcIdx, size_t stopIdx, ColorStopField field,
  ModulatorPtr modulator)` (mirror of `AddStopField` for segments).

Both auto-grow `cfg.neon.arcs` and (for stop bindings) the winner arc's
`colorStops` at write time, seeded with a visible default so binding only
one channel still produces something.

`AnimatableField` loses `NEON_ARC_START` and `NEON_ARC_LENGTH` (they move
to `ArcField`).

`CaptureBaseline`: if any arc/arc-stop binding is present, snapshot the
whole `arcs` vector alongside the existing scalar / segment snapshots.
`RestoreBaseline` writes it back verbatim.

## 7. C API

Version bump: `EL_ABI_VERSION 11` (was 10).

### New POD

```c
typedef struct EL_Arc
{
    float start;
    float length;
    float intensity;
    int32_t colorStopCount;
    EL_ColorStop colorStops[EL_MAX_COLOR_STOPS];
    int32_t blendSpace;
} EL_Arc;
```

### `EL_NeonConfig` changes

Remove:

```c
float arcStart;
float arcLength;
```

Add:

```c
int32_t arcCount;
EL_Arc arcs[EL_MAX_ARCS];
```

`EL_MAX_ARCS` is a new macro, `8`.

### Enum changes

- Remove `EL_FIELD_NEON_ARC_START`, `EL_FIELD_NEON_ARC_LENGTH` from
  `EL_ConfigField`; renumber the enum accordingly.
- Add:

  ```c
  typedef enum EL_ArcField
  {
      EL_ARC_FIELD_START     = 0,
      EL_ARC_FIELD_LENGTH    = 1,
      EL_ARC_FIELD_INTENSITY = 2
  } EL_ArcField;
  ```

### New entry points

```c
EL_API EL_Result el_animation_add_arc_field(
    EL_Animation *anim, int32_t arcIdx, int32_t field /*EL_ArcField*/,
    EL_Modulator *mod);

EL_API EL_Result el_animation_add_arc_stop_field(
    EL_Animation *anim, int32_t arcIdx, int32_t stopIdx,
    int32_t field /*EL_ColorStopField*/, EL_Modulator *mod);
```

## 8. Debug UI

The Neon section's `arcStart` / `arcLength` slider pair is replaced by an
Arcs sub-editor mirroring the segment editor:

- Header row: `Arcs (n / MAX_ARCS)` + `Add arc` button (disabled at cap).
- Per-arc rows (indented):
  - Remove button.
  - `Start`, `Length` sliders in `[0, 1]`, wrap-aware.
  - `Intensity` slider in `[0, 4]` (matches `SegmentBoost.boost` range).
  - Collapsible `Color stops` sub-panel with the same per-stop UI already
    used by the segment editor.

All numeric sliders use `AnimatedSlider` so animated arc fields display
the effect's active value while the base value drives the slider on drag.

## 9. Files touched (~15)

Fresh files:
- `docs/multiple-arcs-design.md` (this doc)

Edited:
- `lib/include/renderer/neon-tuning.h`      -- `MAX_ARCS = 8`
- `lib/include/core/config.h`               -- `Arc` + `arcs` vector; drop `arcStart/Length`
- `lib/shaders/neon.frag`                   -- swap uniforms for `ArcBlock` + `uArcLUT`, winner-take-all
- `lib/shaders/neon-optimized.frag`         -- same
- `lib/include/renderer/neon-renderer.h`    -- `mArcLUT`, `mArcBlock`, `mBakedArcs`, `rebuildArcLUT`
- `lib/src/renderer/neon-renderer.cpp`      -- bake + per-frame pack
- `lib/include/renderer/neon-optimized-renderer.h` -- mirror
- `lib/src/renderer/neon-optimized-renderer.cpp`   -- mirror
- `lib/include/animation/neon-animations.h` -- retarget `ArcWipe` / `OutlineTracer` to `arcs[0]`
- `lib/include/animation/field-bound-animation.h`  -- `ArcField`, `AddArcField`, `AddArcStopField`, drop `NEON_ARC_*`
- `lib/src/animation/field-bound-animation.cpp`    -- `writeArc`, `writeArcStop`, capture/restore
- `lib/capi/edge-lighting-capi.h`              -- `EL_Arc`, `EL_ArcField`, arcs on `EL_NeonConfig`, bump ABI
- `lib/capi/edge-lighting-capi.cpp`            -- helpers + new entry points
- `demo/src/debug-ui.h/.cpp`                -- per-arc rows in Neon section

## 10. Verification plan

1. Default config: single full-perimeter arc renders like today (no visual
   diff vs pre-feature main).
2. Add a second arc from the debug UI covering `[0.5, 0.3]`, intensity 1.0:
   two distinct lit slices, gap dark.
3. Drop arc 0's intensity to 0.0: arc 1 remains lit unchanged.
4. Give arc 0 two colour stops (red -> blue): its slice inherits the custom
   gradient, arc 1 stays on the base gradient.
5. Existing `ArcWipe` / `OutlineTracer` presets still animate (they now
   write into `arcs[0]`).
6. Attach a `FieldBoundAnimation` binding `arcs[1].intensity` to a sine
   oscillator: arc 1 pulses independently of the others.
