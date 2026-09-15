# Add a SpotlightRenderer

**Status: done.** All eight parts landed, with three corrections the plan did
not anticipate - each recorded below where it applies, and all three found by
the offscreen verification rather than by reading the code.

Decisions taken at approval: `SPOT_MAX_LIGHTS` = 8, colour by `colorTemp`
(Kelvin), and no rect gating - a cone crosses the frame freely.

A new renderer that emits N independently placed and aimed cones of light and
nothing else - no backdrop, no fixture housings, no floor, no framebuffer
capture. Each lamp carries its own position, direction, beam angle, throw,
softness, intensity and colour temperature. The layer composites additively over
whatever is behind it, exactly as the droplets and lens-flare layers do.

Two decisions shape everything below and should be confirmed first:

1. **Lamp positions are app coordinates with (0, 0) at the top-left**, the same
   convention `RectGeometry::position` already uses. The consequence is stated
   in Part 1 and it is not a small one.
2. **Per-lamp data rides on vertex attributes, not a uniform block.** That drops
   out of the geometry decision in Part 4 and is what keeps the shader free of
   any array declaration.

## Context

The library's four existing renderers are all *perimeter* effects: every one of
them parameterises its look by position along the rounded rect's edge. This one
does not. A lamp sits wherever the caller puts it and points wherever the caller
aims it; the rect is not involved in the light at all.

That makes it the simplest renderer in the tree by some distance. There is one
pass, one draw call, no offscreen buffer, no LUT, no UBO, and no resolution
scale. What it does need care with is the draw geometry, because a cone of light
covers a small, oriented, tapering region of the screen and bounding it lazily
costs more than the shading does.

The prototype that established the term stack and the geometry is at
<https://claude.ai/artifact/KGcghi21RLNVipwqXiANDT>. The measurement that
motivates Part 4: bounding each cone with an oriented box rasterised **559% of a
1280x720 viewport** for five lamps; the solved strip rasterises **100%** for the
same rig and the same pixels.

## The light model

One lamp's contribution at a point, in the lamp's own frame where `a` is
distance along the beam axis and `c` is distance across it:

```
halfW(a) = apertureWidth + max(a, 0) * tan(beamAngle / 2)

cone(a, c) = intensity
           * exp(-max(a, 0) / throwLength)          // falls off along the throw
           * (apertureWidth / halfW(a))             // energy spreads as it widens
           * exp(-(c / halfW(a))^2 * softK)         // soft across, no hard edge
           * smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, a)

bloom(a, c) = bloomStrength * r^2 / (a^2 + c^2 + r^2)      where r = bloomRadius
            * (1 - smoothstep(S * SPOT_BLOOM_WINDOW_INNER, S, sqrt(a^2 + c^2)))
                                                    where S = r * SPOT_BLOOM_SUPPORT

softK = SPOT_SOFT_MAX + (1 - softness) * (SPOT_SOFT_MIN_SPAN)
```

Three notes on why it is shaped this way.

The lateral falloff is a gaussian rather than a `smoothstep` cut because the
reference look has **no visible beam edge anywhere**; a penumbra parameter that
can reach zero is the wrong control for this effect and is deliberately absent.

The `apertureWidth / halfW(a)` term is what stops a wide beam from reading as
brighter overall than a narrow one at the same intensity. Without it, widening
the beam adds light rather than spreading it.

The bloom is inverse-square and therefore has **no natural end**. It is windowed
off at `SPOT_BLOOM_SUPPORT` radii so its support is finite and the CPU can bound
it. This is the same problem `GetGhostBloomRadius` solves for the lens flare's
ghosts, and it is solved the same way.

## Part 1 - Config (`lib/include/core/config.h`)

