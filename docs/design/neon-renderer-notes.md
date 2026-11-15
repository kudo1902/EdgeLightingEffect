# NeonRenderer: the commentary behind its code

Moved verbatim out of `lib/src/renderer/neon-renderer.cpp` and
`lib/include/renderer/neon-renderer.h` on 2026-11-12. The same precedent as
[`neon-frag-notes.md`](neon-frag-notes.md): the code keeps the invariant, the
CPU mirror and the warning at each line, and these are the measurements and the
history behind it, grouped by file and named after the symbol each comment sat
on. Nothing here is a current instruction the code does not also carry; where
the two disagree, the code and `CLAUDE.md` win.

## neon-renderer.cpp: construction, the frame, config changes

### SCALED_FORMATS


```text
The reduced buffer's formats in PREFERENCE ORDER, best first - walked
by SharedBuffer::BeginWrite (@c mScaled).

Half float leads because every pixel the blit draws comes from this
buffer, at every scale (1.0 included), and the output dither (R7,
neonDither) can only break up gradients that reach it unrounded. In
RGBA8 the halo's and bloom's slow tails are already plateaus of one
level, tens of px wide in the dark, before the blit reads them -
bilinear filtering joins the plateaus with ramps one texel long, and
a dither on an exact level does nothing, so the contour rings stay.
RGBA8 follows for drivers that will not render to half float: the
ring is still dithered there, the blit's area bands as before.
Half float is texture-filterable in GLES 3.0 core. Twice the memory:
1.6 -> 3.2 MB for an 800 x 500 rect at 1080p at 0.5 (752 x 540
texels) - 4.1 MB at 1080p and 0.5 now that the buffer is the whole
viewport.

SharedBuffer::BeginWrite asks the effect's FormatAvailability and
starts at the first row this driver renders.
```

### Initialize: the coverage table and the vertex formats


```text
NOT the glow coverage table, although its size is also a pair of
compile-time constants: at 1 MB it is most of what this renderer
allocates, and a ring lit uniformly - the default - never reads it.
It is allocated on the first frame that bakes it; see
ensureGlowCoverBuffer.

Vertex FORMAT for the two arrays whose contents are rebuilt at
runtime, declared once here rather than on every rebuild.

A VAO remembers both the attribute format and the buffer it reads
from, and neither ever changes for these two: the layout is a fixed
vec2, and the VBO ids are fixed for the life of the renderer
(VertexArray is move-only and both are members, never reassigned).
So re-declaring it alongside each upload was four redundant GL calls
- bind VAO, bind VBO, enable array, attrib pointer - describing state
that had not moved. glVertexAttribPointer only records the binding;
it does not need the store to exist yet, which is why this can run
before the uploads below.

@ref setupFullscreenQuad keeps its own paired call: that one uploads
exactly once and never returns, so there is nothing to separate.
```

### RenderOffscreen: the pass schedule and its two phases


```text
The frame is a pass schedule and nothing else: derive the transform,
then one call per pass. Each pass owns its own shader and, where it
retargets, its own framebuffer restore. Blend state is owned by the
two phase methods.

TWO PHASES, in this order: everything that renders OFFSCREEN first
(the emission and coverage tables, the gather, pass 1b and the field
bakes) - this method - then everything that lands on the caller's
framebuffer (the fill, the blit, the edge ring) - Render. The effect
runs this phase for every layer before any layer's Render, so the
caller's target is drawn in one unbroken run per frame. On a tile-based GPU every switch away
from it and back stores its tiles out and loads them in again, and
the fill used to go first and force exactly that whenever an
offscreen pass followed it. The fill depends on nothing offscreen
and only has to land under the glow, so moving it costs nothing.
ONE path at every resolution scale, 1.0 included: the glow is
shaded into a reduced buffer - full size at 1.0 - blitted back, and
the edge ring re-shaded at full resolution. (There used to be a
direct path at 1.0 that shaded straight onto the target; no shipped
config uses 1.0, and it went with I57.)
```

### RenderOffscreen: DebugConfig::opaqueOnly


```text
Debug: the fill and nothing else. What lands on
screen is the opaque silhouette by itself - which is how the fill's
square corner at cornerRadius 0 gets compared against the emission's
round one. DebugRenderer honours the same flag, so the overlays do
not reappear over a fill-only frame.

The one field this renderer reads out of DebugConfig. It lives there
because it is a debug control, and it is read HERE because it is a
mode of this renderer's pass schedule rather than something another
layer can draw - no other renderer can decline to run these passes.
Ahead of everything else, so fill-only mode compiles no neon program,
uploads no UBO and bakes no table it would never sample.
```

