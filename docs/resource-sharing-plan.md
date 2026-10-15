# Sharing resources between renderers - plan

Proposal, 2026-10-15, on `design_system_sharing_resources_between_renderers`,
written at `3e01ccf` plus the I64 fix, which has since landed as `a213d55`.
The owner settled every decision
the same day, each one as recommended (section 5). The steps start with the
shared shader cache.

**Built so far (2026-10-15, uncommitted): S0 and S1.** Section 7 has what each
measured.

**The owner's goals (2026-10-15).** Three uses for sharing:

- **Cross-layer effects** - one layer reads another's output.
- **Memory pooling** - layers borrow temporary render targets from one pool.
- **One copy of common objects** - quads, shaders, LUTs.

And one rule: **a resource has one owner, which alone writes it; everyone
else only reads it.** Host interop - textures in from the host, layer outputs
out to it - was not asked for and is not planned.

Code references name a file and a symbol, never a line number, as in
[`review-findings.md`](review-findings.md).

---

## 0. Summary

Today's `BufferRegistry` lists, each frame, the offscreen buffers the layers
drew. Only the debug layer's buffer view reads them. The plan grows that
registry into a `ResourceContext` that the effect owns and hands to every
renderer. The context holds five things:

- **The registry** - records registered once and removed with their owner,
  typed (a Framebuffer attachment or a texture), each stamped with the frame
  it is current for.
- **A two-phase frame** - every layer's offscreen work first (`Prepare`),
  then every layer's drawing onto the caller's framebuffer (`Render`).
- **Read declarations** - so producers can make products only when someone
  wants them.
- **Shared objects** - a cache of compiled shader objects, one fullscreen
  quad, and the neon's gradient ring as the debug layer's input.
- **A render-target pool** - for temporary targets.

Three findings decide its shape:

1. **None of the neon's buffers holds the neon's picture.** Its reduced
   buffer (`NeonRenderer.Scaled`) has the halo and bloom, but not the
   filament (I62 - the edge ring redraws it), the one-sided cut or the
   cutoffs (the blit applies those after reading it). A layer that reads it as
   "the glow" sees light where none is drawn. So a cross-layer read must go to
   a **public product**: a buffer with a written contract that the neon's
   optimisations have to keep. It must never go to an internal intermediate,
   whose contents the next perf change is free to move. Section 3.5.
2. **Pooling saves no memory across today's layers once the frame has two
   phases.** The only temporary buffers are the flare's and the spotlight's
   reduced buffers. Each is written in its layer's offscreen pass and read in
   its blit. Under the two-phase frame both are alive across the phase
   boundary at once, so they cannot share memory. And at their default scale
   of 1.0 neither exists. The pool earns its place as the allocator for
   on-demand products and scratch targets, not as a way to merge those two.
   Section 3.7 and decision 2.
3. **At the default config only the neon has offscreen passes.** It already
   runs them all before its own on-screen passes, so the default frame is
   already in two phases. The two-phase frame changes the order only when the
   flare or the spotlight is below 1.0, or once a product exists. Its first
   value is that any layer's `Render` can read any layer's product, whatever
   the compositing order. Its second is fewer stores and reloads of the
   caller's target on a tiler.

The steps (section 4), ordered so that each one is shippable alone and the
early ones are byte-identical. The shader cache comes first because it is the
earliest gain that can be measured (startup and first-frame time), and it
needs nothing from the registry:

| step | what | closes | pixels |
| ---- | ---- | ------ | ------ |
| S0 | a frame-hash, memory and startup baseline over all five layers | - | - |
| S1 | `ResourceContext`, with the shader-object cache as its first member | - | identical |
| S2 | registry v2: owned, typed, stamped records | I64's open case, I65 to I73 | identical |
| S3 | the two-phase frame (`Prepare` / `Render`) | - | identical |
| S4 | read declarations, demand and ordering | - | identical |
| S5 | the shared quad, and the gradient ring shared with the debug layer | - | identical (but the debug strip during a fade) |
| S6 | the neon's glow as a public product (P-a), the shared mask chunk, droplets lit by the glow | - | identical with the pilot off |
| S7 | the render-target pool | - | identical |

---

## 1. Where things stand

### 1.1 What each renderer owns