```cpp
/// One spotlight. Position and direction are the caller's to set; nothing
/// here is derived from Config::geometry.
typedef struct SpotLight
{
    /// Lamp position in APP coordinates - the same space as
    /// @c RectGeometry::position: origin at the viewport's TOP-LEFT, +x right,
    /// +y DOWN. Not rect-local, so the lamp does NOT move when the rect moves.
    glm::vec2 position = glm::vec2(0.0f, 0.0f);
    /// Direction the beam points, in degrees. 0 = +x (right), increasing
    /// CLOCKWISE on screen, which is what +y down makes natural: 90 = straight
    /// down, 180 = left, 270 = up.
    float angle = 90.0f;
    float beamAngle = 26.0f;      ///< Full field angle in degrees.
    float throwLength = 215.0f;   ///< Px along the axis to 1/e of peak.
    float apertureWidth = 13.0f;  ///< Half-width of the beam at the lamp, px.
    float softness = 0.55f;       ///< 0 = tightest gaussian, 1 = broadest.
    float intensity = 1.15f;
    float bloom = 0.4f;           ///< Aperture glow strength; 0 removes it.
    float bloomRadius = 20.0f;    ///< Aperture glow size, px.
    float colorTemp = 5600.0f;    ///< Kelvin, baked to linear RGB on the CPU.
    bool enable = true;           ///< false skips the lamp without removing it.

    bool operator==(const SpotLight &o) const { /* every field */ }
    bool operator!=(const SpotLight &o) const { return !(*this == o); }
} SpotLight;

typedef struct SpotlightConfig
{
    bool enable = false;
    /// Clamped to SPOT_MAX_LIGHTS at draw time; entries past it are ignored.
    std::vector<SpotLight> lights;

    bool operator==(const SpotlightConfig &o) const
    {
        return enable == o.enable && lights == o.lights;
    }
    bool operator!=(const SpotlightConfig &o) const { return !(*this == o); }
} SpotlightConfig;
```

Then a `SpotlightConfig spotlight;` member on `Config` and a `spotlight ==
o.spotlight` term in its `operator==`. Omitting that term is the failure the
conventions section warns about: every rebuild in Part 4 hangs off it.

**The consequence of app coordinates.** With `position` in viewport space, a
change to `Config::geometry.position` moves the rect and leaves the rig where it
is. Under rect-local coordinates the rig would travel with the rect. App
coordinates are what was asked for and they are the simpler model, but it is
worth being sure: if the lamps are meant to stay glued to the frame as it moves,
this is the decision to revisit, and the cheapest fix is an optional
`perimeterPosition` alternative on the same struct rather than a different
coordinate space.

## Part 2 - Tuning header and shaders

### `lib/include/renderer/spotlight-tuning.h`

Plain `#define`s so the file compiles as both GLSL ES 3.00 and C++, ASCII only,
same reasoning as `neon-tuning.h`.

```
#define SPOT_NEAR_FADE           1.6    // aperture widths the near end fades over
#define SPOT_BLOOM_SUPPORT       8.0    // bloom radii at which the glow is windowed off
#define SPOT_BLOOM_WINDOW_INNER  0.55   // where that window starts, as a fraction of it
#define SPOT_SOFT_MAX            3.40   // gaussian exponent at softness 0
#define SPOT_SOFT_MIN            0.85   // gaussian exponent at softness 1
#define SPOT_VISIBILITY_FLOOR    (0.5 / 255.0)   // HALF a step - see below
```

**CORRECTION 1, and the reason the verification exists.** The plan cut the strip
where the falloff drops below ONE 8-bit step. That is wrong, and it shipped
wrong until the offscreen diff caught it: GL rounds float-to-unorm to NEAREST,
so a contribution of 0.9/255 still lands on 1. Cutting at a full step clipped
pixels that were about to be lit - **36,000 to 267,000 of them per scene, in
every one of the ten scenes**, each off by exactly 1 LSB. The floor is half a
step, and the renderer divides it by the number of ENABLED lamps before solving,
so N lamps each under floor/N sum to under floor.

**Be honest about what this header does and does not buy.** In droplets and the
lens flare, the CPU reads these constants directly to derive a bound, so the two
sides cannot disagree. Here only `SPOT_BLOOM_SUPPORT` and the two softness
constants are read by both. The deeper coupling is that the CPU solver in Part 4
inverts the *whole falloff expression* the shader evaluates, and no header can
enforce that. The guard for it is the image diff in Verification, not this file.

### `lib/shaders/spotlight.vert`

