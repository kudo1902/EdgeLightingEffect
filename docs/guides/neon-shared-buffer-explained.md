# How the shared buffer works with the rest of the neon

This page is for understanding, not for using.
[`neon-shared-buffer.md`](neon-shared-buffer.md) tells you how to read the
neon's light and lists every rule. This page explains how the **shared
buffer** (`NeonRenderer.Composite`) fits among the other parts of
`NeonRenderer`:
- what it takes from each part;
- what, if anything, it changes in each;
- what happens frame by frame.

No graphics background is assumed beyond "the neon draws a soft glow and a
sharp line round a rectangle". When a word is new, the glossary at the end
has it.

---

## 1. The neon in one minute

Every frame, the neon draws its light in two phases:

- **Phase 1 (Prepare)** draws into the neon's own textures - never onto the
  screen.
- **Phase 2 (Render)** then draws the screen from them.

These are the parts involved:

| part | what it does, in one line |
| ---- | ------------------------- |
| **Tables** (passes 0, 0b) | Work out, once, the colour of each point on the outline and how lit each side and corner is. Redone only when the config changes them. |
| **Gather** (pass 1a) | For each place near the rect, the colour arriving there from the whole outline - the expensive sum, done on a coarse grid. |
| **Glow** (pass 1b, or 1c) | The soft halo and bloom, shaded at `resolutionScale` (half size by default) from the gather. This texture is `NeonRenderer.Glow`. |
| **Edge ring** (pass 2c) | A thin band round the rect's edge holding the sharp line, shaded at full resolution so it stays sharp. |
| **Ring atlas** (pass 1t) | The same edge ring drawn into a texture of its own (`NeonRenderer.Ring`), packed into four strips. The screen then COPIES the ring from it instead of shading it. |
| **Fields** (passes 1f / 1c, 1r) | Shortcuts for frames where only the hue or the intensity moves: the parts of the shading that do not move are baked once and reused. |
| **Blit** (pass 2b) | Scales the glow up onto the screen everywhere outside the edge ring, and cuts it where the config says (one-sided glow, cutoffs). |
| **Fill** (pass 2a) | The optional opaque fill under the glow. |
| **Geometry** | Boxes worked out on the CPU when the config changes: where the ring is, where the light can reach, where the fill is. |
| **Reuse flags** | When nothing moved since the last frame, phase 1 is skipped: its textures already hold the answer. |
| **Registry** | The effect-wide list of named textures. A renderer publishes a texture there, and readers find it by name. |

So **the screen is the fill, plus the glow scaled up, plus the ring.** The
glow and the ring atlas are the two textures the screen is drawn from - the
neon's two **parts**.

---

## 2. Where the shared buffer sits

The shared buffer is **one more texture, made at the very end of phase 1 by
pass 1s**. It holds the neon's whole layer - the fill, then the light - as a
single flat image, for a reader that wants exactly that: a host handing the
neon to a compositor, say.