| renderer | programs | offscreen buffers | tables and blocks | geometry |
| -------- | -------- | ----------------- | ----------------- | -------- |
| `NeonRenderer` | 11 (emission, glow cover, cover fill, gather, shade, field bake, field composite, fill, blit, ring, ring-field composite) | emission table, glow coverage table (+ the segments' own), gather (1-2 attachments), glow field, ring field, reduced buffer - all RGBA16F / R16F / RG16F with RGBA8 / RG8 fallbacks | gradient ring, segment atlas and arc atlas LUTs (RGBA8); loop-sample, segment and arc blocks | fullscreen NDC quad, gather quad, blit and ring meshes |
| `DropletsRenderer` | 1 | - | - | the band's four strips |
| `LensFlareRenderer` | 2 (flare, blit) | reduced buffer below 1.0, RGBA8, viewport x scale (2.1 MB at 1080p and 0.5) | ghost block | fullscreen NDC quad |
| `SpotlightRenderer` | 2 (lamps, blit) | reduced buffer below 1.0 with no lamp clipped, RGBA16F / RGBA8 (4.1 MB at 1080p and 0.5) | - | lamp strips, fullscreen NDC quad |
| `DebugRenderer` | 4 (LUT strip, stop marker, wireframe, buffer view) | - | its own gradient ring LUT | strip, unit quad, wireframe loop |

### 1.2 What is duplicated

- **`neon.vert` is compiled up to 17 times**: by the neon's 11 programs, the
  debug layer's 3, the droplets', the flare's blit and the spotlight's blit.
  Each `ShaderProgram` compiles its own copy. A `ShaderObject` that compiles
  a stage once and links it into several programs was prototyped on
  2026-10-14 and parked in `stash@{1}` ("bk shared object", from
  `improve_neon_perf2`). Within the neon alone it took the M2's first frame
  from 57-60 to 47-50 ms.
- **Three identical fullscreen NDC quads**: the neon's
  `mFullscreenVertexArray`, the flare's `mVertexArray` and the spotlight's
  `mBlitQuad` - six vertices each, attribute 0, `vec2`.
- **The gradient ring is baked twice**: the neon's `mGradientLUT` and the
  debug layer's, from the same stops. The debug layer's copy keeps
  `NeonRenderer` free of debug members. It brings its own bake, tick, gate
  (`IsStripVisible`) and snap-on-re-show rule (`mStripVisible`).
- **Not duplicates**: the three blit shaders. `neon-blit.frag` applies the
  cut and the cutoffs. `spotlight-blit.frag` dithers after its read.
  `blit.frag` does neither. Their programs stay separate; their vertex
  shader is shared.

### 1.3 The registry today

Covered in `CLAUDE.md` and in I64-I73 of the review findings:

- Records last one frame and are published again every frame.
- Only Framebuffers can be published.
- The key is a name plus an attachment, and nothing keeps it unique.
- The debug view is the only consumer.

I64 is fixed by clearing the registry when a renderer leaves the effect. Its
remaining case - kept renderers drawn on their own after the effect is gone -
and I65 to I73 are open.

### 1.4 The frame today

`EdgeLightingEffect::Render` calls each renderer's `Render` in layer order.
Each layer does its own offscreen passes, then draws onto the caller's
framebuffer. The neon keeps that order inside itself on purpose (`Render` is a
two-phase pass schedule), "so a tiler never stores and reloads the caller's
target mid-frame". Across layers nothing enforces it. With the flare at 0.5,
the frame goes: neon offscreen, neon on screen, flare offscreen, flare on
screen.

---

## 2. Rules the design keeps

1. **One writer.** Only the owner writes a resource. A consumer binds it for
   sampling and nothing else. It must not:
   - render into it;
   - resize it;
   - call `glTexParameter` - filter and wrap are the owner's, so a consumer
     samples with `texelFetch` or a sampler object (core in ES 3.0);
   - call `glGenerateMipmap`.

   A consumer that needs another filter or format makes its own copy, in a
   pooled target.
2. **The neon's caches stay the neon's.** Nothing a consumer does can trigger
   a re-bake or defeat a skip (`mOffscreen.reusable`, `GatherInputs`,
   `LazyBake`). Consumers read what the frame made. They never ask for more.
3. **One program object per pass target and blend state.** One program that
   draws an offscreen buffer unblended and the caller's framebuffer blended
   in the same frame measured 93-624x slower on the AMD macOS driver (build
   AB, `neon-resolution-scale-plan.md` section 7). So the cache shares
   SHADER objects, never a program across passes.
4. **The restricted GL targets.**
   - No bare uniform arrays.
   - Float render targets always come with a fallback.
   - One GL context; nothing is shared across contexts (a VAO cannot be,
     even within a share group).
5. **Steady state allocates nothing**, and records stay trivially copyable.
6. **Host state stays the host's.** Every pass restores the framebuffer,
   viewport and blend state it was handed.
7. **Byte-identical** for every step that is not a new feature, checked as
   the step says.

---

## 3. The design

### 3.1 `ResourceContext`

One object per effect, `core/resource-context.h`, created by the effect and
held by `shared_ptr`. It is introduced in S1 holding only the shader cache
(3.6), and each later step adds its member. `AddRenderer` hands it over
(`BaseRenderer::SetResourceContext`, which replaces `SetBufferRegistry`). It
holds:

- the registry (3.2);
- the frame counter;
- the read list (3.4);
- the shader-object cache and the shared quad (3.6);
- the pool (3.7).

A renderer drawn without an effect has no context. The `BaseRenderer` helpers
then fall back to what each renderer does today - its own quad, its own
compile - so `neon-guide-figures`, the probes and a standalone host keep
working.

### 3.2 The registry: owned, typed, stamped

- **Registered once.** The owner registers a resource when it creates it - in
  `Initialize`, or on the first allocation - and gets back a small handle
  (an index). The record REFERS to the owner's object, as today, never to a
  copied GL name, because the owner reallocates. It takes either a
  `Framebuffer` and an attachment, or a `Texture2D` (for a LUT, such as the
  gradient ring in 3.6).
- **Removed with its owner.** Each record names its producer. `~BaseRenderer`
  removes every record whose producer is `this`. `SetResourceContext` moves
  them when a renderer joins another context. A record therefore lives
  exactly as long as what it points at, whatever the effect does. That closes
  I64's open case, and the two-effects case the I64 fix had to leave alone.
  The effect's own clears from that fix become redundant; keep them as a
  backstop.
- **One string key.** `NeonRenderer.Gather[1]`; attachment 0 drops the
  suffix. Registering a key that is taken is refused and logged (I65). The
  debug view stores one string (I68). `el_effect_get_buffer_name` returns the
  same form. A name longer than `EL_BUFFER_NAME_CAPACITY` is refused at
  registration and at `el_effect_set_debug_buffer_view` (I71).
- **Stamped per frame.** The owner calls `MarkCurrent(handle, placement)`
  when the content describes this frame: written this frame, or a cache it
  checked and found unchanged. It bumps the record's `version` when the bytes
  changed. A consumer asks `IsCurrent()`, and keys its own caches on
  `version` - the idiom `BaseLUT::GetUploadCount` already serves for the
  neon's emission table. A failed bake does not stamp (I66).
- **Two visibilities.**
  - `PUBLIC`: a product with a contract (3.5) that any layer may bind in
    production.
  - `DEBUG`: an intermediate that only the debug view and a host UI list. Its
    contents may change with any optimisation.

  Every buffer published today becomes `DEBUG`.
- **Cost.** Registration happens once. A frame does one store per stamped
  record, with no search (I70). The debug UI's list is the registered
  records, so it is complete before anything is selected - the case I70 noted
  a "wanted" flag alone would break. I67 (protected methods named like
  private ones) and I73 (unknown formats) are fixed in the same change.

### 3.3 The two-phase frame

`BaseRenderer` gains `virtual void Prepare(int w, int h, float time, const
Config &config) {}`. `EdgeLightingEffect::Render` becomes:

1. advance the frame counter;
2. collect reads (3.4);
3. call `Prepare` on every renderer, in the order 3.4 sets;
4. call `Render` on every renderer, in layer order.

`Prepare` draws only offscreen. `Render` draws only onto the caller's
framebuffer. A renderer that does both inside `Render` - every `CUSTOM` one
written to date - still works; it only misses the ordering.

Per layer:

- **Neon.** Passes 0 to 1r go to `Prepare`. The values its on-screen passes
  read go into a `FramePlan` member, filled by `Prepare` and read by
  `Render`: today's locals (the full-res transform, the regions and UV maps,
  the field flags, `centerFull`). The transform is still built ONCE and
  passed to both the blit and the ring, the property the partition rests on.
