# Upgrade notes: moving a host from `main`

What a host has to know when it moves from `main` (`1b5cf94`) to this line of
work - the edge ring and the single neon path, the opaque fill's own cutoffs,
the `double` clock and the shared buffer, the fixes of the 2026-10-20
reviews, and the render-target format check.
Six of the changes are silent - the host still compiles and links, and the
picture moves - so read section 0 first (the default resolution scale is now
0.5), the next two before upgrading anything that uses an opaque fill or a
cutoff softness, and section 5 before upgrading anything that lights only part
of the ring. Three are not silent: a host's own renderer stops compiling until
it takes the clock's time as a `double` (section 6), and so do a host's own
`Animation` and `Modulator` subclasses, and anything that used the removed
contour tracer or `PathUtils` (section 7).
Section 8 moves no pixel on a GPU that renders every format the library asks
for, but changes two things a host can see: the library reads and clears the
host's pending GL errors at `Initialize` and on frames that allocate, and the
flare and the spotlight below scale 1.0 draw at 1.0, rather than nothing, when
memory runs out. Section 9: a preset animation now refuses a duration of 0,
which used to fill the config with NaN. Section 10: the effect clamps
out-of-range config values before anything draws. Section 11: unused C++
helpers are gone. Section 12: a failed render-target allocation is final,
out of memory included.

## 0. `NeonConfig::resolutionScale` defaults to 0.5, and 1.0 takes the same path

The neon has ONE path now (I57). The direct path - `resolutionScale` 1.0
drawing the glow straight onto the target with no buffer, blit or edge ring -
is gone, and the default dropped from 1.0 to 0.5.

- A host that never set the scale now renders at 0.5: the glow shaded into a
  half-resolution buffer and blitted back, a ring round the edge at full
  resolution. Within 2/255 of the 1.0 render on every scene in
  [`neon-resolution-scale-comparison.html`](../pages/neon-resolution-scale-comparison.html),
  and cheaper on frames whose config animates (an intensity pulse, an arc wipe,
  a travelling segment). To keep full resolution, set 1.0 explicitly.
- A host that sets 1.0 gets the same passes with a full-size buffer: within 1
  level of what the direct path drew (a rounding step on up to 17% of
  channels, from the extra 8-bit buffer), and holding the reduced buffer at
  full size over the glow's region (up to 8.3 MB for a full-screen rect at
  1920 x 1080).
- The C ABI follows: `el_effect_get_neon_resolution_scale` on a fresh effect
  reads 0.5.

The sentence below about byte-identity at scale 1.0 describes the branch
before this change.

At `resolutionScale` 1.0, as long as no opaque fill was bounded by the glow's
cutoffs, every cutoff softness is 0 and the ring is lit all the way round (one
arc over the whole of it, no segments), the output is byte-identical to `main`.
Seven scenes were compared pixel for pixel on Mesa llvmpipe: the default, both
one-sided glows (one with a 6 px side softness), a square rect, a hairline, an
inside cutoff at softness 0, and an outside opaque fill with no cutoffs.

## 1. The opaque fill has its own cutoffs, and they default to OFF

**Before:** an `OpaqueMode::INSIDE`, `OUTSIDE` or `BOTH` fill was bounded by the
GLOW's cutoffs, `NeonConfig::insideCutoff` / `outsideCutoff`, with one feather
of its own, `NeonConfig::opaqueSoftness`, centred on each boundary.

**Now:** the fill has its own pair, `NeonConfig::opaqueInsideCutoff` /
`opaqueOutsideCutoff`, each with its own softness, and the glow's cutoffs no
longer reach it. Both default to disabled, so a host that relied on the glow's
cutoffs to bound its fill sees it grow:

| mode | with both fill cutoffs disabled |
| ---- | ------------------------------- |
| `INSIDE` | fills the whole rect |
| `OUTSIDE` | runs to the viewport edge |
| `BOTH` | fills the whole viewport |

Measured at scale 1.0 with `BOTH` and glow cutoffs `{15, 4}` / `{25, 4}`: up to
80/255 off `main` over a black backdrop. Over real content the whole screen
turns `opaqueColor`.