```glsl
uniform mat4 uMVP;
in vec2 aPos;        // app px, top-left origin, +y down
in vec2 aLocal;      // (along, across) px in the lamp's frame
in vec4 aP0;         // tanHalf, throwLength, softK, intensity
in vec4 aP1;         // apertureWidth, bloom, bloomRadius, bloomSupport
in vec3 aColor;      // linear RGB

out vec2 vLocal;
flat out vec4 vP0;
flat out vec4 vP1;
flat out vec3 vColor;

void main()
{
    vLocal = aLocal;
    vP0 = aP0;
    vP1 = aP1;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
```

`aP0` / `aP1` / `aColor` hold the same value on all six vertices of every quad
in a lamp's strip, so they would interpolate to themselves even without `flat`.
`flat` is available in both version lines this project emits (`#version 330
core` and `#version 300 es`) and is used because skipping the interpolation is
free; correctness does not depend on it.

This is also what replaces a uniform block. There is no per-index array in the
shader at all, so the project's no-bare-uniform-arrays rule is satisfied by
construction rather than by a `layout(std140)` block and a binding point.

### `lib/shaders/spotlight.frag`

Evaluates the two terms from **The light model** above on `vLocal`, sums them,
and writes

```glsl
fragColor = vec4(vColor * vP0.w * (cone + bloom), 0.0);
```

Premultiplied with `alpha = 0`, so under the house blend it is pure addition.
Light only ever adds.

Note for whoever maintains this: the fragment stage **never reads
`gl_FragCoord`**. Everything it needs arrives interpolated in the lamp's own
frame. That is why the y-flip in Part 4 is free, and why the sub-viewport
caveat in `BaseRenderer`'s doc comment does not apply to this renderer.

## Part 3 - Build wiring

Three places, per the rule in `CLAUDE.md`:

1. `lib/CMakeLists.txt` - add `shaders/spotlight.vert`, `shaders/spotlight.frag`
   and `include/renderer/spotlight-tuning.h` to `CMAKE_CONFIGURE_DEPENDS`.
2. `lib/CMakeLists.txt` - add the two `file(READ ...)` calls for the shaders and
   one for the tuning header into `SPOTLIGHT_TUNING`.
3. `lib/shaders/shaders.h.in` - add `SPOTLIGHT_VERT_SRC` and
   `SPOTLIGHT_FRAG_SRC`, the latter with `@SPOTLIGHT_TUNING@` above the content.

## Part 4 - The renderer (`lib/include/renderer/spotlight-renderer.h`, `lib/src/renderer/spotlight-renderer.cpp`)

### Geometry: a solved strip per lamp

Each lamp gets `SPOT_STRIP_SEGMENTS` (12) quads forming a tapered strip that
hugs the region where that lamp writes anything at all. All lamps go into one
VBO and one `glDrawArrays`.

The support is **solved, not guessed**. Working in the lamp's frame, the cone is
below one 8-bit step wherever `cone(a, c) < 1/255`. Taking logs and solving for
`c`:

```
e(a)     = ln(intensity) + ln(255) - a / throwLength - ln(halfW(a) / apertureWidth)
cMax(a)  = e(a) <= 0 ? 0 : sqrt(e(a) / softK) * halfW(a)
```

`e` is strictly decreasing in `a` for `a >= 0`, so `aMax` - the furthest the cone
reaches - is a bisection on `e(a) = 0`. The bloom's disc of radius
`bloomRadius * SPOT_BLOOM_SUPPORT` is unioned in, which also sets how far behind
the lamp the strip has to start. Each sample is widened to the MAXIMUM of the support
across the half-intervals its chords cover (`WIDEN_SUBSAMPLES` sub-samples per
side), so a chord can only bulge outward, never cut inside.

**CORRECTION 2.** The plan widened against the neighbours' midpoints alone. That
does not bound the last chord, because `c(a)` falls to zero at the far end with
a near-vertical tangent. Recorded as a correction rather than a fix because
measurement says so: sub-sampling changed **no pixel in any scene**, so it is a
guarantee being made true, not a defect being repaired.

Helper placement follows `BakeGhostTable`: a file-local function in an anonymous
namespace at the top of the `.cpp`, next to the constants it reads.

### Coordinates and the flip

The VBO holds positions in **app coordinates verbatim** - top-left origin, +y
down - and the flip lives in the projection:

```cpp
const glm::mat4 mvp = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                 static_cast<float>(viewportHeight), 0.0f,
                                 -1.0f, 1.0f);
```