- **Flare and spotlight.** The pass into the reduced buffer goes to
  `Prepare`; the blit stays in `Render`.
- **Droplets and debug.** No change; neither has an offscreen pass.

Each `Prepare` still captures and restores the host's target
(`RenderTargetState`), as the passes do today.

It earns its place in two ways:

- **Any `Render` can read any `Prepare`'s product.** Products are made before
  anything is composited, so a layer can read one regardless of where it sits
  in the compositing order. Only a product that reads another product needs
  an order (3.4).
- **A tiler stores the caller's target once per frame**, rather than once per
  layer with an offscreen pass. Apple's GPUs are tilers, so the M2 can
  measure it. So can the target device.

### 3.4 Reads, demand and order

`virtual void CollectReads(const Config &config, ReadList &out) const {}`:
each renderer names the keys it will bind this frame. The read list is the
union, rebuilt each frame into a vector that keeps its capacity. It is used
three ways:

- **Demand.** A producer builds an on-demand product only while some layer
  reads it (`IsWanted(key)`), so a product costs nothing on a frame nobody
  reads it. The debug view's selection becomes one read.
- **Order.** `Prepare` runs in layer order, except that a renderer whose
  `Prepare` reads another's product runs after it - a stable topological
  sort. A cycle is refused, logged once, and runs in layer order. No product
  that would need this exists until a product reads a product.
