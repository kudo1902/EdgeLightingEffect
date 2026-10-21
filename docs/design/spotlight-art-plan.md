# Light framed artwork: smoke, beam terms and art protection

**Status: proposal, not built.** Nothing here is in the library yet. This
works the ideas of the phase-0 art spotlight mockup out against
`SpotlightRenderer` as of `54a7479`, and orders them into phases. Section 8
lists the decisions it needs, and section 9 the questions only the mockup's
source can answer. Every cost below marked "est." is an estimate, not a
measurement.

The mockup is a single-file WebGL page (1920 x 1080 canvas, Vietnamese UI)
that previews spotlights, smoke and artwork protection on framed art, with the
lamp ported from `spotlight.frag`. It is not in this tree. Its summary page,
with this plan and an interactive compositor bench (section 3), is the artifact
<https://claude.ai/artifact/5JE5nduaZBNn2SeWX26je5>; every case in section 4
is drawn live, as a WebGL draft over a framed painting, in
[Art Spotlight Drafts](https://claude.ai/artifact/VWWXQAdwEVNYMVYKvisv7F); the companion
[Spotlight Flare Studies](https://claude.ai/artifact/XCrMnSCzxk2EYAqFGNDnRM)
covers per-lamp lens flare. Both are private until their owner shares them.

## 1. What the mockup does

Four passes:

1. **Art plane.** Procedural artwork plus the mat, frame and mount shading.
   Raised mounts get a drop shadow; recessed mounts get bevels with mitred
   corners.
2. **Smoke.** Density at quarter resolution, from fBm noise with warp and
   drift.
3. **Effect layer.** Up to 8 lamps. Each can draw a cone beam, rays, laser
   edge lines, particles and a glitter pool, and lens flare. Mount shadows
   apply to the beams. Art protection applies after the sum of the lamps.
4. **Composite.** Emulates a TV compositor and handles the view modes.

Eight artwork types (nebula, oil still life, portrait, bright pastel,
black-and-white, neon poster, gold stage, red laser club) and eight presets,
each with a mount mode, lamps, smoke, rays, particles and flare, and a risk
note: the gold stage warns about hot spots spilling onto the art, the laser
preset about red lines crossing it.

Per lamp: position, angle, beam angle, throw length, aperture width, softness,
intensity, bloom, spread falloff, colour temperature, tint, and a sweep
(amplitude, frequency, phase), with toggles for rays, flare, particles and
edges. Controls: drag a lamp to move it, shift-drag to aim it; mount raised or
recessed; protection on or off; a split compare (left unprotected, right
protected); view modes for the result, the effect layer alone, its alpha, the
smoke density and the protection mask. **Export config** writes JSON with
C++-style field names as the handoff to the library.

## 2. At a glance

| idea | verdict | phase | size | section |
| ---- | ------- | ----- | ---- | ------- |
| Device composite view and compositor probe | build first | 0 | S | 4.1 |
| Art protection: mask and feather | try now, no code (`ClipArea`) | 1 | - | 4.2 |
| Art protection: residual light and shade | build after phase 0 | 1 | M | 4.2 |
| Art protection: brightness cap | only if residuals fall short | 1 | M below scale 1, L at 1 | 4.2 |
| Mount shadows | build if a lamp height is acceptable (D4) | 2 | M | 4.3 |
| Smoke in the beams | build, from a noise texture | 2 | M | 4.4 |
| Rays | build | 2 | S | 4.5 |
| Lens flare per lamp | build studies F and A | 3 | M | 4.8 |
| Laser edge lines | opt-in look | 4 | M | 4.6 |
| Particles | opt-in, pins scale 1.0 | 4 | M | 4.7 |
| Glitter pool | hold: needs a floor | 4 | L | 4.7 |
| Smoke as visible haze on its own | hold | - | - | 4.4 |
| View modes | with phases 0 and 2 | 0, 2 | S | 4.9 |
| Exported config | alongside | 5 | M | 4.10 |
| Art plane, mats, bevels, drop shadow | host content | - | - | 4.11 |

S is days, M about a week with its checks, L open-ended.

## 3. The compositor decides protection

### 3.1 What the layer writes

`spotlight.frag` writes premultiplied light, `lit`, with alpha set to its
brightest channel, and `UseScreenBlend` screens both onto the target. In the
C++ demo the background quad is drawn into the same opaque framebuffer first
(`demo/src/main.cpp`), so the light screens over the art and can only brighten
it. On the device the surface is blended over the video plane by its alpha.

### 3.2 Two models for the device

The 2026-09-17 bug, where the layer vanished over video while it wrote alpha 0,
fits two blend models:

```text
premultiplied over:  out = ui.rgb + video * (1 - ui.a)        (colour may be clamped to alpha)
straight alpha:      out = ui.rgb * ui.a + video * (1 - ui.a)  (the formula in spotlight.frag's comments)
```

Under premultiplied over without a clamp, alpha 0 would have ADDED the light,
which is why it has to be one of the other two. They differ a lot for light
over bright content.

### 3.3 Worked numbers

A painting pixel (0.6, 0.5, 0.4) under three lights, `cov` = brightest
channel:

| light | C++ demo (screen) | device, premultiplied | device, straight alpha |
| ----- | ----------------- | --------------------- | ---------------------- |
| warm core (0.5, 0.4, 0.29) | 0.80, 0.70, 0.57 | 0.80, 0.65, 0.49 | **0.55, 0.45, 0.35** |
| dim tail (0.1, 0.08, 0.06) | 0.64, 0.54, 0.44 | 0.64, 0.53, 0.42 | **0.55, 0.46, 0.37** |
| red laser (0.9, 0.05, 0.02) | 0.96, 0.53, 0.41 | 0.96, **0.10, 0.06** | 0.87, **0.10, 0.06** |

The artifact's bench sweeps the same three composites across a strip of
artwork, with the library's highlight shoulder: tungsten at peak 0.7 over the
bright pastel art leaves 83% of the lit columns darker than the art even
premultiplied (blue down by up to 0.16), and 100% under straight alpha.

### 3.4 What follows

- **Under straight alpha, light darkens the art** wherever the beam is dimmer
  than the art: the whole soft tail of every beam.
- **Under either model, coloured light strips the art's other channels.**
  Alpha follows the brightest channel, so a red laser at coverage 0.9 removes
  90% of the art's green and blue - the laser preset's risk - and a tungsten
  beam lowers a bright painting's blue.
- **Protection works mainly through alpha.** Lowering coverage over the art
  while keeping the colour leaves the light visible and the art's own colour
  under it - on an unclamped premultiplied compositor only (4.2).
- **The C++ demo cannot judge any of this.** Its window is opaque and nothing
  reads its alpha.

### 3.5 Settling it on the device

Four probe patches, written with blending off, over a full-screen 50% grey
video. Beside each probe, opaque reference patches at each model's prediction;
the reference the probe blends into names the model. Matching neighbouring
patches by eye needs no capture card.

| patch | writes rgb, a | premultiplied | premultiplied, clamped | straight alpha | tells |
| ----- | ------------- | ------------- | ---------------------- | -------------- | ----- |
| A | 0.50, 0.50 | 0.75 | 0.75 | 0.50 | premultiplied or straight |
| B | 0.50, 0.00 | 1.00 | 0.50 | 0.50 | whether colour above alpha survives |
| C | 0.25, 0.50 | 0.50 | 0.50 | 0.375 | confirms A |
| D | 0.00, 0.50 | 0.25 | 0.25 | 0.25 | 0.25 if it blends encoded values, about 0.36 if it blends in linear light |

## 4. Ideas in detail

Fragment costs are against the measured baseline: one default lamp shades
183,868 fragments at 1280 x 720, eight lamps about 1.9 million
([`spotlight-renderer-plan.md`](spotlight-renderer-plan.md)).

### 4.1 Device composite view and compositor probe

- **Model.** The demo renders the effect into an `OffscreenCapture` cleared to
  (0, 0, 0, 0), draws the background, then composites the capture over it with
  a fullscreen shader. Modes: *Desktop* (today's path, drawn straight in),
  *Premultiplied*, *Premultiplied, clamped*, *Straight alpha*; and two
  inspection views, the layer over black and its alpha as grey. The probe is
  library code, so it runs on the device: `DebugConfig::showCompositorProbe`
  makes `DebugRenderer` draw patches A to D of 3.5 and their references with
  blending off.
- **Lands in.** `demo/src/composite-view.{h,cpp}` (new), a Composite combo in
  `DebugUI`, the render loop in `demo/src/main.cpp`; `demo-capi` mirrors it
  with its own framebuffer in `gl-mini.h`, since it cannot see `lib/include/`.
  The probe: a `DebugConfig` field, a pass in `DebugRenderer`,
  `el_effect_set_debug_show_compositor_probe`.
- **Cost.** Demo only: one viewport-sized RGBA8 texture and one fullscreen
  pass. The probe costs nothing while off.
- **Risks.** The demo's backgrounds stand in for video; a uniform grey test
  video has to come from the device host.
- **Check.** In Desktop mode the frame is byte-identical to today's; in each
  device mode the probe reads the values of 3.5.

### 4.2 Art protection

Beams should still read as passing over the artwork while the painting keeps
its own colours and contrast.

**Level 0, today.** `clipArea` = the artwork rect, `mode = KEEP_OUTSIDE`,
`edgeSoftness` as the feather, `clipped = true` on every lamp: mask plus
feather with no new code. Its limits: light inside goes to zero, it is per
lamp, it pins `resolutionScale` to 1.0 (`GetClampedSpotScale`), and there is
no shade.

**Level 1.** One artwork area for the whole layer, with independent amounts
for colour and coverage:

```text
lit = shoulder(colour * I * (cone + bloom)) * clip   // as today
m   = artworkMask(vApp)                              // 1 on the art, feathered
cov = maxChannel(lit) * mix(1, protectAlpha, m)
lit = lit * mix(1, protectColor, m)
```

**Shade** is a quad over the artwork, drawn after the lamps with the same
screen blend, writing (0, 0, 0, `shade * m`). It raises alpha only: on the
device it lays a black veil over the art, on the desktop it does nothing -
which is why it needs 4.1 to be judged. Whether the mockup's shade term is
this is question 9.1.

*Where it runs.* At scale 1.0 per lamp in `spotlight.frag`, reusing the clip's
rounded-box code. Below 1.0 in `spotlight-blit.frag`, after the bilinear read,
at full resolution: there it is exactly "after the sum", and its edge stays
sharp. The clip pins the scale because it is per lamp (I21 in
[`review-findings.md`](../records/review-findings.md)); a protection that
covers every lamp does not need to, since the blitted buffer holds exactly the
lamps it applies to.

*Per lamp against after the sum.* Per lamp gives `1 - prod(1 - m * a_i)`,
after the sum `m * (1 - prod(1 - a_i))`. They agree wherever `m` is 0 or 1;
inside the feather two lamps differ by `m (1 - m) a b`, at most `a b / 4`.

*Fields:*

```cpp
typedef enum class MountMode { FLUSH, RAISED, RECESSED } MountMode;

typedef struct SpotArtwork
{
    glm::vec2 position = glm::vec2(0.0f); ///< Top-left, app px.
    float width = 0.0f;                   ///< 0 = no artwork; everything below is inert.
    float height = 0.0f;
    float cornerRadius = 0.0f;
    float edgeSoftness = 16.0f;           ///< Feather of the protection, px.
    float protectColor = 1.0f;            ///< Share of light colour kept on the art, [0, 1].
    float protectAlpha = 1.0f;            ///< Share of coverage kept on the art, [0, 1].
    float shade = 0.0f;                   ///< Black veil over the art, [0, 1]. Shows only on a transparent surface.
    MountMode mount = MountMode::FLUSH;   ///< 4.3.
    float mountDepth = 0.0f;              ///< px.
    bool operator==(const SpotArtwork &o) const; // every field
} SpotArtwork;

// SpotlightConfig gains:
SpotArtwork artwork;
```

Unlike `ClipArea`, a zero-size artwork protects nothing, so the defaults are
inert and every existing rig renders as before. C ABI setters and
`AnimatableField` entries for the three amounts, so a host can fade
protection in; `MountMode` gets an `el_*` mirror and its static_asserts.

**Level 2, a cap.** A ceiling on what the art receives needs the summed layer.
Below scale 1.0 it fits in the blit with no extra pass. At 1.0 it needs an
offscreen buffer and a blit: one lamp at 720p goes from 184k fragments to
about 1.1 million, the blit alone being ~922k.

**Scope.** Flare ghosts and the neon glow can cross the art too. `ClipArea`
was made a shared shape for this, so each layer can take the artwork mask at
its own last step. One multiply pass over the art at the end of
`EdgeLightingEffect::Render` would also work on the device's transparent
surface, but would darken the host's own content on any host that draws into
the same framebuffer first - the demo does.

- **Cost.** One rounded-box distance and a smoothstep per strip fragment at
  1.0 (est. ~15 operations), nothing extra in the blit, plus the shade quad's
  artwork-sized area with a trivial shader.
- **Risks.** `protectAlpha` below `protectColor` leaves colour above alpha,
  which only an unclamped premultiplied compositor shows; the others cut it.
  Any protection that reads the art's pixels cannot be ported: the library
  cannot read the video plane.
- **Check.** In the composite view, per preset and art type: the mean change
  inside the art, and the share of art pixels darker than the art alone. With
  `width` 0, byte-identical to `main`.

### 4.3 Mount shadows

Raised mount: the panel's shadow falls on the wall on the side away from each
lamp, and the panel face stays lit. Recessed: the frame's inner edge nearest
the lamp shades a band of the art.

Each lamp needs a height above the wall, `zL` px toward the viewer, and the
mount a depth `h`. The shadow is the mount's outline projected from the lamp
onto the receiving plane:

```text
// raised: the panel face sits h px off the wall
k        = zL / (zL - h)
dFace    = sdRoundBox((p - L) / k + L - c, half, r) * k   // face projected onto the wall
shadow   = inside(dFace, penumbra) * outside(dPanel)
penumbra = 2 * apertureWidth * h / (zL - h)

// recessed: the art sits h px behind the opening
q        = p + (L - p) * h / (zL + h)                      // p's ray to the lamp, at the opening
lit      = inside(sdRoundBox(q - c, halfOpening, r), penumbra)
penumbra = 2 * apertureWidth * h / zL
```

The penumbra comes from the lamp's own size (`apertureWidth` is a
half-width), so the shadow's softness needs no control of its own.

- **Lands in.** `SpotLight::height` (new; 0 = no shadows), `SpotArtwork::mount`
  and `mountDepth`. The term multiplies the cone in `spotlight.frag`, not the
  bloom, which is glare at the lens. Height rides in a new flat `vec4 aP2`:
  `StripVertex` grows from 17 to 21 floats and from 7 to 8 attribute
  locations, of the 16 GLES 3.0 guarantees.
- **Cost.** One more rounded-box distance per strip fragment (est. ~15
  operations). No geometry change: the term is at most 1, so
  `SolveConeAcross` still bounds the strip.
- **Risks.** A third dimension in a rig that is 2D on purpose (D4). A narrow
  penumbra drawn at scale 0.5 blurs as the clip edge did (up to 65/255
  measured), so floor the penumbra at 2 / scale px or pin 1.0 while shadows are
  on. The mount's drop shadow from room light is host content: a light layer
  can only withhold light.
- **Check.** Shadow edges against the projection for a few lamp and mount
  layouts; height 0 byte-identical.

### 4.4 Smoke

Beams break up and drift where they pass through haze; the dark room stays
dark. A density in [0, 1], anchored to the room so a moving lamp sweeps
through still smoke:

```text
uv    = vApp / scale
w     = N(uv * 0.5 + phase.zw).rg - 0.5                    // slow warp
rho   = N(uv + warp * w + phase.xy).b
rho   = smoothstep(0.5 - contrast / 2, 0.5 + contrast / 2, rho)
cone *= 1 - amount * (1 - rho)                             // in [1 - amount, 1]
```

The bloom is left alone: it is glare in the lens or eye, not light in the air.

- **A texture, not fBm.** The mockup's warped fBm costs about 12 to 15
  value-noise evaluations per fragment (est.). A tiling 256 x 256 RGBA8
  texture baked once in `Initialize` (256 KB, an octave per channel) costs two
  or three reads, and keeps to the library's RGBA8-only rule.
- **No extra pass.** The mockup builds density in a quarter-resolution pass of
  its own. Read inside the strip fragments instead, smoke costs nothing where
  no lamp shines and needs no buffer; it is smooth, so the scaled path keeps
  it.
- **Time.** Drift and evolution are rates integrated by `PhaseAccumulator`s and
  wrapped by the texture's tile, seamless because the texture tiles - never
  `time * rate`. One uniform, `uSmokePhase`.
- **Fields.** `SpotlightConfig::smoke`: `amount` (0 = off, and a uniform branch
  skips the reads), `scale` (px per tile), `drift` (px/s), `warp`,
  `evolveRate`, `contrast`.
- **Cost.** Two or three reads and ~10 operations per strip fragment (est.),
  every frame while it drifts.
- **Risks.** Smoke as visible haze on its own would cost a full viewport every
  frame and lift the blacks over the art: hold it (D5). Repetition at large
  scales: mix two tile sizes.
- **Check.** `amount` 0 byte-identical; a run started at about 10 days draws
  the frames a run from 0 draws, as `neon-scale-check clock` checks the other
  layers.

### 4.5 Rays

Streaks fanning out from the aperture inside the cone, shimmering slowly.
`across / halfW`, which the shader already computes for the gaussian, is an
angle across the cone, so the streaks need no `atan`:

```text
lat   = across / halfW
n     = valueNoise1D(lat * rayCount + rayPhase)            // [0, 1]
fade  = smoothstep(1.0, 3.0, halfW / rayCount * scale)    // streaks under ~2 px fade out
cone *= 1 - rays * fade * (1 - n)
```

A streak is `halfW / rayCount` px wide, under a pixel near the aperture; the
fade removes it there rather than letting it alias.

- **Lands in.** `SpotLight::rays` (depth, 0 = off, per lamp as the mockup's
  toggles are) in `aP2`; `SpotlightConfig::rayCount` and `rayRate` (turns per
  second, through a `PhaseAccumulator`).
- **Cost.** ~12 operations per strip fragment (est.). At most 1, so the solve
  does not change.
- **Check.** `rays` 0 byte-identical; watch a high rate for flicker.

### 4.6 Laser edge lines

Thin bright lines along both edges of the cone, as a stage laser draws:

```text
e    = abs(abs(across) - halfW) * cosHalfBeam             // px from the nominal edge
line = edgeIntensity * exp(-max(along, 0) / edgeThrow)
     * (1 - smoothstep(w - 0.5, w + 0.5, e))               // closed-form AA, no fwidth
lit  = colour * I * (cone + line + bloom)
```

`spotlight.frag`'s rule that no setting gives the cone an edge still holds;
the line is a separate term, off by default.

- **Strip.** The line adds light with a reach of its own, so `SolveConeAcross`
  returns the larger of the cone's half-width and `halfW + w + 1` wherever the
  line is above the floor, and `SolveConeReach` takes the line's reach.
- **Lands in.** `SpotLight::edgeIntensity` (0 = off), `edgeWidth`, `edgeThrow`.
  `GetClampedSpotScale` pins the scale to 1.0 while any lamp has edges, as for
  a clipped lamp: 1 to 2 px lines blur at 0.5.
- **Risks.** On the device a saturated line strips the art's other channels
  (3.4), so it has to sit under protection.
- **Check.** The strip-clipping grid check described in `buildStrips`, with
  edges on.

### 4.7 Particles and glitter pool

*Particles:*

```text
cell = floor((vApp + drift) / cellSize)                    // room-anchored, two layers
s    = sparkle(hash(cell), twinklePhase)                   // 0 almost everywhere
lit  = colour * I * (cone * (1 + gain * s) + bloom)        // the solve uses I * (1 + gain)
```

Motes exist only where light passes. The gain is folded into the solve's
intensity, as a boosted tint is (`DeriveLamp`'s `solveIntensity`). Pixel-sized
and moving, so they pin the scale to 1.0 and need a density ceiling and a
minimum size of about 1.5 px against shimmer. Fields: `SpotLight::particles`,
`SpotlightConfig::particleSize`, `particleRate`. Est. ~30 operations per strip
fragment for two cells.

*Glitter pool:* sparkle where a beam lands on a surface. The rig has no floor,
so it needs a floor line or the artwork's bottom edge to land on; the Flare
Studies dropped floor sheen for the same reason. Hold until a floor is wanted.

### 4.8 Lens flare per lamp

As the Flare Studies recommend: study F (lens face) and study A (starburst)
first - `SpotLight::flare` in [0, 1], one `SpotFlareConfig` for the lens, and
sprites in a new `spotlight-flare.frag` drawn at full resolution in `Render`
after the blit. `LensFlareRenderer` is not the base: it is a fullscreen pass
per sun.

The studies keep the clip area off the flare, since a flare happens in the
lens. The mockup protects after the sum, which includes the flare. Proposed
(D6): the flare honours the artwork protection and ignores the clip area, so
stars and ghosts stay off the painting.

Check: no pixel with colour and zero alpha; `flare` 0 byte-identical.

### 4.9 View modes

Result, layer only and layer alpha belong to the demo's composite view (4.1).
Protection mask and smoke density need the renderer:
`DebugConfig::spotlightView` (`NONE`, `PROTECTION_MASK`, `SMOKE_DENSITY`)
picks what `spotlight.frag` writes. The split compare renders twice in the
demo, protection off on the left.

### 4.10 Exported config

- **Names.** The lamp fields already match `SpotLight` one to one:
  `position`, `angle`, `beamAngle`, `throwLength`, `spreadFalloff`,
  `apertureWidth`, `softness`, `intensity`, `bloom`, `bloomRadius`,
  `colorTemp`, `tint`, `enable`, `clipped`.
- **Sweep is an animation, not a lamp field.**
  `angle + amplitude * sin(2 pi (frequency * t + phase))` is exactly
  `Oscillator(frequency, angle - amplitude, angle + amplitude, phase)` bound by
  `FieldBoundAnimation::AddSpotlightField(lamp, SpotlightField::ANGLE, ...)`,
  since the sine oscillator starts at the middle of its range.
- **Scaling.** Pixel fields are canvas pixels. On a viewport of another size
  the loader scales positions per axis, and lengths, radii, depths, the smoke
  scale and drift by height. Angles, intensities, Kelvin and tints stay.
- **Loader.** The library has no JSON reader. The demo needs one (D7); the
  device host maps the same file onto the C ABI setters. The field names and
  the `schema` number are the contract.

```json
{
  "schema": 1,
  "canvas": [1920, 1080],
  "spotlight": {
    "resolutionScale": 1.0,
    "lamps": [{ "position": [640, 72], "angle": 90, "beamAngle": 26,
                "throwLength": 700, "colorTemp": 3000, "tint": [1, 1, 1],
                "height": 0, "rays": 0, "edgeIntensity": 0, "particles": 0, "flare": 0 }],
    "artwork": { "position": [452, 252], "width": 1016, "height": 576,
                 "protectColor": 1, "protectAlpha": 0.4, "shade": 0,
                 "mount": "RAISED", "mountDepth": 24 },
    "smoke": { "amount": 0.4, "scale": 512, "drift": [6, -2] }
  },
  "animations": [
    { "lamp": 0, "field": "ANGLE",
      "modulator": { "type": "oscillator", "waveform": "SINE",
                     "frequency": 0.12, "min": 78, "max": 102, "phase": 0 } }
  ]
}
```

### 4.11 Art plane, mats, bevels and drop shadow

Host content. On the device the artwork is the video or image plane under the
UI; the library draws light only and cannot see it. The mockup's art plane
stays as the test set - its eight types cover dark, mid, bright, monochrome
and saturated, the range protection has to handle - as long as no protection
term in the mockup reads art pixels (question 9.2).

## 5. One fragment after phases 1 to 4

```text
cone  = gaussian * nearFade * throw * spread                 // today
cone *= rays * smoke * mountShadow                           // phase 2, each in [0, 1]
lit   = colour * I * (cone * (1 + gain * s) + line + bloom)  // particles, edges: phase 4
lit   = shoulder(lit) * clip                                 // today
cov   = maxChannel(lit) * mix(1, protectAlpha, m)            // phase 1
lit  *= mix(1, protectColor, m)                              // phase 1
out   = (lit, cov) + dither                                  // today
```

The rules the new terms keep:

- A factor in [0, 1] leaves `SolveConeAcross` valid as it is. A term that adds
  light with a reach of its own (edges, particle gain) extends the solve.
- Anything one or two pixels wide pins the scale to 1.0. `GetClampedSpotScale`
  gains reasons beside the clipped lamp, and logs each transition.
- Every time-driven term is a `PhaseAccumulator`, never raw time.
- Every new per-lamp scalar is a flat vertex attribute, never a uniform array.
- Every default is inert, so existing rigs render byte-identically.

| term | extra work per strip fragment (est.) | geometry | below scale 1.0 |
| ---- | ------------------------------------ | -------- | --------------- |
| protection | rounded-box distance, smoothstep, ~15 ops | none; shade quad over the art | in the blit, sharp |
| mount shadow | rounded-box distance, ~15 ops | none | floor the penumbra or pin 1.0 |
| smoke | 2 to 3 texture reads, ~10 ops | none | fine, it is smooth |
| rays | ~12 ops | none | the fade covers it |
| edges | ~10 ops | solve extension | pins 1.0 |
| particles | ~30 ops for two cells | solve gain | pins 1.0 |

Scale 0.5 pays only on a rig of six to eight lamps
([`spotlight-renderer.md`](../guides/spotlight-renderer.md) section 6), and
edges, particles and flare spikes force scale 1.0 on exactly those rigs.
Measure the eight-lamp preset with everything on before promising a frame
budget.

## 6. Plan

Phases run in order where one needs another's answer; 2, 3 and 5 can overlap
once phase 1's fields are settled.

### Phase 0 - learn how the device composites

Needs nothing. Gate: the probe on the device matches one model of 3.5. Size S.

1. The compositor probe in `DebugRenderer`, with its `DebugConfig` field and C
   ABI setter.
2. Run it on the device over a 50% grey video; record the model, and whether
   it blends in linear light.
3. The composite view in the C++ demo, then in `demo-capi`.
4. The mockup's Pass 4 on the measured formula, showing the share of darkened
   pixels.

### Phase 1 - art protection

Needs phase 0. Gate: the protection metric of 4.2 per art type; a zero-size
artwork byte-identical. Size M.

1. Review level 0 (`ClipArea`, `KEEP_OUTSIDE`) across the eight presets and
   eight art types in the composite view. No library code; write down what it
   cannot do.
2. `SpotArtwork`: the mask per lamp at 1.0 and in the blit below it, the shade
   quad, C ABI setters, `AnimatableField` entries.
3. Defaults for `protectColor`, `protectAlpha` and `shade` from the review, for
   the measured compositor.
4. The cap, only if residuals leave a preset washed out.
5. The scope (D2): the spotlight alone, or the flare and the neon as well.

### Phase 2 - terms in the beam

Needs phase 1's fields for the protection interplay. Gate: each term off is
byte-identical; the strip grid check; the clock check; timings. Size M.

1. Rays: `aP2`, the fade rule, a `PhaseAccumulator`.
2. Smoke: the baked noise texture, two accumulators, the density view.
3. Mount shadows, if D4 is yes: raised first, then recessed.
4. Time the 1-, 3- and 8-lamp presets, still and animated, on the M2 Pro, the
   AMD Radeon Pro 5300M and the device - wall-clock on the AMD, whose timer
   queries read ~3.85x.

### Phase 3 - lens flare, studies F and A

Needs the Flare Studies' decisions and D6. Gate: no colour with zero alpha;
`flare` 0 byte-identical. Size M.

1. `SpotLight::flare` and `SpotFlareConfig`, with C ABI and animatable fields.
2. `spotlight-flare.frag` sprites with solved reach, at full resolution, under
   the artwork protection.

### Phase 4 - opt-in looks

Needs phase 2. Gate: the grid check with edges on; a shimmer review on the
device. Size M to L.

1. Laser edges: the term, the solve extension, the scale pin.
2. Particles: the cells, the gain fold, the scale pin, the density ceiling.
3. Glitter pool, only once a floor line is wanted.

### Phase 5 - config contract and presets

Needs today's field names to start, phases 1 to 4 to finish. Gate: a mockup
export loaded in the demo matches the mockup's own screenshot. Size M.

1. Schema 1: canvas size, lamp fields, artwork, smoke, animations with sweep as
   an oscillator.
2. The demo loader and the scaling rule.
3. The eight presets, with their risk notes, in the demo.

## 7. Verification

- **Composite view and probe (4.1)** - the only place the device's picture
  can be judged off the device.
- **Byte-identical defaults.** Each new field at its default renders exactly
  what `main` renders: an offscreen A/B against a checkout of `main`, as the
  merge verifications in [`neon-unification-plan.md`](neon-unification-plan.md)
  were done.
- **Strip bound.** The grid check `buildStrips` describes, rerun for every
  term that adds light (edges, particles) and for any change to
  `SPOT_NEAR_FADE` or `SPOT_STRIP_SEGMENTS`.
- **Coverage alpha.** Render offscreen over a (0, 0, 0, 0) clear and count
  pixels with colour but zero alpha - the probe that caught the 2026-09-17 bug.
- **Clock.** Smoke, rays and particles join the layers `neon-scale-check
  clock` runs from ~10 days.
- **A spotlight mode for `neon-scale-check`.** There is none yet. One that
  renders the presets over the art types offscreen, compares against committed
  images with every new term off, and reports the protection metric under each
  compositor model would serve as the gate for phases 1 to 4.

## 8. Decisions

| # | decision | proposed | blocks |
| - | -------- | -------- | ------ |
| D1 | which blend model the device uses | measure it with the probe (3.5): a fact to find, not a choice | phase 1 defaults, shade |
| D2 | protection scope | the spotlight and its flare first; the neon and droplets later if they need it | phase 1 step 5, phase 3 |
| D3 | kind of protection | residual colour and coverage; the cap only if a preset still washes out | phase 1 |
| D4 | a lamp height in a 2D rig | yes, `SpotLight::height`, 0 meaning no shadows | mount shadows |
| D5 | smoke as visible haze on its own | no: smoke shows only where light passes | phase 2 |
| D6 | flare on the artwork | protected by the artwork, ignored by the clip area | phase 3 |
| D7 | JSON parser for the demo | a vendored single-header parser under `external/` | phase 5 |

## 9. Questions for the mockup's source

1. What the shade term computes: a constant veil over the art, or one that
   follows the light around it.
2. Whether the protection mask reads the art's pixels anywhere. That part
   could not be ported.
3. Which formula Pass 4 uses, and whether it blends in linear light.
4. How smoke combines with the beam: as a factor on the cone, or as light
   added on top.
5. The exact field list the export writes, to fix schema 1 against.