Note `bottom` and `top` are swapped relative to the ortho the other renderers
build. The payoff is that **the VBO depends only on `Config::spotlight` and
never on the viewport**, so a window resize costs a uniform and not a rebuild.
Triangle winding is reversed by the flip, which is harmless because nothing in
this library enables `GL_CULL_FACE` - worth a comment at the ortho so it is not
mistaken for a bug later.

### Buffer lifetime

Allocate once in `Initialize` at the ceiling size - `SPOT_MAX_LIGHTS *
SPOT_STRIP_SEGMENTS * 6 * sizeof(Vertex)`, which is 8 * 12 * 6 * 60 = **34 KB**
- with `GL_DYNAMIC_DRAW`, and fill with `glBufferSubData`. Never size it to the
live lamp count: that puts a reallocation on the animation path, which is the
mistake the neon emission table's sizing comment already records.

### Dirty gating

`OnConfigChanged` rebuilds when `config.spotlight != mCurrentSpotlight`, then
stores it. Narrow rather than wide, like `bakeGhostBlock`'s gate: every input to
the strip lives in one struct that carries its own `operator==`, so the gate
cannot fall out of step with what the rebuild reads.

Under a `FieldBoundAnimation` driving any lamp scalar this rebuilds every frame.
That is roughly 100 transcendental calls plus a 34 KB upload, which is cheap
enough to accept for the simplicity of one code path - but it is the number to
look at first if this layer ever shows up in a profile.

### Pass schedule

One pass. `Render` is:

1. Bail if `!config.spotlight.enable`, no enabled lamps, or a non-positive
   viewport.