- **Diagnostics.** A read with no registered producer is logged once per key
  per config change, not every frame.

### 3.5 Public products: a contract, not an internal

A product is PUBLIC only with a contract written next to its registration
saying three things:

- what its texels mean;
- which box of the frame they cover;
- which of the producer's optimisations it constrains.

The contract is then an invariant of the producer, like the field's
factorisation is of `neon.frag`.

**The neon's glow.** The first product a cross-layer effect would want. Two
ways to make it - decision 1, settled as P-a:

- **P-a: publish the reduced buffer as it is, with the mask as shared code.**
  `NeonRenderer.Glow` is `mScaledBuffer`. Contract: premultiplied,
  tone-mapped halo and bloom, at the reduced scale over the published box,
  BEFORE the one-sided cut and the cutoffs, without the filament. A consumer
  applies the mask itself with `neonEdgeMask` from a new chunk,
  `neon-mask.glsl`. Every renderer already receives the whole `Config`, so it
  has the mask's inputs; a static `NeonRenderer::GetEdgeMask(config)` gives
  the CPU side.

  The chunk is one more dedupe. The blit, the edge ring's post-grade mask and
  the ring field's composite carry that mask copied "term for term" today
  (`UploadEdgeMaskUniforms`); it becomes one copy.

  - Producer cost: nothing.
  - What it constrains: the reduced buffer must keep holding halo and bloom
    unmasked over the published box. I62 (no filament) and I74 (no pass 1b
    while the blit is empty - parked in `stash@{0}`) both fit, the second as
    "not current".
- **P-b: make a masked copy on demand.** While some layer reads
  `NeonRenderer.Glow`, one more reduced-scale pass, in the blit's place,
  writes the masked glow into a pooled target.
  - Consumer cost: nothing - a plain texture.
  - Producer cost, only while read: one reduced-scale pass and its buffer
    (4.1 MB of RGBA16F at 1080p and 0.5 for a viewport-sized rect).
  - What it constrains: only the copy.

Decided: **P-a**. It costs nothing, the chunk removes a hand copy, and every
consumer is a layer of this library, compiled against the same chunk. P-b is
the answer once something outside the library - a host, or a `CUSTOM`
renderer from another codebase - wants the glow.

**The pilot consumer** (decision 3). An effect that shows the design works
end to end and gives S6 something to measure. Decided: **droplets lit by the
glow.** The droplets' band hugs the perimeter where the glow is. Today the
drops are self-lit; under the pilot they also take the glow's colour at their
position, through a new `DropletsConfig` field whose default of 0 reads
nothing and leaves the frame byte-identical. The droplets read in `Render`,
so the pilot needs no ordering. The alternative: the flare's sun taking its
colour or intensity from the glow at its perimeter position.

**Not planned: the composite so far as an input** - what is behind a layer,
for refraction. It needs a copy of the caller's framebuffer in the middle of
the frame. That breaks the one-store-per-frame property 3.3 buys, and it is
the capture the droplets were designed to avoid.

### 3.6 Shared objects