### RenderOffscreen: reusing passes 1a and 1b


```text
Passes 1a and 1b not at all when what their buffers already hold is
what they would draw - see mOffscreen.reusable. Then the frame never
leaves the caller's framebuffer, so there is no target to capture
either. Nor when either buffer was allocated since it was drawn -
by refreshTargets above, moving it back up its format list, above
all: its texels are undefined - or lost since: a move whose retry
and fallback both failed releases it, which moves the generation
too, and the frame then runs pass 1b, whose walk finds no row and
drops the glow. Before a loss moved it, every still frame after
such a move reused the released buffer and blitted texture 0 -
opaque black over the host. Both, each for itself: at a scale at
or below GATHER_MIN_SCALE the gather's buffer can be the larger
allocation (96 x 64 x 2 texels against 80 x 45 at 0.0625 and 720p),
so one can move without the other.
```

### RenderOffscreen: pass 1b sets its own blend state


```text
Unblended, set HERE: pass 1a, which used to set it
just above, is skipped on a frame its inputs held,
and pass 1b then drew with the previous frame's
GL_SRC_ALPHA blend into its cleared buffer.
```

### OnConfigChanged: what clears the offscreen reuse


```text
The offscreen buffers were drawn from the config this replaces. Wide,
unlike the gates below - pass 1b reads most of the neon's config, and
re-drawing them costs one frame's offscreen phase - but no wider than
what this renderer reads at all: its own sub-config and the geometry
(Render reads debug.opaqueOnly too, and returns before any of this).
This call comes whenever the COMPOSITED config changed, another
layer's fields included, and an animation of one - the lens flare's
sun riding the perimeter, an AnimatableField - changes it every
frame. Cleared on that, the field never settled and the neon drew
pass 1 directly every frame: 4.9x with the hue rotating and 15x
still at 1.0, ~4x still at 0.5 (AMD Radeon Pro 5300M, 1920 x 1080),
for an image the field draws byte-identically.
```

### OnConfigChanged: the rebuild gates


```text
Snapshot dirtiness before we overwrite mCurrentConfig. Each rebuild
is gated on the exact set of fields it reads (see the corresponding
methods below) - dragging a slider like `bloomStrength` used to
re-upload the whole LUT and loop-samples UBO every frame; now only
the geometry quad refreshes.

resolutionScale and numSamples join geometry on the sample walk:
the samples are stored pre-scaled, and only the first numSamples of
them are filled. Both therefore reach the quad as well, through
samplesDirty.

NeonBypass::REBUILD sets every one of these gates, below too.
```

### OnConfigChanged: lineWidth in the quad's gate


```text
lineWidth feeds setupGeometry's filament-reach floor, which is
what sizes the quad whenever glowRadius is small. Miss it and a
widened filament keeps the old, tighter margin and gets clipped
on the OUTSIDE only (the quad bounds the exterior; the interior
is always covered) - the "outer glow dropped at glowRadius 0"
report. Only bites at small glowRadius, because above
lineWidth / 10.4 the glow term wins the max() anyway.
```

### OnConfigChanged: glowSide and insideCutoff in the quad's gate


```text
glowSide and insideCutoff BOUND THE QUAD NOW. They did not
when this gate was written - the quad was rect + glow reach
whatever the glow did with its own interior, so both of these
moved only the shader's discards. @ref setupGeometry now caps
the margin under GlowSide::INSIDE and cuts a hole for the
other two, so a change to either has to rebuild it.

Missing them does not under-draw, it draws the PREVIOUS
config's bound: switching BOTH + insideCutoff 8 to INSIDE with
no cutoff kept the ring built for the first and rendered the
second through it, holing out 48131 px of lit interior and
leaving a 12 px frame of glow round the inside of the edge.
The same shape of mistake as adding a field to a Config struct
and not to its operator== - see AGENTS.md.
```

### OnConfigChanged: the merged segment view's gate


```text
The merged transient+preserved view is a pure function of the two
segment pools, so it gets a gate like every other rebuild here. It
used to run on EVERY config change, which with an animation attached
is nearly every frame - and it is not free: SegmentBoost owns a
colorStops vector, so clear() + push_back frees and reallocates one
heap block per stopped segment each time, to reproduce a list that
in a segment-less animation never differs.
```