![The shared buffer among the neon's other components: on the left the neon's own pipeline (tables, gather, glow, ring field, ring atlas, geometry); in the middle the shared buffer, built from the glow, the ring atlas and the geometry's boxes; on the right what steers it (who reads it, compositeScale, reuse, releases, its programs); at the bottom the screen, drawn from the glow and the ring atlas - never from the shared buffer.](../images/neon-shared-buffer/components.svg)

Read the picture this way:

- **Three arrows go in.** The shared buffer is made from:
  - the Glow;
  - the Ring atlas;
  - the boxes the geometry worked out - where the light can be, where the
    fill is.

  It shades nothing itself. For each of its texels it asks, through
  `neonLight`, "what light did the screen get here?", and averages the
  answers.
- **No arrow goes from it to the screen.** The screen never uses it, and
  nothing inside the neon reads it. Only its readers do.
- **One red arrow goes back.** That is the only thing it changes in the rest
  of the neon: while someone wants it, the ring atlas must exist on every
  frame, so the screen's ring is copied from the atlas on moving frames too
  (section 3.3).
- **The right-hand panel steers it.** Whether anyone reads it, its scale,
  whether it can be reused, when it is freed.

---

## 3. Part by part

For each part of the neon: what the shared buffer takes from it, and what it
does to it.

### 3.1 The tables and the gather (passes 0, 0b, 1a)

- **Takes:** nothing directly. The glow and the ring are made from them, and
  the shared buffer reads the glow and the ring.
- **Does:** nothing. No table is baked again and no gather runs again because
  the shared buffer is read. (That is a rule of all sharing in this library:
  a read never makes the producer redo its work.)

### 3.2 The glow (pass 1b, or 1c)

- **Takes:** the glow texture, read at every screen pixel through
  `neonLight`. `neonLight` reads it exactly as the blit does - the same
  bilinear read, then the same edge mask (the one-sided cut and the
  cutoffs), with the very numbers the blit gets (`GetEdgeMask`). So the
  shared buffer's glow is the screen's glow.
- **Does:** nothing. If this frame's glow is a shortcut - the glow field's
  composite under a rotating hue or a moving intensity - the shared buffer
  gets that same composite, because it reads the same texture.

### 3.3 The edge ring and its atlas (passes 1r, 1t, 2c)

This is the one real interaction.

**Without the shared buffer** the ring takes one of two routes:

- **On a frame where something moved**, the ring is shaded straight onto the
  screen. No texture holds it.
- **On a frame where nothing moved**, the ring is shaded once into the ring
  atlas, and every still frame after that just copies it. That is cheaper
  than shading it again.

**With the shared buffer wanted**, the ring has to be in the atlas on EVERY
frame, because the shared buffer reads it from there. So on moving frames
too:

1. pass 1t draws the ring into the atlas - shaded, or composited from the
   ring's field when that shortcut serves;
2. pass 2c copies it from the atlas onto the screen;
3. pass 1s reads the same atlas.

The ring is still shaded **once** per frame, never twice, and screen and
reader see the same ring. Two things change:

- **The screen's ring pixels can move by one level** (of 255). The atlas
  stores the ring in half-float, and a pixel the copy finds exactly black is
  left untouched, where shading would have added its dither noise.
- **One more render pass a moving frame**, for the atlas: about 0.03 ms on an
  Apple M2 Pro.

One more rule ties them. On a moving frame the ring may come from its field
scaled to a moving intensity, which is close but not exact. On the frame the
intensity stops, an atlas made that way is drawn again, exactly, and so is
the shared buffer (`RingAtlas::exact`). Neither keeps an approximation.

### 3.4 The blit (pass 2b)

- **Takes:** nothing from the pass itself. It repeats the blit's rule for the
  glow - read, then mask - inside `neonLight`, fed by the same upload. That
  is why a cut or a cutoff looks the same in the shared buffer as on the
  screen.
- **Does:** nothing.

### 3.5 The fill (pass 2a)

- **Takes:** the fill's code and shape, not the screen's fill pixels. Pass 1s
  draws the fill again into the shared buffer, at the shared scale:
  - through the same function (`drawFill`);
  - with a program object of its own (`mCompositeFillShader`). The library
    never lets one program draw an offscreen texture and the screen in the
    same frame, because that measured 93-624x slower on one driver.
- **Does:** nothing to the screen's fill. The fill's extent (`mFillOuter`)
  does decide how big the shared buffer is: a fill that covers the whole
  screen makes the shared buffer cover the whole viewport.

### 3.6 The geometry (worked out in `OnConfigChanged`)

- **Takes:** two boxes.
  - The light's whole box (`mBlitBounds`) is where pass 1s runs, grown by one
    texel (`mComposite.lightMesh`).
  - That box and the fill's band set the shared buffer's **region**: the
    part of the screen it covers, rounded up to 16 texels so a moving rect
    does not reallocate it every frame.
- **Does:** nothing. A change to `compositeScale` alone rebuilds only the
  shared buffer's own area (`setupCompositeGeometry`). It does not make the
  neon redo its own passes: the scale is the one config field the neon's
  "did anything change?" test ignores.

### 3.7 The reuse flags

When nothing moved since the last frame (`reuseOffscreen`), the neon skips
phase 1: its textures already hold the answer. The shared buffer follows:

- **reused** when nothing moved, the ring atlas was not drawn again, and the
  scale, viewport and region are the same;
- **drawn again** otherwise.

So a still scene with a reader costs nothing extra after its first frame: no
pass runs in phase 1, and the shared buffer's version number stays the same,
which tells a reader nothing changed.

### 3.8 The registry: how it is asked for, and how it is handed over

- **Asked for.** At the top of each frame, everyone who will read a texture
  says so:
  - a layer in its `CollectReads`;
  - a host with `EdgeLightingEffect::AddHostRead`;
  - the debug layer's buffer view, when you select it there.

  The neon asks `IsBufferWanted` and draws the shared buffer only if the
  answer is yes. With no reader, pass 1s never runs, whatever
  `compositeScale` says.
- **Handed over.** When pass 1s has drawn it, or found it reusable, the neon
  marks its record current (`publishBuffers`). The record carries where it
  lies on the screen (`origin`, `size`) and a `version` that moves only when
  the picture changed. A reader finds it by name (`NeonRenderer.Composite`),
  checks it is current, and samples it.

### 3.9 Programs

Pass 1s has two programs of its own, both built the first frame the shared
buffer is wanted (`ensureCompositePrograms`):
- the fill (`mCompositeFillShader`), a link of the fill's existing shader;
- the assemble pass (`mAssembleShader`, `neon-assemble.frag`).

If one fails to build, it is logged once and the shared buffer is simply
never made. The screen is unaffected.

### 3.10 Freeing it

The shared buffer is freed:
- when `compositeScale` goes back to 0;
- when the neon is switched off;
- after 5 seconds with nobody reading it.

The frees happen in `OnConfigChanged` and `Update`, never in the middle of
drawing a frame.

---

## 4. Two examples, frame by frame

### 4.1 The default look (hue rotating), a host reading the shared buffer at 0.5

The hue moves every frame, so nothing is ever "still".

| frame | what changed | phase 1 runs | pass 2c | the shared buffer |
| ----- | ------------ | ------------ | ------- | ----------------- |
| 1 | everything (first frame) | builds its programs; tables, gather, glow, ring atlas, **1s** | copy of the atlas | drawn; current |
| 2, 3, ... | the hue (time) | the emission table, the gather, the glow from its field (1c), the ring atlas from the ring's field, **1s** | copy | drawn every frame |
| the host stops reading | - | as above, without 1t and **without 1s** | the ring shaded (its field) on the screen | not current; freed 5 s later |

Cost on an Apple M2 Pro at 1080p: a hue frame goes from 0.143 ms with no
reader to 0.209 ms with this one. Almost all of the difference is two
render passes, the atlas's and the shared buffer's. Drawing into the shared
buffer adds 0.001 ms.

### 4.2 A still look (hue not rotating), a host reading at 0.5, then an intensity animation

| frame | what changed | phase 1 runs | pass 2c | the shared buffer |
| ----- | ------------ | ------------ | ------- | ----------------- |
| 1 | the config | gather, glow, ring atlas (shaded), **1s** | copy | drawn; current |
| 2 ... | nothing | **nothing** | copy (atlas reused) | reused; version unchanged |
| animation | the intensity, each frame | the glow (possibly its field, scaled), the ring atlas (possibly from its field, scaled), **1s** | copy | drawn every frame |
| the frame it stops | nothing | anything made from a scaled field is drawn again, exactly - the ring atlas and **1s** among them | copy | drawn, exact |
| after | nothing | nothing | copy | reused |

---

## 5. Questions people ask

**Does setting `compositeScale` change what I see on the screen?**
Not by itself: with nobody reading the shared buffer, nothing happens. While
someone reads it, the screen's ring is a copy of the ring atlas on moving
frames too, within one level of what it was.

**Why not copy the screen into the shared buffer?**
The screen is the caller's framebuffer. It already holds every other layer
and the host's own content under the neon, and it is dithered. The shared
buffer is the neon alone, over transparent black, without noise.

**Why does it need the ring atlas?**
Without the atlas, the ring only ever exists on the screen. The only way to
get it into another texture would be to shade it a second time. That is
what the shared buffer used to do, before the atlas, and it was up to 82
levels wrong at half scale, because it shaded a one-pixel line at half
resolution. Now it reads the ring the screen got.

**Why is it not free, then?**
It is cheap to fill but not free to have:
- the ring atlas's render pass;
- the shared buffer's own render pass, with its clear and store.

On the M2 that is 0.04-0.19 ms a moving frame. A reader that can run the
neon's shader code should read the two parts instead (0.016-0.033 ms); see
[`neon-shared-buffer.md`](neon-shared-buffer.md) section 2.

**Can two readers want different scales?**
No: there is one shared buffer at one scale (`compositeScale`). Both readers
get it.

**Can a layer read it while preparing its own frame?**
Yes. A layer that declares the read in its `Prepare` phase is run after the
neon's `Prepare` (the effect orders them), so the shared buffer is ready.

**Why is it half-float and not dithered?**
A reader usually scales or filters it. Noise added here would be blurred into
blotches, and 8 bits would round the glow's faint tails into steps. A reader
writing it to an 8-bit target dithers there itself.

**What is in it past the edge of the screen?**
Its size is rounded up, so a few texels can lie past the viewport. They hold
the light at the edge, so a filtered read of the last row or column does not
darken.

**What if the GPU cannot render half-float?**
Then there is no ring atlas: the ring is always shaded on the screen. The
shared buffer is not made while any of the ring is on the screen, since it
would have nothing to read the ring from.

---

## 6. Glossary

- **Phase 1 / phase 2** - Prepare (into the neon's own textures) and Render
  (onto the screen). Every layer's phase 1 runs before any layer's phase 2.
- **Reduced buffer** - the glow's texture, at `resolutionScale` of full size.
  Published as `NeonRenderer.Glow`.
- **Edge ring** - the thin band round the rect's edge, shaded at full
  resolution because the line is too sharp for the reduced buffer.
- **Ring atlas** - the edge ring drawn into a texture, its four sides stored
  as four strips (the left and right turned on their side). Published as
  `NeonRenderer.Ring`.
- **Field** - a baked shortcut: the parts of the shading that do not change
  while only the hue or the intensity moves.
- **Scaled field** - a field baked at one intensity and multiplied up or down
  to another; close but not exact, so never kept once the intensity stops.
- **`reuseOffscreen`** - "nothing moved since the last frame": phase 1's
  textures still hold the answer.
- **Record** - an entry in the registry naming a texture, where it lies, and
  whether it holds this frame's picture (**current**).
- **PUBLIC / DEBUG** - a PUBLIC record has a written contract readers may rely
  on (the glow, the ring atlas, the shared buffer); a DEBUG one is an internal
  texture shown only by the debug view.
- **Region** - the part of the screen the shared buffer covers.
- **`neonLight`** - the shader function that answers "what light did the
  screen get at this pixel?" from the two parts. Pass 1s uses it, and so can a
  host.

---

Next: [`neon-shared-buffer.md`](neon-shared-buffer.md) for reading it and the
full rules; [`neon-shared-layer-perf-review.md`](../history/neon-shared-layer-perf-review.md)
for what it costs; [`neon-onboarding-guide.md`](neon-onboarding-guide.md) Part
6 for every pass of the neon in detail.