- **The shader-object cache.** `ShaderCache::Get(stage, source, defines)`
  returns a compiled `ShaderObject` (port it from `stash@{1}`), keyed by the
  source pointer and the defines string. A program links the objects it is
  given and keeps none of its own.
  - Program objects stay one per pass (rule 3).
  - `neon.vert`: up to 17 compiles become 1.
  - The fragment shaders that two of the neon's programs share (shade and
    ring) compile once.
  - Measured by the first frame's time and `Initialize`'s.
  - The neon's lazy build (`ensureGlowPrograms` and the rest) is unchanged:
    a stage is compiled the first time any program needs it.
  - A compile failure is recorded in the cache, so a second program that
    needs the same stage does not retry it. That keeps `mFailedPrograms`'s
    log-once rule across renderers.
- **The fullscreen quad.** One VAO from the context, for the neon, the flare
  and the spotlight. It saves about 150 bytes. The point is one definition of
  the NDC quad, not memory.
- **The gradient ring.** The neon registers `mGradientLUT`'s texture as
  `NeonRenderer.GradientRing` (DEBUG), and the debug layer's strip reads it.
  The debug layer's own LUT goes, with its bake, its tick and
  `mStripVisible`. The strip then shows exactly the ring the glow samples,
  cross-fade included. That is a behaviour change, and arguably the fix:
  today a re-shown strip snaps to the current stops while the glow may still
  be fading. Byte-identical on steady frames; different during a fade and on
  the frame the strip is re-shown (decision 5). `NeonRenderer` still has no
  debug member - it registers, it does not draw.

### 3.7 The render-target pool

`RenderTargetPool::Acquire(desc)` returns a `Lease` (RAII, move-only):

- **The descriptor**: size, a list of format tiers walked like
  `ResizeInBestFormat`, attachment count, filter.
- **Allocation**: texel counts are rounded up to a step, as
  `REGION_ALLOC_STEP` does, so a resizing rect does not reallocate every
  frame.
- **Return**: a lease goes back to the pool when it is released.
- **Release**: a pooled target unleased for a few seconds of frame time is
  freed, as `GLOW_COVER_RELEASE_SECONDS` does for the coverage table.
- **What may lease**: temporary targets only. That means on-demand products
  (P-b), a consumer's scratch targets (a blur's ping-pong), and the flare's
  and spotlight's reduced buffers. Never one of the neon's caches (rule 2).

**What it saves, honestly.**

- At the defaults: nothing. The flare and the spotlight are at 1.0, where
  neither allocates.
- At 0.5 and 1080p: the flare's 2.1 MB of RGBA8 and the spotlight's 4.1 MB of
  RGBA16F. They could share one 4.1 MB allocation only if their lifetimes did
  not overlap. Under 3.3 both live from `Prepare` to `Render`, so they
  overlap, and the pool saves 0 MB.
- Sharing them would need those two layers to run `Prepare` and `Render` back
  to back (decision 2). That is the per-layer frame 3.3 replaces. It also
  costs a pass whenever the two formats differ: an RGBA8 lease cannot serve
  the spotlight's half-float tail (V27).

So the pool comes last (S7), built when S6 or a later consumer has a
temporary target to give it.

---

## 4. Steps

Each step ends with the tree building, `neon-scale-check check` and
`partition` passing, and the step's own check.

**S0 - baseline.** A probe, later a mode of the check tool, that hashes the
frame across all five layers, captured through `CaptureUtil`.

- **Scenes**: the defaults; neon at 1.0 and 0.25; flare at 0.5; the
  spotlight's five-lamp fan at 0.5; droplets on; the debug view on each
  published buffer.
- **Also recorded**: `Initialize` time and first-frame time, split into
  compile and link time; the number of shader compiles; GL memory by buffer;
  the heap-allocation count per frame.
- **Builds**: the same build type throughout - Debug and Release libs differ
  on a few hashes.

**S1 - the context and the shader cache** (3.1, 3.6). `ResourceContext` is
created by the effect and handed over by `AddRenderer`, holding only
`ShaderCache` for now. `ShaderObject` is ported from `stash@{1}`. Every
program in the five renderers links cached stage objects, and keeps none of
its own. Program objects stay one per pass (rule 2.3), and the neon's lazy
builds are unchanged. A renderer drawn without an effect compiles privately,
as today.