### OnConfigChanged: the overflow warnings


```text
Overflow warnings, before mCurrentConfig is overwritten below: the
previous counts are still in it, which is what lets these fire once
per overflow without a latch of their own.

Both count what the HOST ASKED FOR, not what survives. That matters
for segments: SegmentUtils::FillEffectiveSegments stops merging at
MAX_SEGMENT_BOOSTS_CAP, so mEffectiveSegments is already clamped and
measuring it could never exceed the cap - which is exactly why the
old warning here never fired. The two pools are summed because they
share the slots.
```

### OnConfigChanged: the emission and coverage tables' gates


```text
The emission table needs nothing from here. It is keyed on what its
pass binds - two uniforms by value, three LUTs and two light blocks
by upload count (isEmissionTableStale) - so a change that moves one
of those re-bakes it through that count, and a change that moves
none (intensity, bloom, the glow, the rect) re-bakes nothing.

The glow coverage table is gated here instead, on the narrow,
visible set of inputs above. Gated wide, it re-ran every frame under
any animation at all - intensity, colour, or a field of another
layer entirely - for a table none of those move. And per PIECE: a
light that moved re-bakes only the pieces its old and new supports
reach, so a segment travelling along one straight re-bakes that
straight's band, not the whole table. Accumulated, because OnConfigChanged can
run more than once between frames.

And per light TYPE: the arcs' channels and the segments' are dirtied
separately, so arcs animating over still segments never re-integrate
the segments' bells (see GlowCoverTable::dirtyArcPieces).
```

### OnConfigChanged: the light blocks have no gate


```text
The light blocks have no gate here: packLightBlocks packs them on
every frame and UniformBuffer::SetData skips an upload whose bytes
match the last one. They used to be gated on segmentsDirty ||
arcsDirty, which missed the perimeter - PackArcFlags turns the
feathers' px into perimeter fractions - so a resized rect kept the
old abutment flags, up to 3 levels off until the arcs next changed
(V28). A gate on a hand-written input list is what can go stale; the
byte compare cannot.
```

### OnConfigChanged: releasing the reduced buffer


```text
Give the reduced buffer back the moment the layer is switched off.
With the glow coverage table below it is one of the only allocations in this
renderer that are not a handful of KB: at 1920x1080 and scale 0.5
the reduced buffer is 2.1 MB of colour attachment, and nothing else
here freed it, so a host that enabled the neon at a reduced scale
during setup and then turned it off held that for the life of the
effect. Everything else the renderer owns - the three atlases, the
emission table, the UBOs, the quads - is fixed-size and small enough
that reclaiming it would cost more in reallocation than it saves.

Cheap to get wrong in only one direction, and this is the safe one:
@ref renderShadePass re-Resizes before it binds, so a release of a
buffer that turns out to be wanted again costs one allocation on the
next drawn frame and nothing else. Resize's own early-out then keeps
it allocated for as long as the size and format hold.

Here rather than in Render because Render must not be the thing that
deletes a framebuffer - see Framebuffer::Release on why the deletion
wants to be outside a pass. This runs between frames, from SetConfig
or the effect's active-config refresh.

BEFORE the shader-validity return below, which guards REBUILDS: a
release is not one, and this path is also reachable pre-Initialize
(AddRenderer calls OnConfigChanged), where Release no-ops on the
buffer it finds unallocated.
```

### OnConfigChanged: the mInitialized gate


```text
Rebuilds need the GL objects Initialize creates. The neon.frag
programs are no gate any more - they are built on first draw, so a
valid program no longer means "initialised".
```

## neon-renderer.h: the renderer's private types

### LazyBake