2. `glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);`
3. Set `uMVP`, bind, `DrawArrays(GL_TRIANGLES, mVertexCount)`.
4. Restore `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, the convention
   the other renderers leave behind.

No framebuffer retarget, so no `Framebuffer::GetBoundId` / `BindId` dance.

### Registration order

After `NeonRenderer`, before `DebugRenderer`. Neon's opaque fill
(`black-rect.frag`) is opaque and would paint over spotlights drawn before it;
the debug overlays annotate what the layers under them drew and must stay last.

## Part 5 - Animation

Every spotlight scalar is per-index, so this needs no `AnimatableField` entries -
only a new per-index family alongside `SegmentField` / `ArcField` in
`lib/include/animation/field-bound-animation.h`:

```cpp
typedef enum class SpotlightField
{
    POSITION_X = 0, POSITION_Y = 1, ANGLE = 2, BEAM_ANGLE = 3,
    THROW_LENGTH = 4, APERTURE_WIDTH = 5, SOFTNESS = 6, INTENSITY = 7,
    BLOOM = 8, BLOOM_RADIUS = 9, COLOR_TEMP = 10,
} SpotlightField;
```

plus `FieldBoundAnimation::AddSpotlightField(size_t index, SpotlightField,
ModulatorPtr)`, an out-of-range index being a logged no-op as the existing
`AddArcField` is.

## Part 6 - C ABI (`lib/capi/`)

- `el-types.h`: `EL_RENDERER_SPOTLIGHT = 1 << 3`, joining `EL_RENDERER_ALL`; a
  POD `el_spotlight_t` mirroring `SpotLight`; `el_spotlight_field_e` mirroring
  `SpotlightField`.
- `el-effect.h` / `.cpp`: the list family the segment boosts already model -
  `el_effect_set_spotlight_enabled` / `get`, `set_spotlight_count` /
  `get_spotlight_count`, `set_spotlight(index, const el_spotlight_t*)` /
  `get_spotlight`, `clear_spotlights`. All write staging only; staging reaches
  the effect in `el_effect_update`, as ever.
- `el-animation.h` / `.cpp`: `el_animation_add_spotlight_field`.
- `capi-internal.h`: `static_assert`s pinning `el_spotlight_field_e` to
  `SpotlightField`, appended to the existing wall.
- `el_effect_init_with_renderers`: register the renderer on the new bit, in the
  same position as the C++ demo.

## Part 7 - Demos

- `demo/src/debug-ui.{h,cpp}`: a Spotlights section - a lamp list with
  add/remove/select, then position, angle and the beam sliders for the selected
  lamp. Reading back through `GetActiveConfig()` so the sliders follow any
  animation.
- `demo/src/main.cpp`: construct and register in the order from Part 4.
- `demo-capi/src/`: the same UI against the flat ABI. It is a hand-maintained
  fork, so this is a real second edit, and it is also the check that Part 6 left
  the ABI sufficient for a UI-shaped host.

## Part 8 - Docs

- `CLAUDE.md`: a renderer bullet in the Architecture list, and the new shader
  and tuning header in the shader-embedding section.
- `docs/spotlight-renderer.md`: the per-parameter reference, written after it
  lands so it describes what shipped.
- This file: flipped to `**Status: done.**` with the verification results.

## Verification

There is no test target, so verification is by offscreen capture through
`OffscreenCapture` plus one C-only program, matching how the unification work
was checked.

1. **The support solve is correct.** The one that matters, and the one that
   found the real defect. Ten scenes at 1280x720 through `OffscreenCapture`: one
   lamp straight down; one at 45 degrees; a five-lamp fan; `bloom = 0`; bloom
   and intensity both high, where the bloom window rather than the cone sets the
   bound; `softness = 1`, the widest gaussian; a tight 5-degree beam on a 700 px
   throw; two overlapping lamps at 2700 K and 7000 K; the same pair with the
   order swapped; and a lamp whose origin sits off the left edge.

   **CORRECTION 3 - byte-identical was the wrong bar.** The plan asked for a
   byte-identical diff against a deliberately oversized quad. That test cannot
   pass, and not because of clipping: changing a quad's corner magnitudes
   changes how `aLocal` interpolates in its last bit, so the shaded value moves
   by up to 1 LSB. The giveaway is WHERE the differences fall - spread across
   every brightness level including the brightest core, and clipping can only
   remove light from the dim tail.

   What replaced it measures the thing actually at stake: a **coverage** test.
   Render each scene again with every strip scaled 4x, and require that no pixel
   the larger strip lights is dark in the shipped one. Result after correction
   1: **at most 3 pixels per scene out of 100,000 to 900,000 lit, every one at
   value 1** - the rounding coin-flip at the boundary, not missing coverage;
   halving the floor again does not reduce it. Before correction 1 the same
   scenes were wrong by tens of thousands of pixels.

   The closest precedent in this tree is
   [`docs/lens-flare-perf-review.md`](lens-flare-perf-review.md), which records
   one change that cannot be byte-identical on any GPU for the same class of
   reason.
2. **Additivity.** PASSES, byte-identical: scenes 8 and 9 are the same two
   lamps in opposite config order and produce the same file. Premultiplied
   addition is order-independent, and anything that breaks that is a
   blend-state bug.
3. **Blend-state hygiene.** A frame with spotlights plus droplets plus neon
   matches a frame with the same layers minus spotlights, in the regions no
   lamp reaches.
4. **Resize invariance.** The same rig at two viewport sizes puts each lamp at
   the same app-space pixel, and the VBO is not re-uploaded on resize.
5. **ABI parity.** A C-only program building the same rig through
   `libedge-lighting-c` produces a capture identical to the C++ path.
6. **Conventions.** ASCII-only shader sources, no em-dash anywhere, every new
   struct carrying `operator==` / `operator!=` over all its fields, and
   `Config::operator==` including the new member.

## Settled at approval

1. **App coordinates**, origin top-left. The rig does not follow the rect; a
   perimeter-anchored alternative on the same struct stays available if that
   turns out to matter.
2. **`SPOT_MAX_LIGHTS` = 8.** Sizes the VBO ceiling only (34 KB).
3. **`colorTemp` in Kelvin**, baked to linear RGB on the CPU. No RGB tint, so a
   saturated non-blackbody lamp is not reachable - add a `glm::vec3 tint`
   multiplied on top if one is ever wanted.
4. **No rect gating.** A cone crosses the frame freely.

## Still open

- **Verification 4 and 5 were not run.** Resize invariance and the C-only ABI
  smoke program are specified above but untested. The ABI compiles and the
  `demo-capi` fork drives the new surface, which is weaker evidence than a
  capture diff.
- **No `docs/spotlight-renderer.md`.** The per-parameter reference is still only
  the doc comments in `config.h`.