To keep the old shape, copy the glow's bounds into the fill's, moving each
size in by half the old feather (see section 2 for why):

```cpp
auto fillCutoff = [&](const Cutoff &glow) {
    return Cutoff{glow.enable, std::max(glow.size - 0.5f * oldOpaqueSoftness, 0.0f), oldOpaqueSoftness};
};
cfg.neon.opaqueInsideCutoff = fillCutoff(cfg.neon.insideCutoff);
cfg.neon.opaqueOutsideCutoff = fillCutoff(cfg.neon.outsideCutoff);
```

```c
el_effect_set_opaque_cutoff(fx, EL_CUTOFF_SIDE_INSIDE, inEnable, fmaxf(inSize - 0.5f * soft, 0.0f), soft);
el_effect_set_opaque_cutoff(fx, EL_CUTOFF_SIDE_OUTSIDE, outEnable, fmaxf(outSize - 0.5f * soft, 0.0f), soft);
```

`NeonConfig::opaqueSoftness` is gone from the C++ config, so C++ hosts get a
compile error at the old field. C hosts keep
`el_effect_set_opaque_softness` / `el_effect_get_opaque_softness`, now
deprecated forwarders in `el-deprecated.h`. They set the feather on both fill
cutoffs and read back the wider of the two, but they do NOT restore the
bounds. The fill's bounds come only from `el_effect_set_opaque_cutoff`.

## 2. `Cutoff::softness` now runs outward from `size`

**Before:** the feather was centred on `size`: full strength at
`size - softness/2`, gone at `size + softness/2`.

**Now:** the feather starts at `size`: full strength there, gone at
`size + softness`. This holds for the glow's cutoffs and the fill's alike, and
at every resolution scale.

Softness 0 is unchanged (its 50% point is still `size`). Any other softness
moves the edge out by `softness/2`. Measured at scale 1.0, `outsideCutoff`
`{20, 8}` differs from `main` by up to 63/255. For the old look, subtract half
the softness from the size:

```cpp
c.size = std::max(c.size - 0.5f * c.softness, 0.0f);
```

## 3. Below scale 1.0 the neon looks different, costs differently, and holds more memory

- **Quality.** A thin ring around the edge is re-shaded at full resolution, so
  the line and every edge near it now look as they do at 1.0. On the comparison
  page's scenes the result is within 2/255 of 1.0 down to scale 0.125, and the
  cut and cutoffs no longer blur. See
  [`neon-resolution-scale-comparison.html`](../pages/neon-resolution-scale-comparison.html).
  Small rects are the exception: a 20 x 17 rect reads 18 / 53 / 93 levels off
  at 0.5 / 0.25 / 0.125, no better than before the ring. Keep small rects at
  1.0.