```text
When a field is baked and when it is read - the schedule the
shading's field (@c mGlowField.schedule) and the edge ring's (@c mRingField.schedule)
share. A field outlives the frame it was baked on; a config change
(@ref OnConfigChanged calls @ref Invalidate), the viewport and a
gradient ring upload - a cross-fade frame - invalidate it, and so
does an input buffer that moved rows since its bake
(@ref FieldInputFormats, checked by @ref NeonRenderer::RenderOffscreen
right before each composite).

The bake is LAZY: only a config that has held for a frame is baked
(@c settled), so an animation that changes the config every frame
keeps shading directly and never pays a bake it cannot reuse - a
bake AND a composite on every frame measured 5-7% slower than
shading directly.

Except on the FIRST frame a renderer draws a config the field can
serve, which builds the field's programs and bakes it at once,
while still shading directly. Building alone is not enough: this
driver finishes a program's compile on its first DRAW, in the
target and blend state it draws with, so a bake program only linked
there left a ~125 ms stall on the bake frame after it (~155 ms all
told, AMD Radeon Pro 5300M). And that frame still shades directly
so that the shading program's first draw lands there too -
compositing instead deferred THAT stall to the first config change.
The price is one bake an animated config may not reuse, once per
renderer.

The neon's INTENSITY is the one config change that does not
invalidate a field (I56): neon.frag's output is near enough
I * G with G fixed while only the intensity moves, so a field baked
at intensity I0 serves intensity I as Fa * I / I0 (@c Decision::gain).
Near enough, not exactly: the bloom's reach and the quad's fade and
hole also follow the intensity, and the field holds them as they
were at I0 - up to 2 levels on the default look across
IntensityPulse's 0.4-1.0, 6 on a 1.5 bloom (docs/neon-reduced-scale-
plan.md, D1). So only while the intensity is MOVING, and only below
I0, whose quad covers every dimmer one's; a brighter frame bakes
again at its own intensity, so a pulse settles on its peak after
one cycle; and the frame the intensity stops on bakes again too,
exactly - a held config never shows a scaled field.

@par One frame
@ref Decide, in this order - the first row that holds decides:
| State                                      | Decision            | Why |
| ------------------------------------------ | ------------------- | --- |
| viewport or gradient upload count moved    | (invalidates, then reads on) | what the buffer holds was drawn for another frame |
| @c current, intensity unchanged            | use                 | the field still describes this frame |
| @c current, intensity moving, 0 <= I < I0  | use, gain I / I0    | serve the dimmer frame scaled (I56) |
| @c current, any other intensity            | bake + use          | brighter, or stopped: bake again, exactly |
| @c settled                                 | bake + use          | the config held for a frame: a bake now is reused |
| @p firstFrame                              | bake                | warm the programs' first draws; still shade directly |
| otherwise                                  | nothing; sets @c settled | the config just changed: shade directly, bake next frame if it holds |

@par Who calls what
  - @ref NeonRenderer::Render calls @ref Decide once a frame, and
    only for a config the field can serve and a field not
    @c unavailable; then, after a bake, @ref OnBaked.
  - @ref NeonRenderer::OnConfigChanged calls @ref Invalidate on any
    change to the neon or the geometry but the intensity alone, and
    clears @c current itself when it releases the buffer for a
    config that cannot use it.
  - @ref NeonRenderer::RenderOffscreen calls @ref DropIfInputsMoved
    once the gather has run, before either composite: a field whose
    inputs moved rows since its bake shades directly that frame and
    is baked again on the next.
  - A failed program build sets @c unavailable
    (@ref NeonRenderer::ensureFieldPrograms,
    @ref NeonRenderer::ensureRingFieldPrograms); nothing else does.
  - A driver that cannot render the field's half-float format never
    gets this far, nor a field whose allocation already failed at
    this size or larger: @ref NeonRenderer::RenderOffscreen asks the
    effect's FormatAvailability (MayAllocate, with the field's
    size) before it builds the programs or calls @ref Decide, every
    frame, since the format follows the segments and the size the
    rect.
```

### GlowCoverTable::dirtyArcPieces and dirtySegmentPieces


```text
Which pieces of @c mGlowCover.buffer have to be re-baked: bit 2b is
band b's straight, bit 2b + 1 its corner, all eight when the shape
moves. NOT set on every config change, as the emission table's
flag used to be: the bake
reads the two light blocks (the arcs and the effective segments),
the rect's width, height, corner radius and winding, and the glow
radius - every one of them in @ref renderGlowCoverPass or the
blocks it binds - so @ref OnConfigChanged gates it on exactly
those, as the light blocks once were gated. The bake is the costly
pass (0.14-0.27 ms on an AMD Radeon Pro 5300M), and "any change"
re-ran it every frame under an intensity or colour animation, or an
animation of another LAYER's fields, none of which it reads.

Per PIECE because each piece's texels integrate the lights over that
piece alone: a changed arc or segment dirties only the pieces its
old and new supports reach (GetGlowCoverDirtyArcPieces,
GetGlowCoverDirtySegmentPieces), so a segment
travelling along one straight re-bakes that band rather than the
table - 1.18-1.91x on such frames on an Apple M2 Pro
(neon-perf-plan.md, item 5). The shape, the winding and the
glow radius dirty all eight, as do the brightest arc's intensity
and any light covering the whole ring.

Accumulated, never assigned, because OnConfigChanged can run more than once between frames, and
cleared only by the bake - so a change made while the bake is
skipped (a ring lit uniformly) is still pending when it next runs.
Also set whole by @ref ensureGlowCoverBuffer on a fresh allocation.
Never depends on time. Starts with every piece - the buffer holds
undefined texels until the first bake.

And per LIGHT TYPE: a piece's arc channels (.r / .g) depend on the
arcs and the geometry alone, its segment channels (.b / .a) on the
segments alone. So the two kinds keep separate masks, and the bake
integrates and writes only the kind a piece needs - arcs animating
over still segments no longer re-integrate every segment's bell
in each dirty piece (1.35 -> 0.36 ms of bake with 4 + 4 on an AMD
Radeon Pro 5300M). The geometry dirties both.
```