- **Frames**: identical to S0.
- **Startup**: `neon.vert` compiled once instead of up to 17 times.
  `Initialize` and first-frame time measured against S0 on the M2 and the
  AMD.
- **Failure**: a stage forced to fail is logged once and not retried, however
  many programs need it. `mFailedPrograms` still keeps the frame drawing the
  fill.

**S2 - registry v2** (3.2). The registry moves into the context, with owned
records, string keys, stamps and visibilities. The debug view and both demos
are ported. The C ABI's listing keeps its signatures. `SetBufferRegistry`
goes; the protected methods take public-style names. Closes I64's open case
and I65 to I73.

- **ASan**: the I64 probe extended to kept renderers after the effect and to
  one renderer in two effects - clean.
- **Debug view**: texel-exact against S0.
- **Allocations**: none in the library on any steady frame - `layer-check`'s
  `alloc` line 0 on every scene, as since I75 (section 7.1). The driver's own
  line, `alloc-driver`, is listed but not a bar.
- **Frames**: identical to S0.

**S3 - two phases** (3.3).

- **Frames**: identical to S0 at every scene and scale.
- **`partition`**: passes (the transform is still built once).
- **Timing**: `neon-scale-check time` still and hue, plus the flare and the
  spotlight at 0.5, on the M2 and the AMD - no slower.

**S4 - reads** (3.4).

- **Frames**: identical.
- **Order**: a probe with a deliberate cycle - refused, logged once, drawn in
  layer order.

**S5 - the quad and the gradient ring** (3.6), one commit each.

- **Frames**: identical, but the debug strip during a fade and on the frame
  it is re-shown, where it now follows the glow's live ring.
- **Memory**: the debug layer's LUT texture and its CPU bake are gone.

**S6 - the glow product and the pilot** (3.5).

- **The chunk**: moving the mask into `neon-mask.glsl` leaves the blit and
  the ring byte-identical.
- **The product**: `NeonRenderer.Glow`, P-a, PUBLIC, with its contract
  written into `neon-renderer.h` beside the registration and into
  `CLAUDE.md`'s neon section, which lists the neon's invariants.
- **The pilot**: droplets lit by the glow, behind a `DropletsConfig` field
  whose default leaves the frame identical. Its cost measured with it on.

**S7 - the pool** (3.7), when there is a temporary target. The flare's and
the spotlight's reduced buffers stay out of it.

- **Frames**: identical.
- **Allocation**: no reallocation per frame under a resizing rect.
- **Release**: the idle release frees what it should.

---

## 5. Decisions

Settled by the owner on 2026-10-15, each as recommended.

1. **The glow product: P-a** (3.5). Publish the reduced buffer with its
   contract; consumers apply the mask from `neon-mask.glsl`. P-b waits for a
   consumer outside the library.
2. **The pool leaves the flare and the spotlight alone** (3.7). Their
   reduced buffers keep their own allocations, so the frame keeps its two
   phases below 1.0. The pool is built last.
3. **The pilot: droplets lit by the glow** (3.5).
4. **The key: one string**, with an `[n]` suffix for an attachment other than
   0 (3.2).
5. **The debug strip shows the live ring** (3.6). The snap on re-show goes
   with the debug layer's own LUT.
6. **The API.**
   - `SetBufferRegistry` becomes `SetResourceContext`. That costs nothing
     outside this branch, since the registry was never merged to `main`.
   - `publishBuffer` becomes registration plus `MarkCurrent`, and the
     protected methods take public-style names (I67).
   - The C ABI keeps every export's arity: the buffer listing returns the new
     key form, and `el_effect_set_debug_buffer_view(name, attachment)` takes
     either form.
7. **The order: the shader cache first** (S1). It is the earliest measurable
   gain, and it needs nothing from the registry.

---

## 6. Out of scope

- Host interop: textures in, products out to the host. P-b is where it would
  start.
- Sharing across GL contexts or threads.
- The composite so far as an input (3.5).

---

## 7. Results

### 7.1 S0 - the baseline

`tools/layer-check` (README there).

- **The scenes.** A report hashes 248 frames over 62 scenes. Two runs of the
  same binary agree on every hash, every memory figure and every allocation
  count, so a diff between two builds measures the change alone.