- **Cost.** The gather - the sample loop, most of the cost - now runs once,
  in its own pass, at a resolution set by how smooth its result is (typically
  about an eighth of the viewport's), and the glow is shaded from it. On Mesa
  llvmpipe the default scene renders about 13.5x faster at 0.5 than at 1.0 and
  about 19x faster at 0.25; `main`'s reduced path, timed on the same machine,
  managed 3.6x and 11.5x.
  The ring, the composite and the extra pass are a fixed cost, so a layer
  already cheap at 1.0 (a tight cutoff band) can run slower below it, and a
  rect small enough to gather at the scale itself (under about 450 px of
  perimeter at 0.5) gains nothing from the split. See
  [`neon-resolution-scale-plan.md`](../design/neon-resolution-scale-plan.md) section 13
  and `neon-resolution-scale-perf-comparison.md`.
- **Memory.** One RGBA8 buffer at the reduced scale, as on `main`, but covering
  only the part of the frame the glow reaches (never more than the viewport at
  that scale), plus a small RGBA16F gather buffer over the same area at the
  gather's scale (two attachments with segments). At 1920 x 1080 and 0.5: 2.15
  MB for a full-screen rect (`main`: 2.1 MB), 1.8 MB for a 900 x 540 one. At
  0.25: 0.6 MB and 0.54 MB. A driver that cannot render to RGBA16F (GLES 3.0
  without `EXT_color_buffer_half_float`) gets RGBA8 instead, logged once, and
  reads up to 3/255 off 1.0 rather than 2.

## 4. Neon shaders are compiled on first draw

`Initialize` now compiles only the emission pre-pass and the opaque fill. The
neon's own programs are built on the first frame: the gather, the shading
twice - once for the reduced buffer and once for the edge ring, so no program
draws two targets in a frame - and the blit; the glow coverage table's bake on
the first frame that needs it (section 5), and the fields' programs on the
first frame of a config they can serve.

- A host creates its effect faster. On Mesa llvmpipe, init plus the first
  frame at 0.25 dropped from ~50 ms to ~40 ms.
- A neon shader that fails to compile no longer fails `Initialize`, so the
  renderer is no longer dropped from the effect. The error is logged once, on
  the first frame. The frame then draws the opaque fill and no glow, and the
  compile is not retried.

## 5. The glow on a partly lit ring looks different

When arcs light only part of the ring, or segments shine on it, each piece of
the outline's halo and bloom is now scaled by that piece's own coverage, read
from a table baked once per config change, instead of by one coverage
averaged around the pixel (V19 and V20 in
[`review-findings.md`](../records/review-findings.md)). What a host will see:

- **No faint outline along an unlit stretch.** The averaged coverage let a dark
  stretch keep some of the lit part's light, a thin line at a narrow
  `glowRadius` and a bright one at a high `intensity`. It is gone.
- **Darker past each arc end**, by up to 60-80 levels right beside it on a
  narrow halo: the halo now ends with the arc, as its own falloff says.
- **Brighter where a lit edge's bloom reaches a dark region**, by up to 17-27
  levels on a small rect with a strong bloom: that light is no longer dimmed
  by the dark piece's coverage.
- **A fully lit ring is unchanged**, bit for bit.

It costs one more offscreen pass and a 1.0 MB RGBA16F table (0.5 MB in RGBA8
on a driver that cannot render to half float) - since I59 sized to the rect and
the halo width, 0.25-0.69 MB for a default glow, 1.0 MB only for a thin glow on
a large rect. V20's version of it - one
table per perimeter, 2 MB - cost ~1.1x on a partly lit ring and ~1.04x on a
fully lit one at 1.0 (~1.2x and ~1.08x at 0.5) on an M2 Pro; V21's, one table
per piece, measures 1.00-1.01x of that at 1.0 and 4-7% faster below it, on an
AMD Radeon Pro 5300M. The pass runs only when the config changes, and not at
all on a ring lit uniformly (one full arc, no segments), which never reads it;
its program is compiled on the first frame that needs it rather than in
`Initialize`, which on the AMD drops from ~8.7 ms to ~4.5 ms. Under an
animation of the arcs or segments it runs every frame: on the AMD, 0.14 ms a
frame in all with one arc animating and 0.27 ms with three arcs and two
segments (0.17 and 0.55 ms before V21), everything a config change costs
included - still worth measuring on a slower GPU before shipping an animated
segment. See V20 and V21 for the measurements.

## 6. The clock is a `double`, and a host's own renderer must take `double time`

`Clock::GetTime` / `SetTime` and the `time` parameter of `BaseRenderer::Update`,
`RenderOffscreen` and `Render` are `double` now (they were `float`). A float
clock stepped by 1/60 s ran slow after half a day of uptime (10 s of frames
moved it 9.375 s) and stopped altogether past 2^19 s (~6.07 days), freezing
the hue rotation, the droplets and the flare's rays.

- **A host renderer (`RendererLayer::CUSTOM`) no longer compiles** until its
  overrides take `double time` - with `override` the compiler says so; without
  it the class is abstract. That is the only source break, and it is C++ only:
  the C ABI has no clock-time function, and no exported signature changed.
- Host code that reads `GetClock().GetTime()` into a `float` still compiles and
  loses the precision again. Keep it in a `double`, and do not hand a shader an
  absolute time: reduce it on the CPU to the phase the shader needs, as every
  library layer now does. For a rate a config or an animation can change, that
  is a `PhaseAccumulator` (`Advance(time, rate, period)` or
  `AdvanceCyclePhase(time, cyclesPerSecond)`, in `animation/clock.h`), which
  integrates the rate - see section 7; `GetCyclePhase` / `WrapPeriod` are the
  constant-rate forms.
- Pixels: none at time 0 (every `neon-scale-check` page image is byte-identical).
  While time runs, the phases are rounded on the CPU instead of the GPU: up to
  2 levels on 0.015% of pixels over the first two seconds of a scene with the
  hue, the droplets and the flare's rays all moving (Apple M2 Pro). The droplets
  hash their rows modulo `DROPLET_HASH_ROWS` so their wrapped time has no seam;
  that is an identity until late in the first ~2.8 h period at speed 1, after
  which the drops are a different (equally random) draw from before.

## 7. The 2026-10-20 review fixes

Three full-tree reviews on 2026-10-20 (`docs/records/review-findings.md`, the
thirty-fourth to thirty-sixth passes) changed the following. The C++ items
marked **source break** stop a host compiling until it follows them.

- **Source break - `Animation` and `Modulator` take `double` time.**
  `Animation::ApplyAt(Config &, double)`, `GetElapsed` / `SetElapsed` and
  `Modulator::Evaluate(double)` were `float`: an animation with a duration of 0
  (`FieldBoundAnimation`'s default) is never wrapped, so its float elapsed time
  ran slow after a day and froze past ~6 days, the clock's bug one layer up
  (I78). A host subclass changes its override's parameter type; with
  `override` the compiler names it. The C ABI keeps `float` and converts.
- **Source break - the contour tracer and `PathUtils` are gone.**
  `util/contour-tracer.h` (`TraceOutermostContour`) and `util/path-utils.h`
  were left over from the removed stroke system and used by nothing in the
  tree.
- **Rate-driven phases are integrated, not `time * rate` (V30).** The neon's and
  the debug strip's hue, the flare's ray spin and the droplets' fall are now
  each a `PhaseAccumulator`. Under a moving rate - a hue-rate animation, a rate
  slider - `time * rate` raced by an amount that grew with the uptime (the
  `HueRotationEaseReverse` preset moved the hue 0.44 turn a frame a minute in);
  now the phase moves at the rate it is given. Pixels: none while a rate holds
  (byte-identical page images). What changes: a rate set to 0 holds the phase
  where it stopped instead of returning it to 0, and a renderer registered
  mid-session starts from `rate * time` while the others have integrated the
  rate's history.
- **HSV and HSL blends toward white, grey or black keep the other stop's hue
  (V33).** They used to take the achromatic stop's hue as 0 (red) and sweep
  toward it - blue -> white peaked at pink. A gradient with such a stop in a
  hue space looks different; RGB gradients, the default, do not move.
- **A colour change made mid-fade is chased, not restarted (V34).** With
  `colorTransitionDuration` above 0, a stop edited every frame now trails it
  closely (the ring ends a one-second drag 9% short instead of 77%); a change
  made from rest still fades with the smoothstep it always had.
- **C ABI: refusals that used to be crashes or silent corruption.**
  - Calls that need an initialised effect return `EL_ERROR_INVALID_HANDLE`
    before `el_effect_init*` instead of dereferencing a null pointer (I79).
  - Every entry point taking a float refuses NaN and infinity with
    `EL_ERROR_INVALID_PARAMETER` (`nullptr` from a factory), and the preset
    animation factories refuse a duration of 0 or less;
    `el_animation_set_duration`, `el_modulator_sequence_append` and
    `el_modulator_create_ease` refuse a negative one (I80).
  - `el_animation_destroy` clears the handle's two C callbacks first, so an
    animation destroyed while attached no longer calls into freed `userData`
    (I89).
- **Smaller behaviour changes.**
  - Animation callbacks (`OnComplete`, `OnStateChanged`) may attach and
    detach animations on the effect or a group while it is updating; this was
    a use-after-free (I77).
  - The droplets' band geometry is rebuilt when `glowSide` changes; it kept the
    old side's strips until something else moved the rect (V31).
  - `CaptureUtil::ReadFramebuffer` / `ReadTexture2D` work under a multisampled
    window, and read into client memory even with a host's pixel-pack buffer
    bound (I85).
  - `SegmentUtils::AcquireSegment` never hands out an id that wraps to 0.
  - The neon's shared buffer ends where pass 1b ends on hue and intensity
    frames too (I76): a reader that does not apply the cut and the cutoffs
    itself no longer sees a line of glow appear past them while the hue
    rotates. No frame pixel moves.
  - The lens flare is dithered (V32, fixed 2026-10-21): every pixel it lights
    moves by up to a level, so a flare frame is no longer comparable bit for
    bit with one from before; two dithered builds still are. Below
    `resolutionScale` 1.0 its buffer is finer than 8 bits, with an RGBA8
    fallback that is logged once (half float then; RGB10_A2 since section 14,
    2.0 MB at 1080p and 0.5).
  - The flare's thin sparkle needles, between its rays, are no longer cut down
    the middle (V36, fixed 2026-10-21): each takes the mean length of the two
    rays beside it. At most about a level, on under 1% of the frame; the main
    rays do not move.
- **Known, still open** (see `review-findings.md`): calling `el_effect_init*`
  a second time drops the attached animations and resets the clock (I91); an
  animation attached to two effects runs at their combined speed (I92); and a
  modulator sequence must not contain itself (I90).

## 8. Render-target formats are asked about first, and the GL error queue is read

The library now asks the driver which formats it can render to before it
allocates anything (`FormatAvailability`, I93), and keeps what each full-size
allocation that failed says. On a GPU that renders every format it asks for -
both Macs - no pixel moves. What a host can see:

- **The host's pending GL errors are read and cleared.** Telling a failed
  allocation from a format the driver will not render takes `glGetError`, so
  the library drains whatever is pending first and logs each error as a
  warning naming it as the caller's (`... was pending before probing format
  ...`, `... was pending before allocating ...`). It does that in
  `EdgeLightingEffect::Initialize`, which now probes five formats (a 4 x 4
  texture and a framebuffer each; 0.18 ms on an M2 Pro, 0.22 ms on an AMD
  5300M) and logs a report of them, and in `Framebuffer::Resize` on any frame
  that allocates: the first frames, a resize of the rect or the viewport, a
  scale change, a layer switched on.
  A host that checks `glGetError` after the effect renders loses its own
  errors on those frames - check before calling into the effect, or read the
  warnings. The C ABI's `el_effect_init*` probe the same way.
- **The flare and the spotlight draw at 1.0 when no scaled buffer can be
  allocated.** Below scale 1.0 they used to draw nothing; they now draw the
  1.0 path, which needs no buffer - the same picture as at 1.0, at 1.0's cost
  (four times the fragments of 0.5), logged once each way. That happens when
  memory is short, so expect it under memory pressure.
- **A failed allocation is final.** A buffer whose full-size allocation
  fails falls to the next format in its list (8 bits for most) and stays
  there for the effect's life; out of memory included (section 12).
- **A driver without half-float targets** lands every buffer on its 8-bit row
  from the first frame, with no failed allocation and no ERROR line, and
  never compiles the hue-invariant fields' programs. The picture is what such
  a driver drew before: within 3 levels of a half-float GPU on every
  `neon-scale-check` scene but one whose segment boosts of 2.0 clamp (~53
  levels).
- **`Framebuffer`** (`gl/framebuffer.h`): `Resize` takes an optional last
  parameter, `GLenum *failure`, saying why it failed; it fails on any GL error
  its allocation raises, not only on an incomplete framebuffer; and it unbinds
  the pixel-unpack buffer around its upload and puts it back. A host that uses
  the wrapper compiles unchanged. `IsColorRenderable` is new, and so is
  `GetAllocationCount`, which moves on every allocation: compare it, not the
  size or format, to know a buffer's contents were replaced.
- **A host's own renderer** compiles unchanged. `BaseRenderer` gains private
  members and a protected method, `GetFormatAvailability`, so rebuild it with
  the library. A host
  renderer with a buffer in a format a driver may refuse can hold it as an
  `AdaptiveFormatTarget` (`core/adaptive-format-target.h`), as the library's
  do - named, filtered and given its fallback cost at construction, sized by
  `Ensure` with a `FormatList` (one of `LIBRARY_FORMAT_LISTS`, or its own
  array), and any cache over it keyed on `GetGeneration` - which moves when
  the target allocates and when it loses its buffer (a walk that ends FAILED,
  or `Release`), so the key alone tells a cache its texels are gone. A format of its own
  outside the library's lists is not in the startup report: ask it from the
  renderer's `Initialize` (`FormatAvailability::Ask`), or the first walk
  probes it mid-frame and clears the frame's pending GL errors.
  `SharedBuffer::Format` is now a typedef of `TextureFormat`, what every
  format list holds, so a host's list still compiles. Drawn outside an effect, such a renderer gets an object of its
  own from `GetFormatAvailability`.
- **`EdgeLightingEffect::GetFormatAvailability()`** is public, for reading
  the answers (`GetAnswers`). Its setters (`SetRefusedFormats`,
  `SetFailedAllocations`, `SetUnknownFormats`, `SetMaxTextureSize`) are
  verification hooks for `neon-scale-check
  formats`: a host never calls them. Nothing changed in the C ABI.

## 9. A preset refuses a duration of 0 (I104)

An oscillator preset handed a 0 s cycle built its oscillator at a frequency of
infinity and wrote NaN into the config on every frame. The 13 presets that
build a modulator from their duration (`IntensityPulse` to `ArcWipe` - every
one but the `*Curve`s) now take only a duration of at least 1e-6 s
(`PositiveDurationAnimation::MIN_DURATION_SECONDS`) - their C factories took only
one above 0, which let 1e-40 s, whose reciprocal is infinity, through:

- **C ABI.** `el_animation_set_duration` on such a preset returns
  `EL_ERROR_INVALID_PARAMETER` for 0 - or anything under 1e-6 s - and leaves
  the animation as it was. 0 on
  a field-bound animation (`el_animation_create_field_bound`,
  `el_animation_from_modulator`) is still accepted: it opts out of the wrap.
- **C++.** `Animation::SetDuration` returns `bool` - false for a duration the
  animation refuses (`Animation::AcceptsDuration`), which changes nothing. A
  call that ignored the old `void` still compiles, though a host linking the
  static library has to rebuild: `Animation` gained a virtual. A host's own subclass that
  builds a modulator from its duration should override `AcceptsDuration`, or
  derive from `PositiveDurationAnimation` (`animation/neon-animations.h`).
  A preset constructed with 0 holds its cycle's start rather than write NaN.

## 10. Out-of-range config values are clamped before anything draws (I105-I108, V38)

Finite but absurd values used to reach the renderers: a spotlight throw of
1e10 px made `SetConfig` (and `el_effect_update`) never return, a rect whose
negative width outweighed its height hung the GPU, and a 1e10 px rect hit
undefined behaviour. The effect now holds the composited config - base plus
animation overlays - to `ConfigValidation`'s ranges before any renderer sees
it:

- **Lengths and positions** to +/-1e5 px, a **rect's width and height** to 0
  or more, **every other float** to +/-1e6, `gradientLutSize` to 2048, and
  **NaN** to 0. Every bound is far outside any value a host means, so no
  in-range config draws differently.
- **`GetActiveConfig` returns the clamped values; `GetConfig` and the C ABI
  getters return what was set.** A UI slider bound to the active config shows
  the clamp.
- **A host driving a renderer without `EdgeLightingEffect`** should call
  `ConfigValidation::Sanitize` on what it hands over. The renderers carry
  local guards against the hangs, not a range check of every field.

Nothing in the C ABI changed: its setters still accept any finite value.

## 11. Unused C++ helpers are removed

Nothing in the library, the C ABI, the demos or the tools called these, so
they were removed on 2026-11-13. A C++ host that did no longer compiles:

- `GLUtils::CheckExtension`, `GLUtils::CheckGLError` and its `GL_CHECK_ERROR`
  macro, `GLUtils::LogExtensions`;
- `Framebuffer::BindDefault` - return to the framebuffer you were handed
  (`Framebuffer::GetBoundId` / `BindId`), never to 0;
- `Texture2D::SetDataFromFile`, and the `Texture` base class, folded into
  `Texture2D` (`gl/texture.h` is gone);
- the presets `GlowRadiusCurve`, `BloomStrengthCurve` and
  `HueRotationRateCurve` - a `FieldBoundAnimation` over the field does the
  same;
- `FieldBoundAnimation`'s binding getters, `GetBindingCount` and
  `ClearBindings`.

`EdgeLightingEffect::Initialize` also logs the GL vendor, renderer, version
and extension count instead of one line per extension.

## 12. A failed allocation is final, out of memory included

Until 2026-11-14 a buffer that ran out of memory fell back, waited 2 s of
frame time (doubling with each retry that failed again, up to 512 s) and was
tried again in its better format, and every renderer moved each of its buffers
back up once a frame. That is gone: a full-size allocation that fails, for any
reason, holds that format at that attachment count and above for the effect's
life. What a host can see:

- **After a real out-of-memory failure** the format that failed is off for
  every buffer with that many attachments or more, not only the one that
  failed: a buffer already holding it is reallocated in its 8-bit row the
  next time it is written. Measured with a forced failure of the flare's
  RGBA16F buffer mid-run: the neon's shared buffer and gather buffer, both
  holding RGBA16F, went to RGBA8 on the next frame, and the frames after were
  up to 3 levels from a run with no failure. With the neon's shared buffer on
  8 bits its output dither no longer hides the contour rings in the glow's
  dark tail (R7), and the hue-invariant fields stop for good after one failed
  bake. A layer whose every row ran out of memory draws without it - the neon
  drops to the fill, the flare and the spotlight draw at 1.0. All of it holds
  until the effect is created again: destroying and recreating the effect (or
  `el_effect_destroy` and a new `el_effect_create`) is how a host that knows
  memory came back gets the better formats back. On a GPU that renders every
  format and never runs out, no frame changes: the gates' 582 images are
  byte-identical.
- **C++:** `FormatAvailability::Advance`, `GetTime`, `SetOutOfMemoryRetry`,
  `OUT_OF_MEMORY_RETRY_SECONDS` and `OUT_OF_MEMORY_RETRY_MAX_SECONDS`,
  `GetFailedAllocations`, `AdaptiveFormatTarget::Refresh`,
  `SharedBuffer::Refresh` and `BaseRenderer::AdvanceOwnFormatAvailability`
  are removed; `ForcedFailure` loses `fromTexels` and `count`. A host renderer
  that called `Refresh` or `AdvanceOwnFormatAvailability` drops the call.
  Nothing changed in the C ABI.

## 13. `SetConfig` no longer calls the renderers

`EdgeLightingEffect::SetConfig` used to notify every renderer before it
returned, and each rebuilt what the change moved - LUT bakes, uniform blocks,
vertex buffers - on the spot: GL work on whatever the host was doing when it
set the config, once per call.

- **Now** `SetConfig` writes the base and recomposes the active config
  (`GetActiveConfig` follows at once, as before) and nothing else. The
  renderers hear of the change once, from the next `Update` - or the next
  `Render` when no `Update` comes first - however many `SetConfig` calls came
  in between.
- **What a host sees.** Nothing in the frames: a host that calls `Update` then
  `Render` draws what it drew. Three `SetConfig` calls in a frame cost one
  rebuild, not three.
- **A host's own renderer** (`RendererLayer::CUSTOM`) gets `OnConfigChanged`
  from `Update` or `Render`, not from inside `SetConfig`; code that read state
  it rebuilt there right after `SetConfig` reads it after `Update` now. A
  change set and set back before the next `Update` may still notify once, with
  a config equal to the last one.
- **The C ABI is unchanged**: staging already reached the effect only in
  `el_effect_update`.

## 14. The reduced buffers are ten bits a channel, and the glow field is given back (I109, I110)

The neon's reduced buffer - its shared buffer, the whole viewport at
`resolutionScale` - and the lens flare's scaled buffer are `GL_RGB10_A2`, where
they were `GL_RGBA16F`: 4 bytes a texel in place of 8. Both hold finished
colour in [0, 1], which needs no float, only more than 8 bits for the blit's
dither. Textures, Apple M2 Pro:

| scene | 1080p | 4K |
| --- | --- | --- |
| production-style band (glow outside, 20 px cutoff), neon at 0.5 | 4.40 -> 2.42 MB | 17.18 -> 9.27 MB |
| mid rect, 3 arcs + 3 segments, hue rotating | 6.75 -> 4.77 MB | 22.35 -> 14.43 MB |
| neon at 1.0, mid rect | 19.43 -> 11.52 MB | 72.39 -> 40.75 MB |
| every layer, flare and spotlight at 0.5 | 14.66 -> 10.71 MB | 53.99 -> 38.17 MB |

What a host can see:

- **Frames move by at most 1 level** (alpha by 2) against the build before, on
  about a sixth of the lit pixels: the buffer now rounds to a quarter level
  before the blit's dither, where half float rounded far below it. Measured
  on the 306 `cover` frames (100% of lit pixels within 1); `check` stays
  inside every bound. A frame from before and one from after are not
  comparable byte for byte.
- **Over a coloured background, a few pixels move further.** On 49 of 1,494
  random-config frames (`fuzz`), up to 0.02% of the lit pixels moved by 2 to
  5 levels: where the hue changes within one buffer texel, the coverage the
  blit rebuilds - the brightest channel of the filtered colour - is slightly
  less than the stored alpha was (the filtered brightest channel), so the
  background shows through by that much more. Neither consistently closer to
  nor further from the 1.0 frame. Figures:
  [`ten-bit-buffers-visual-check.md`](../design/ten-bit-buffers-visual-check.md).
- **A driver without half-float render targets** (GLES 3.0 with neither
  `EXT_color_buffer_half_float` nor `EXT_color_buffer_float`) used to put the
  reduced buffer on RGBA8, where the dark tail's contour rings come back (R7).
  RGB10_A2 is colour-renderable in GLES 3.0 with no extension, so that driver
  now keeps ten bits there. The startup report has a row for it.
- **A reader of the neon's shared buffer must not read its alpha.** RGB10_A2
  keeps 2 bits of it. The coverage is the brightest colour channel - every
  pass that writes the buffer writes exactly that - so a reader rebuilds it as
  `max(r, g, b)` after its fetch, as `neon-blit.frag` does, and declares its
  sampler `highp` (GLSL ES gives `sampler2D` lowp by default). The library's
  own readers do; a host's custom renderer that read `.a` has to change.
- The spotlight's scaled buffer stays RGBA16F: it screens alpha separately, so
  its coverage is not the brightest channel.

The glow's hue-invariant field (0.8 MB at 1080p for a 960 x 540 rect, up to
6.7 MB at 4K for a full-screen two-sided glow with segments) is released once
no frame has used it for 5 s of frame time. It was kept for the effect's life
after an intensity animation stopped with the hue still, because no config
change follows the last moving frame. The frame that next wants it bakes it
again; no pixel moves.

## 15. Nothing else a host can see moved

The pass schedule now runs every offscreen pass before it touches the caller's
framebuffer. This changes no pixel, but saves a store and reload of the target
on tile-based GPUs when an opaque fill is on. The blit and the edge ring now
cover only where the glow can be lit. This changes at most 1/255 on a few dozen
pixels from rounding, and saves up to 97% of the composite's fragments on a
cutoff band. See [`neon-resolution-scale-plan.md`](../design/neon-resolution-scale-plan.md)
section 12.