### OffscreenState::reusable


```text
Whether the offscreen phase's buffers - @c mGather.buffer and
@c mScaled, at every scale - still hold exactly what this
frame's passes 1a and 1b would draw into them, so @ref Render can
skip both and never leave the caller's framebuffer.

The gather has a skip of its own since I55 (@ref GatherCache), which
a still frame meets too; pass 1b has no other. So what this flag
alone saves is pass 1b's shading - the glow's field serves only hue
and intensity frames, and is not consulted on a frame that reuses
the buffers - and the trip off the caller's framebuffer and back,
with the target capture it takes.

Both passes are pure functions of the config, the viewport, the two
tables they read (emission, glow coverage) and - only through the
hue rotation - the time; both buffers persist between frames and
nothing else writes them. So: cleared by @ref OnConfigChanged on ANY
change to the neon's config or the geometry (pass 1b reads too much
of the former for a narrower gate to be worth its risk), but not on
another layer's, which nothing here reads and an animation of which
would otherwise clear it every frame; set by @ref Render on every
frame whose offscreen phase drew or reused both, unless the reduced
buffer holds the field's composite rather than the shading (I56: the
frame the intensity stops on redraws pass 1b exactly); and honoured
only on a frame whose emission table is current
(@ref isEmissionTableStale, which carries the hue phase and the ring's
cross-fade), whose glow coverage table needs no bake, and whose
viewport is @c viewport. Byte-identical. With the gather's own
skip in place, a still frame without this flag is 4.2x slower
at scale 0.5 (3.1-5.5x across neon-scale-check's twelve scenes),
10.3x at 1.0 and 2.1x at 0.125; frames whose config changes are
unaffected, since every change clears it (AMD Radeon Pro 5300M,
1920 x 1080, 2026-10-08). First built as neon-perf-plan.md
item 4.
```

## neon-renderer.cpp: the glow's bounds and meshes

### GetGlowInnerReach


```text
How deep inside the rect edge, in FULL-RES px, the glow can still
write a non-zero pixel, for a glow whose emission is at most
@p emission (@ref GetGlowEmissionBound): the interior counterpart
of the quad's margin. CUTOFF_DISABLED_SIZE when that is the middle.

WHY THE INTERIOR NEEDS THIS. With no inside cutoff, every fragment
inside the rect used to run the full shader - the gather loop
included - whatever the glow did there. On a screen-sized rect most
of the frame is interior, and nearly all of it is further from the
edge than the glow reaches: measured at 1920 x 1080, a 1840 x 1000
rect at glowRadius 2 shaded the whole interior to write zeros, and
an inside cutoff placed past where anything was lit - changing no
pixel - took the frame from 6.0 ms to 2.8 ms (AMD Radeon Pro 5300M).
neon-perf-review.md section 6 had set this aside because the
demo's rect is smaller than twice the glow reach.

WHY NOT SIMPLY `reach`. Past neon.frag's `reach` the filament and
every straight's bloom are exact zeros (the pedestals; bloomSegment
falls monotonically with distance from its line). Two terms are not:

  - the halo, which has no pedestal. Its 1/a^2 tail is invisible at
    `reach` at intensity 1, but not at intensity 3 with bloom 0,
    where `reach` stops growing with the brightness: measured lit to
    1.9x `reach` inside the rect, up to 2/255.
  - the corner arcs' bloom. Their ONE shared pedestal is exact for a
    fragment outside an arc and too small on its concave side, where
    arcTangentSegment develops the arc at the fragment's own radius:
    measured lit to 1.3x `reach` inside a circle, up to 6/255.

The outside does not see either - the quad-edge fade takes every
term to 0 at the margin. The inside has no such fade, and adding one
would change pixels, so the hole is cut where the glow is ALREADY
below half a level instead: the smallest depth D at or past `reach`
at which an upper bound on both terms falls under
@ref GetGlowInvisibleLevel. Every bound holds for any fragment at
least D inside the edge, which is every fragment the hole can hold:

  - each straight is no brighter than its whole line,
    2 kh^2 / (a^2 + kh^2), and every line is at least D away; the
    two facing each other sum highest where one is as near as it can
    be, so the pair is at most the near one at D and the far one at
    the rect's width (height) less D;
  - each corner arc is weighted r / lam over a span lam * HALF_PI
    (arcTangentSegment), so lam cancels and its halo is at most
    pi r kh^2 / (2 c^3), its bloom pi r bw / (2 c^2), c measured from
    the arc - at least D away - and the bloom less the shader's
    shared pedestal.

The bound is loose by design - all four arcs at D, two lines at D -
and lands ~20% past `reach` at the defaults on a 1840 x 1000 rect
(368 px against 312, with the last lit pixel at 311). Never tighter
than `reach`, which keeps the exact zeros above exact. Lengths
enter as ratios, so one full-res solve serves every resolution
scale; @p scale only places `reach` (the filament's Nyquist floor
widens it below 1.0).

A function of the geometry, the glow's shape and @p emission, all of
which rebuild the quad - the first two through geometryDirty, the
third through mGlowEmission (see @ref NeonRenderer::OnConfigChanged).
```

