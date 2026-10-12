# Implementation

The short version of how this library is put together and how a frame runs.
For depth, see [`neon-onboarding-guide.md`](neon-onboarding-guide.md) (the
neon from first principles, every pass and config field),
[`effect-reference.md`](effect-reference.md) (per-parameter behaviour), and
[`neon-renderer-explained.html`](neon-renderer-explained.html) (why the shader
is shaped the way it is). When any of them disagrees with the
headers under `lib/include/`, the headers win.

## 1. What gets built

`cmake -S . -B build -G Ninja && cmake --build build` produces four artifacts:

| Artifact | What it is |
|---|---|
| `build/lib/libedge-lighting.a` | The C++ static library. The real implementation. |
| `build/lib/libedge-lighting-c.dylib` | Flat `extern "C"` wrapper for FFI / P-Invoke. |
| `build/demo/edge-lighting-demo` | ImGui demo against the C++ library. |
| `build/demo-capi/edge-lighting-capi-demo` | The same UI against only the C ABI. |

macOS arm64 only in practice: `external/lib/` ships macOS GLFW binaries and the
imported location is hardcoded to `libglfw.3.dylib`. The `PLATFORM_WINDOWS` /
`PLATFORM_LINUX` branches in the root `CMakeLists.txt` are unfinished
scaffolding.

## 2. The one structural idea

`EdgeLighting::EdgeLightingEffect` ([`core/edge-lighting.h`](../lib/include/core/edge-lighting.h))
is an **orchestrator**. It owns no drawing code. It owns state and a list of
renderer plugins, and forwards to them:

```
Config (base) ─┐
               ├─► active Config ─► for each renderer: Update / Render
animations ────┘
```

Concretely it holds:

- a **base** `Config` - geometry plus one sub-config per renderer. What
  `SetConfig` writes and `GetConfig` reads.
- an **active** `Config` - base with animation overlays composited on top. What
  renderers actually see, readable via `GetActiveConfig()`.
- a `Clock` (play/pause time accumulator).
- an `AnimationManager` (by `unique_ptr`).
- a `vector<shared_ptr<BaseRenderer>>`, registered by the host.

Renderers are independent visual layers with no scene graph and no depth
sorting. They composite by blending, list order is stacking order, and any
subset can be enabled.

## 3. Per-frame control flow

Two calls per frame, from the host:

**`Update(dt)`** - [`edge-lighting.cpp:38`](../lib/src/core/edge-lighting.cpp)

1. Tick the clock, advance every attached animation by the clock delta.
2. Rebuild the active config (`refreshActiveConfig()`).
3. If the active config actually changed, call `OnConfigChanged(active)` on
   every renderer.
4. Call `Update(dt, clockTime, active)` on every renderer.

**`Render(w, h)`** - calls `Render(w, h, clockTime, active)` on every renderer,
in registration order.

The step-3 condition matters and is easy to misread: `SetConfig` returns early
when the base is unchanged, and `refreshActiveConfig` returns early when the
composited result is unchanged. So `OnConfigChanged` is **not** a per-frame
callback. An idle effect never fires it; an effect with a running animation
fires it on nearly every frame. Renderers can therefore treat the call as
meaningful, but must still gate their own rebuilds (§5).

## 4. The renderer contract

Subclass `BaseRenderer` ([`renderer/base-renderer.h`](../lib/include/renderer/base-renderer.h)),
four virtuals:

| Method | When | Job |
|---|---|---|
| `Initialize()` | once | Compile shaders, allocate GL objects. Returning false makes the orchestrator drop the renderer. |
| `OnConfigChanged(cfg)` | on change | Re-bake whatever the changed fields feed. Dirty-gated internally. |
| `Update(dt, t, cfg)` | every frame | Time-based state that is not drawing (e.g. the neon's colour cross-fade). |
| `Render(w, h, t, cfg)` | every frame | Bind and draw. |

Five renderers ship, registered by the demo in this order - which is also the
compositing order, since they blend onto one another in the order they draw:

| # | Renderer | Layer |
|---|---|---|
| 1 | `NeonRenderer` | The neon stroke: an emission pre-pass, the opaque fill, the gather, and - below `resolutionScale` 1.0 - a scaled buffer plus its blit, with a full-resolution ring re-shaded around the edge. |
| 2 | `DropletsRenderer` | Rain-on-glass in a band hugging the perimeter. Screen-space gravity, self-lit, no framebuffer capture. Always full-res. |
| 3 | `LensFlareRenderer` | Sun plus hex-aperture flare as one fullscreen premultiplied pass. The sun rides the perimeter, and - below `resolutionScale` 1.0 - a scaled buffer plus its blit. |
| 4 | `SpotlightRenderer` | Freely placed and aimed cones of light, as one screened pass over solved per-lamp strips. The odd one out: not a perimeter effect, reads only `Config::spotlight`, and - below `resolutionScale` 1.0 - a scaled buffer plus its blit. See [`spotlight-renderer.md`](spotlight-renderer.md). |
| 5 | `DebugRenderer` | The LUT strip, colour-stop markers and the 1px bounding box. **Last on purpose** - it annotates what the layers under it drew, so its overlays have to sit above all of them. Always full-res. |

`DebugRenderer` being last is the one position in that list that is load-bearing
rather than incidental, and the C ABI's `el_effect_init_with_renderers`
registers it last too. The bit `DebugRenderer` holds in `el_renderer_flags_e` is
the top one for the same reason: a content layer added later takes the next
dense bit and never renumbers it.

That list is only the DEFAULT order, though: layers composite by blending, so a
layer lands on whatever was drawn before it, and a host can choose differently.
The core owns the order: every renderer names its layer
(`BaseRenderer::GetLayer` returns a `RendererLayer`), and `EdgeLightingEffect`
builds and reorders by layer - `AddRenderer(RendererLayer)`, `GetLayerOrder`,
`SetLayerOrder`, `GetDefaultLayerOrder` - with `SetRendererOrder` underneath for
a host's own `CUSTOM` renderers, which the layer calls leave in place. The C
ABI's flag bits are the same values, so its calls only forward:
`el_effect_init_with_renderer_order` registers the listed layers, bottom first;
`el_effect_set_renderer_order` reorders an initialised effect with no re-init;
`el_effect_get_renderer_count` / `el_effect_get_renderer_at` read it back. The
motivating case is the spotlight: below the neon, the neon composites over the
cones by its own coverage instead of the cones adding onto the neon.

**There are no forked renderer pairs left.** Both layers that had one now carry
the half-res path as a resolution scale on a single renderer: `NeonConfig::
resolutionScale` and `LensFlareConfig::resolutionScale`, where 1.0 draws
straight onto the target and anything lower renders into a scaled buffer and
blits back - the neon then re-shading a thin ring around the edge at full
resolution, from a variant of the same `neon.frag`. One `.cpp` each and one
copy of every shader stage, no pair to keep in step and no way to double-draw.
(The neon's gather pass is a `.frag` of its own, `neon-gather.frag`, but the
loop it runs is the one `neon.frag` runs, shared through `neon-common.glsl`.) See
`docs/neon-unification-plan.md`, which also records how both merges were verified.

To add a renderer: subclass `BaseRenderer`, add a sub-config struct to `Config`
with `operator==`, register it in [`demo/src/main.cpp`](../demo/src/main.cpp),
add an ImGui section in `DebugUI`.

## 5. Change detection

`Config` and every sub-struct define `operator==` / `operator!=` over all their
fields. Two mechanisms depend on it:

- the orchestrator's active-config comparison, which decides whether
  `OnConfigChanged` fires at all;
- each renderer's internal dirty flags, which decide which rebuilds run.

`NeonRenderer::OnConfigChanged` ([`neon-renderer.cpp:354`](../lib/src/renderer/neon-renderer.cpp))
is the pattern: snapshot dirtiness against `mCurrentConfig` before overwriting
it, then run only the rebuilds whose inputs moved - perimeter samples, draw-quad
geometry, gradient LUT, segment atlas, arc atlas.

**Adding a field without adding it to `operator==` silently breaks rebuilds.**
That is the single most common way to introduce a bug here.

## 6. How the neon shader works, in a paragraph

No geometry is built for the glow. One quad is drawn, oversized to cover
everywhere light can land, and the fragment shader answers two questions per
pixel:

- **`d`** - exact signed distance to the rounded-rect outline, from a six-line
  analytic SDF (`sdRoundBox`). Negative inside, positive outside. Drives
  brightness via three stacked layers: filament (generalized Gaussian, gain
  12), halo (closed-form `1/d^2`), bloom (closed-form `1/d`). Corners need no
  special case because every distance contour is itself a rounded rectangle.
- **`t`** - position around the perimeter in `[0, 1)`, recovered geometrically
  by finding the nearest outline point and identifying which of the eight
  pieces (four edges, four corner arcs) it landed on. Drives colour and arc /
  segment gating. Matches `GeometryUtils::GetPointOnRectangle` exactly so
  CPU-authored positions line up with what the shader draws.

Multiply the two, tone map hue-preservingly, apply gamma, emit premultiplied
alpha (coverage = brightest channel) so the effect composites over arbitrary
content rather than only adding light.

Before that quad runs, the neon renderer executes an **emission pre-pass**.
The gather's per-sample work - the arc winner-take-all, the segment bells, the
LUT fetches - is a pure function of `(si, uTime, config)` and does not vary per
fragment, so it is baked once per frame into an `N x 2` RGBA16F table
(`neon-emission.frag`) that the gather then reads with `texelFetch`.
Per-fragment cost is `O(samples)` rather than `O(samples * (arcs + segments))`.
The invariant that keeps the split honest: **a pure function of
`(si, uTime, config)` belongs in the pre-pass; anything that reads `vPos`
belongs in the main shader.**

A second pre-pass bakes the **glow coverage table** (`neon-glow-cover.frag`):
for each of the outline's eight pieces - four straights, four corner arcs -
how lit that piece is as the halo and the bloom see it from a fragment, its own
coverage over its own extent weighted by each layer's kernel, in closed form
for the arcs and numerically for the segments' bells. The halo and bloom are a
sum over those pieces, and each piece's share is scaled by its own read of
that table (one linear fetch), not by one coverage gathered around the
fragment: the gathered one let an unlit stretch keep a faint line of light
along it and dimmed lit edges' light reaching a dark one (V19 and V20 in
[`review-findings.md`](review-findings.md)), and a table per perimeter rather
than per piece spilled light round each corner (V21). Where each piece's
coverage sits in the table is in `neon-pieces.glsl`, which both the bake and
`neon.frag` include. The table depends on the arcs, the segments, the rect's
shape and the glow radius only, so it is re-baked on a config change and never
on time. Its width follows the rect and the halo width (`GetGlowCoverWidth`,
256-1024 columns), and its layout is stored with it, so the bake and every
read agree on it (I59). The segments' bells, far wider than an arc's feather,
are baked into a narrow table of their own and copied into the main one by a
fill pass (`neon-glow-cover-fill.frag`, I60), so `neon.frag` still reads one
texel per piece.