- **Startup**, Debug build, Apple M2 Pro, warm driver cache, five runs:
  - `Initialize`: 14.2-15.0 ms, of which compiles 8.9-9.5.
  - The all-programs first frame: 119.7-123.2 ms, of which compiles 37.3-38.6
    and links 17.5-18.0. Most of the rest is the driver finishing each
    program on its first draw.
  - 38 compiles for 19 programs. `neon.vert` is compiled 16 times; 17 with the
    opaque fill's program, which this frame does not build. `neon.frag` is
    compiled twice.
  - A cold driver cache, the first run after a build, put that frame at
    ~500 ms.
- **Memory**, textures plus buffers, for a 640 x 360 rect in 1280 x 720:

  | scene | textures |
  | ----- | -------- |
  | the neon at 0.5 | 2.1 MB |
  | the same with the hue rotating | 2.6 MB (the field) |
  | adding the flare at 0.5 | 3.1 MB |
  | adding the spotlight fan at 0.5 instead | 4.0 MB |
  | all five layers below 1.0 | 6.4 MB |

  Buffers are 43 KB in every scene.
- **The allocation counts were traced: almost all of them are the GL
  driver's** (I75 in [`review-findings.md`](review-findings.md)). The tool
  replaces the global `operator new`. On macOS that also catches Apple's GL
  stack, which is C++:
  - 24 B per draw;
  - 40 B each time drawing moves to another framebuffer;
  - 128 B whenever the driver starts a command buffer, plus 24 B per GL
    object it touches. That was every few frames here, because the tool never
    flushed. A host's swap starts one every frame.

  On still frames S0's count was the draw count exactly: 2 for still neon,
  3 with droplets, 8 with the debug overlays.

  The library itself allocated in three places, all on neon config-change
  paths. So only intensity-pulse frames (5 a frame) and travelling-segment
  frames (3) did:
  - `OnlyIntensityMoved`'s config copy;
  - `setupRingGeometry`'s local vectors;
  - `FillEffectiveSegments`'s re-copy.

  It allocated nothing on still or hue frames, nor with the debug view on.
  All three are fixed since, with all 248 hashes identical. The tool now
  files each allocation by caller:
  - `alloc` is the library's. It reads 0 on every scene.
  - `alloc-driver` is the driver's.

  It counts only the thread that runs the frame. `report --flush` makes the
  driver's line steady: 15 a frame for still neon, 40 under a hue, 72 with all
  five layers. So S2 and later steps hold rule 5 as written: `alloc` 0 on
  every scene.

### 7.2 S1 - the context and the shader cache

`core/resource-context.h`, `gl/shader-cache.h`, `ShaderObject` in
`gl/shader-program.h`, `BaseRenderer::BuildProgram`. Every program in the five
renderers is built through `BuildProgram`; the neon's `WithDefine` moved into
the cache as `ShaderCache::WithDefines`.

- **Frames**: all 248 identical to S0.
- **Compiles**: 38 -> 22, one per distinct source, `neon.vert` included.
- **Startup**, six interleaved runs of each build:

  | | before | after |
  | - | ------ | ----- |
  | first frame | 116.6-119.9 ms | 108.3-113.8 ms (~9 ms, 1.08x) |
  | `Initialize` | 13.9-15.3 ms | 13.6-14.3 ms |

  The compile time inside the first frame fell from ~37.7 to ~28.3 ms; the
  links and the driver's first-draw work are unchanged, since every pass
  still has its own program object.
- **Failure**: a `neon.vert` forced to fail is compiled once and logged
  once. Before, it was compiled and logged 6 times, once per renderer that
  tried. A `neon.frag` forced to fail is compiled once, as before: all five
  layers stay, and the frame still draws.
- **Without an effect**: a `NeonRenderer` drawn on its own compiles
  privately and draws byte-identically to the same renderer inside an effect.
  Clean under AddressSanitizer, as are the I64 probes.
- **Allocations**: unchanged in every scene but one. In the stop-fade scene
  one frame read 222 against S0's 220. That is the driver: that frame's ring
  upload is the process's first, and it builds a Metal pipeline whose
  allocations vary by run (160-237 in I75). A third run read 225. `diff`
  will always list that line. (Since I75's fix the tool counts only the
  thread that runs the frame, and that frame's `alloc-driver` reads 157 on
  every run.)
- **Not measured**: the AMD 5300M, and the target device. Linking one shader
  object into several programs is standard GL and GLES 3.0 behaviour, but a
  driver that mishandled it would show here first. A renderer can always be
  driven without a context, which compiles privately as before.