## neon-renderer.cpp: the passes

### renderFillPass: a clear in place of a coverage-1 fill


```text
A shaped mode reaching the shader below draws the band ring built by
@ref setupFillGeometry, which is in full-res rect-local pixels and so
needs a full-res transform to place it. A coverage-1 mode that could
not take the clear draws the identity-MVP fullscreen quad instead,
which is what `ring` selects between - no ring was built for it. The
SDF is unaffected either way: it reads gl_FragCoord, not the vertex
position, so the transform decides only WHICH fragments are shaded,
never what they shade to.
A fill whose coverage is 1 at every pixel is a flat overwrite, so it
does not need a shader at all. Premultiplied-over with coverage 1
leaves exactly uOpaqueColor.rgb and alpha 1, which is what glClear
writes - and a clear costs no fragments, no blending and no vertex
work. Measured here at 3840x2160 it takes ~0.4 ms off the pass, the
one case @ref setupFillGeometry's ring cannot help with because the
coverage really is the whole screen.

The test is @ref FillsWholeViewport, not `mode == ALL`: BOTH with
both cutoffs disabled produces identical output, and that is the
DEFAULT cutoff state, so it was the common way into this cost rather
than a corner case. @ref setupFillGeometry asks the same function, so
a coverage-1 mode always arrives with an empty mFillMesh.

Measured on that BOTH case with debug.opaqueOnly isolating the pass,
3600x2126, min of 200 frames around glFinish: 1.18 ms before the
routing, 0.59 ms after, stable to +-0.07 ms across runs. The frame
is byte-identical either way - and byte-identical to ALL, which is
the whole argument for the routing.

WHAT THIS DOES NOT BUY, because an earlier version of this comment
claimed it did: on a tile-based GPU a clear can mean "the previous
contents are dead, never load the tile from memory". That is a
property of an UNSCISSORED, full-attachment clear at the START of a
render pass, and this clear is neither. It runs mid-pass - the host
has already drawn its backdrop into this target and the neon is
still to come - so the load has happened either way; and it is
scissored by construction (see the box derivation below), which is
enough on its own to keep most tilers off the fast path. Expect the
saving here to be exactly what the numbers above measure - no
fragments, no blending, no vertex work - and nothing structural
beyond it. Still a clear win, just a smaller one than advertised.
Both measurements are desktop macOS, an immediate-mode renderer;
the tiler behaviour is reasoned, not measured on device.

@ref ClearClipsLikeDraw is the second half of the condition, and it
is a correctness guard rather than an optimisation: the clear is an
exact substitute only while nothing that clips a draw but not a
clear is switched on. When one is, this falls through to the shader
path below and pays the full-viewport shade, which is the right way
round - a host enabled those tests in order to clip something, and
the saving is not worth silently ignoring it. Note the short-circuit
ordering: the two glIsEnabled queries run only for the modes that
could actually use a clear, not on every frame of every mode.
```