Below `resolutionScale` 1.0 the same split pays twice. The gather is the only
part of the shader that is both expensive and smooth across the screen, so it
runs alone, in a pass of its own (`neon-gather.frag`, which calls the same
`gatherPerimeter` from `neon-common.glsl` that `neon.frag` calls inline), on a
grid set by its own smoothness (about two texels per colour kernel - far
coarser than the reduced buffer), and stores its four results in a small
buffer. A variant of `neon.frag` that reads those
results back instead of looping then shades the glow twice: at the reduced
scale into the reduced buffer, and at full resolution in a thin ring around the
edge. The line and every edge near it are shaded at full resolution, as with no scale at all.
Everything outside the ring is the reduced buffer, bilinear-blitted.

The full derivation, including the closed forms and the sampling bugs they
replaced, is in [`neon-renderer-explained.html`](neon-renderer-explained.html);
the pre-pass has its own design note in
[`emission-prepass.md`](emission-prepass.md), and the edge ring in
[`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md) (sections 7
and 13). For the whole pipeline walked once in order, pass by pass and field
by field, start with [`neon-onboarding-guide.md`](neon-onboarding-guide.md).

## 7. Shader and C++ interop

**Shaders are embedded at configure time, not loaded at runtime.**
[`lib/CMakeLists.txt`](../lib/CMakeLists.txt) reads `lib/shaders/*.{vert,frag}`
and substitutes them into `shaders.h.in`, producing
`build/lib/generated/shaders.h` with each source as a `const char* const` raw
string in `EdgeLighting::ShaderSource::*`. No runtime file I/O, no shader
directory to ship.

- `@GLSL_VERSION@` supplies the version line (`330 core` on desktop,
  `300 es` on the mobile branches).
- `@NEON_TUNING@` injects [`renderer/neon-tuning.h`](../lib/include/renderer/neon-tuning.h)
  verbatim, so tuning constants are shared between shader and C++ and cannot
  drift. That is why the file is `#define`-based: GLSL ES 3.00 has no
  `constexpr` and rejects the `f` literal suffix, so plain macros are the only
  form valid in both languages.
- `CMAKE_CONFIGURE_DEPENDS` lists every shader plus `neon-tuning.h`, so editing
  any of them re-configures on the next build.

**Adding a shader means updating three places**: the `CMAKE_CONFIGURE_DEPENDS`
list and the `file(READ ...)` list in `lib/CMakeLists.txt`, and
`lib/shaders/shaders.h.in`.

Bulk data reaches the shader three ways:

- **UBOs** (std140): `LoopSamplesBlock` (128 perimeter points),
  `SegmentBlock`, `ArcBlock`. Per-frame values that the shader indexes.
  `ArcBlock`'s `.w` is a bitmask, not a bool: bit 0 is "has own colour
  stops", bits 1 and 2 record whether another arc abuts this one's tail /
  head, which is what picks each endpoint's feather direction. Packed by
  `PackArcFlags` in both renderers.
- **LUT textures**, all baked on the CPU as **RGBA8** - float textures are
  deliberately avoided for edge-device compatibility. Two shapes, and
  confusing them is silent:
  `uGradientLUT` is a 256x1 colour **ring** - `GL_REPEAT` on U (which makes
  hue rotation a single addition), baked with `ColorUtils::SampleRing`.
  `uSegmentLUT` and `uArcLUT` are 128-wide **span** atlases, one row per
  segment / per arc - `GL_CLAMP_TO_EDGE` on both axes, baked with
  `ColorUtils::SampleSpan`, which holds the end colours instead of wrapping.
- **The emission table** (`uEmission`) - the pre-pass's output (§6): `N x 2`,
  `GL_RGBA16F`, `GL_NEAREST`, read only with `texelFetch`. Falls back to
  `GL_RGBA8` where the driver refuses float colour rendering. Allocated once in
  `Initialize` rather than per frame - `N` is `NEON_MAX_LOOP_SAMPLES`, the
  sample-count CEILING, so the size never changes and `numSamples` only bounds
  how much of it the gather reads.

## 8. Animation

Three layers, low to high:

| Layer | File | What it is |
|---|---|---|
| `Modulator` | [`animation/modulator.h`](../lib/include/animation/modulator.h) | Header-only pure functions `time -> float`. `Constant`, `Oscillator`, `Ease`, `Sequence`, `Multiplier`, `Adder`, `Remap`. No coupling to `Config`. |
| `Animation` | [`animation/animation.h`](../lib/include/animation/animation.h) | Modulators plus play state, `PlaybackMode` (LOOP / ONE_SHOT) and `EndAction` (HOLD_CURRENT / HOLD_END / HOLD_START / RESTORE). Two-phase: `Update(dt)` advances, `Apply(cfg)` writes. `AnimationGroup` is itself an `Animation`. |
| `AnimationManager` | [`animation/animation-manager.h`](../lib/include/animation/animation-manager.h) | The attach list. `Update` and `Apply` fan out in attach order. |

Concrete presets live in `animation/neon-animations.h` (`IntensityPulse`,
`ArcWipe`, `SegmentTravel`, `OutlineTracer`, ...). `FieldBoundAnimation`
instead binds modulators to `AnimatableField` / `SegmentField` / `ArcField` /
`ColorStopField` targets at runtime, phase-locked on one shared clock.

Because the effect embeds the manager, hosts do not hand-composite: call
`effect.Attach(anim)` then `anim->Play()`, and each `Update` rebuilds the active
config. There is no "revert to base" end action - `Detach` gives that.

## 9. C ABI

`libedge-lighting-c` ([`lib/capi/`](../lib/capi/)) is a flat `extern "C"`
surface. `edge-lighting-capi.h` is the single public include, aggregating
`el-types.h`, `el-effect.h`, `el-animation.h`, `el-modulator.h`.

- Three opaque handle families - effect, animation, modulator - defined in
  `capi-internal.h`. Attaching an animation does not transfer ownership.
- Each effect handle carries a **staging `Config`**. Every `el_effect_set_*`
  mutates staging and *nothing else*; every `el_effect_get_*` reads staging
  back, *not* the animation-overlaid active config. Staging reaches the
  effect in `el_effect_update`, which is the only place that calls
  `SetConfig` - so a host that sets config and then calls only
  `el_effect_render` renders the previous frame's config. `el_effect_capture`
  re-syncs staging from the effect's base.
- No C++ exception crosses the boundary; everything maps to `el_result_e`.
- Enum ABI parity is enforced by a wall of `static_assert`s at the top of
  `capi-internal.h`. **Reordering or renumbering a mirrored C++ enum means
  adjusting those asserts** - append new values at the end.
- Symbols are hidden by default; only `EL_API`-marked `el_*` functions export.
- **Lifecycle.** `el_effect_create` makes a handle that owns only its staging
  `Config` - no GL state, no renderers - so a host can build it before a context
  exists. `el_effect_init` (the whole stack in the default order),
  `el_effect_init_with_renderers(mask)` or `el_effect_init_with_renderer_order`
  constructs the effect and its layers, and must run on the GL thread with a
  current context. `el_effect_destroy` tears it down.
- **Setters short-circuit unchanged writes** (`SET_AND_LOG` in
  `capi-internal.h`): an unchanged value returns `EL_SUCCESS` without logging,
  so a debug UI's get - widget - set every frame does not flood the log. Getters
  log at `LOG_D`, which `capi-internal.h` compiles out on macOS and Windows.
- **Arrays** (colour stops, segments, arcs, and each one's own stops) are a
  `_count` setter plus per-index accessors: grow with `set_..._count(n + 1)`
  then set index `n`; remove `k` by shifting down and `set_..._count(n - 1)`.
  Both demos do exactly that.
- **Animations** come from `el_animation_create(EL_ANIM_*)` presets, the
  parametric `el_animation_create_*` factories, or
  `el_animation_create_field_bound()` plus `el_animation_add_*_field` over
  modulator handles (`el_modulator_create_*`). Each carries its own play state,
  playhead, speed, duration, mode and end action, and completion /
  state-change callbacks with a `void *userData`. The effect's manager is
  reached through `el_effect_attach_animation` and its siblings; the clock
  through `el_effect_clock_play` / `_pause`.

`demo-capi/`'s include path deliberately excludes `lib/include/`, so it can only
compile against the flat ABI. That is the guard proving the ABI is sufficient
for a real UI-shaped host. It is a hand-maintained fork of `demo/`.

## 10. GL resource handling

[`lib/include/gl/`](../lib/include/gl/) provides move-only RAII wrappers:
`ShaderProgram` (with uniform-location and last-value caching), `VertexArray`,
`Framebuffer`, `UniformBuffer`, `Texture` / `Texture2D`. `gl-header.h` is the
single include for `<glad/glad.h>`. Use these; do not call `glGen*` / `glDelete*`
directly in renderer code.

Two rules that are easy to get wrong:

- **A multi-pass renderer must return to the framebuffer it was handed**, not to
  `BindDefault()`. Snapshot `Framebuffer::GetBoundId()` before the offscreen
  pass and `Framebuffer::BindId(prev)` after. During a frame capture the
  "backbuffer" is a real FBO, and binding the default one sends the composite to
  the window while the capture comes back empty.
- **To grab pixels, use [`util/capture-util.h`](../lib/include/util/capture-util.h)**
  (`OffscreenCapture` plus `CaptureUtil::Read*` / `WritePNG`), never
  `glReadPixels` on the window's default framebuffer: post-swap backbuffer
  contents are undefined and its size follows the platform's HiDPI backing
  scale. (`demo-capi/src/` is the exception - it cannot see `lib/include/`, so it
  has its own minimal helpers in `gl-mini.h`.)

## 11. Coordinate spaces, and the viewport origin

| Space | Origin | +X | +Y | Used by |
|---|---|---|---|---|
| **App** | viewport top-left | right | down | `RectGeometry::position`, `SpotLight::position`, mouse input |
| **Local** | rect centre | right | up | the SDF, `GetPointOnRectangle`, the loop samples, `vPos` |
| **GL window** | viewport bottom-left | right | up | the model translation, `gl_FragCoord` |
| **NDC** | viewport centre | right | up | `gl_Position` |

The app-to-GL flip is one line, `center.y = viewportH - position.y - halfH`, and
every renderer builds its MVP the same way, so rect-local vertices land where
the app placed the rect. (The spotlight keeps app coordinates in its VBO and
flips in its ortho instead.)

**The viewport origin is assumed to be (0, 0).** `BaseRenderer::Render` takes
the viewport's SIZE, never its origin, and that is baked into the shaders, not
just the C++: several read `gl_FragCoord`, which is in window coordinates, against
uniforms the CPU computes as if the viewport started at the window's origin -
`black-rect.frag`'s `gl_FragCoord.xy - uRectCenter` is the plainest case, and
the blit's cut is another. Render into a viewport at `(x, y)` and every such
comparison is off by exactly that, so the fill and the cut draw displaced from
the glow. A sub-viewport is not supported; supporting one means threading an
origin into those uniforms, not restoring the viewport more carefully. The
framebuffer is the opposite case, because it genuinely varies (the default one,
or an `OffscreenCapture` FBO): a multi-pass renderer restores exactly the one
it was handed (section 10).

## 12. The demos

`demo/` opens two GLFW windows sharing one GL context: the render surface and a
floating debug panel (`DebugUI`). Each frame it snapshots the authored config
(`GetConfig`), lets `DebugUI::Build` mutate that copy and attach or drive
animations on the effect, writes it back with `SetConfig` (which reaches the
renderers through `OnConfigChanged` only when something changed), draws the
panel, then on the main context draws any backdrop, `Update(dt)` and
`Render(w, h)`. Sliders showing an animated field read `GetActiveConfig()`, so
the knob follows the live value, and pin to the base while being dragged. The
border colour picker (`border-color-picker.{h,cpp}`) samples an image's border
into colour stops placed on the rect's own perimeter parameterisation.

`demo-capi/` is the same UI against the C ABI alone: its include path omits
`lib/include/`, so an accidental `#include "core/..."` fails to build. Its
`gl-mini.h`, `background-quad.h` and `image-quad.h` are raw-GLAD ports of the
C++ demo's helpers, since the RAII wrappers live under `lib/include/gl/`, and its
debug UI reads every widget through a getter and writes through a setter. It is
a hand-maintained fork: a change to one demo usually needs the same change in
the other.

## 13. Where to change what

| Goal | Touch |
|---|---|
| Tune neon appearance | `neon-tuning.h` and `neon.frag` - one copy, every scale |
| Change the gather loop | `neon-gather.frag` (`gatherPerimeter`) - the loop and its encode; the decode in `neon.frag` changes with it |
| Change what the gather bakes | `neon-emission.frag` **and** `neon-gather.frag` - keep the pre-pass invariant (§6) |
| Add a config field | `config.h` (field **and** `operator==`), the renderer that reads it, `DebugUI`, and the C ABI mirror if exposed |
| Add a shader | `lib/CMakeLists.txt` (two lists) and `shaders.h.in` |
| Add a renderer | `BaseRenderer` subclass, `Config` sub-struct, `main.cpp` registration, `DebugUI` section, `el_renderer_flags_e` bit |
| Reorder a mirrored enum | the `static_assert` wall in `capi-internal.h` |
| Change the demo UI | `demo/src/debug-ui.cpp` **and** its `demo-capi/` counterpart |

Conventions (naming, bracing, the no-em-dash rule) are in
[`AGENTS.md`](../AGENTS.md) and enforced by hand. There is no formatter config
and no test target; the nearest thing is the optional
[`tools/neon-scale-check`](../tools/neon-scale-check/README.md), a regression
check for the neon's resolution scale (`-DEDGE_LIGHTING_BUILD_TOOLS=ON`).
