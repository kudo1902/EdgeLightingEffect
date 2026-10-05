#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"
#include "renderer/span-atlas-lut.h"
#include "renderer/gradient-ring-lut.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// The neon renderer.
    ///
    /// Draws a tight quad over the rect + glow-reach margin and runs one
    /// fragment shader that composes filament + halo + bloom.
    ///
    /// Per-fragment work: an analytic rounded-box SDF, plus a gather loop over
    /// @c NeonConfig::numSamples perimeter samples (positions live in a UBO)
    /// that costs two @c texelFetch calls per sample into @c uEmission - the
    /// table baked by the emission pre-pass - or ONE where the config carries
    /// no segments, since the pre-pass then writes row 1 as all zeros and the
    /// shader takes a second, shorter loop body. That branch is on a uniform
    /// and sits OUTSIDE the loop; see the note at the gather in neon.frag for
    /// why inside was measured slower than not branching at all. The
    /// per-sample arc scan, segment
    /// loop and filtered LUT reads that used to run here are all
    /// fragment-invariant and moved to @c neon-emission.frag, which is why the
    /// per-fragment cost no longer scales with the arc or segment count. See
    /// docs/emission-prepass.md.
    ///
    /// The three baked LUTs stay bound, but for the POINTWISE reads only - the
    /// colour-stop alpha, taken at the fragment's own perimeter position:
    ///   - @c uGradientLUT      - base colour RING (REPEAT, cyclic).
    ///   - @c uSegmentLUT       - per-segment atlas, one row per segment
    ///                            (CLAMP, head-to-tail SPAN).
    ///   - @c uArcLUT           - per-arc atlas, one row per arc (CLAMP, SPAN).
    ///
    /// Arc gating splits in two, and the halves use different rules on purpose.
    /// The gather's HUE resolves overlap winner-take-all per sample (largest
    /// @c mask*intensity), decided in the pre-pass. The EMISSION comes from
    /// @c arcCoverContinuous evaluated at the fragment's own perimeter
    /// position and combined across arcs with @c max. See neon.frag for the
    /// full compose. Visual parameters come from @c Config::neon;
    /// @c NeonConfig::arcs and @c NeonConfig::segmentBoosts drive their
    /// respective UBOs and atlases.
    ///
    /// --- Resolution scale -----------------------------------------------
    ///
    /// One renderer, two paths, chosen by @c NeonConfig::resolutionScale:
    ///
    ///   1.0  - the gather draws straight onto the framebuffer it was handed.
    ///          No offscreen buffer, no blit, nothing allocated.
    ///   <1.0 - the gather runs ALONE (neon-gather.frag) into
    ///          @c mGatherBuffer, at a scale set by the gather's own smoothness
    ///          rather than by @c resolutionScale - about 2 texels per colour
    ///          kernel, so typically far coarser (@ref GetGatherScale). Then a
    ///          variant that reads that result instead of running the loop
    ///          (NEON_READS_GATHER) shades @c mScaledBuffer at the requested
    ///          fraction, which is bilinear-blitted back over the area that
    ///          can still be lit outside a thin ring around the rect edge, and
    ///          shades that ring at full resolution. The loop - ~95% of the
    ///          neon's cost - runs on a few thousand texels, while the
    ///          filament, the cut and the cutoffs near the line still come out
    ///          as the direct path draws them. Both buffers cover only the
    ///          part of the frame their readers reach (@ref GetBufferRegion).
    ///          See docs/neon-resolution-scale-plan.md sections 12 and 13.
    ///
    /// At 1.0 with @c NeonConfig::decoupledGather, the 1.0 path borrows the
    /// first half of that: the gather runs alone into @c mGatherBuffer, and
    /// pass 1 is the glow quad at full resolution shaded from it by the edge
    /// ring's program (@ref renderDecoupledNeonPass) - no reduced buffer, no
    /// blit, no ring. Within 1-2/255 of the exact 1.0 render and 2-5x
    /// faster; off by default because it gives up that exactness.
    ///
    /// The paths share one schedule: every pixel-valued uniform is multiplied
    /// by the scale unconditionally (a no-op at 1.0) and the shader converts
    /// its own full-res px constants with @c uResolutionScale. Only the render
    /// target, the program variants, the gather pass, the blit, the ring and
    /// the buffer allocations are conditional - and at exactly 1.0 none of
    /// them runs, which is what keeps that path bit-identical to the dedicated full-res renderer
    /// this class used to have as a separate fork (@c NeonOptimizedRenderer,
    /// removed).
    ///
    /// The gather loop itself lives in neon-common.glsl, injected into both
    /// neon.frag (which runs it inline at 1.0) and neon-gather.frag (which runs
    /// it alone below 1.0), so the two paths share one copy of it.
    ///
    /// The neon programs and the blit are built per PATH, the first frame that
    /// path renders (@ref ensurePathPrograms): one program at 1.0, two at 1.0
    /// decoupled (the gather and the full-resolution shading), four below it
    /// (those two, the reduced shading - one object per target - and the
    /// blit). A host that stays on one path never compiles the others', and
    /// the price is a one-time compile on the first frame after a switch.
    ///
    /// Debug overlays (LUT strip, colour-stop markers) are NOT here - they are
    /// a separate layer, @ref DebugRenderer, driven by @ref DebugConfig. The
    /// one debug field this renderer does read is
    /// @c DebugConfig::opaqueOnly, which selects which of its passes run and
    /// so cannot live anywhere but in the schedule below.
    class NeonRenderer : public BaseRenderer
    {
    public:
        NeonRenderer() = default;
        virtual ~NeonRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;
        virtual RendererLayer GetLayer() const override { return RendererLayer::NEON; }

    private:
        /// Build the programs both paths use - the emission pre-pass and the
        /// opaque fill. The rest are per path; see @ref ensurePathPrograms -
        /// and the glow coverage bake is built when first needed; see
        /// @ref ensureGlowCoverProgram.
        bool setupShaders();
        /// Make sure the glow coverage bake (neon-glow-cover.frag) is built,
        /// building it if not. Called only on a frame that will bake the
        /// table, so a host whose ring is lit uniformly never compiles it. A
        /// failed build is recorded in @c mFailedPrograms and never retried,
        /// and the frame draws the fill alone, as for a path program.
        bool ensureGlowCoverProgram();
        /// Build @p program from @p fragSrc (neon.frag or neon-gather.frag) with
        /// @p define spliced in (none for @c nullptr), once. Returns true if it
        /// is ready. A failed build is recorded in @c mFailedPrograms under
        /// @p programBit and never retried, so a broken driver costs one compile
        /// and one log line, not one per frame. Every program gets the segment
        /// block bound; @p gathers binds the sample block too, @p shades the arc
        /// block - each only where the source declares it.
        bool buildNeonProgram(ShaderProgram &program, const char *fragSrc, const char *define, const char *name,
                              unsigned int programBit, bool gathers, bool shades);
        /// Make sure every program the path draws with is built, building any
        /// that are not. @p decoupled: the gather runs as its own pass (every
        /// scaled frame, and 1.0 under NeonConfig::decoupledGather);
        /// @p scaled: below 1.0. The plain direct path needs neon.frag alone; a
        /// decoupled one needs neon-gather.frag and neon.frag's
        /// NEON_READS_GATHER variant for the full-resolution shading (the ring
        /// below 1.0, pass 1 at 1.0); a scaled one adds that variant again for
        /// pass 1b and the blit - and none of them the plain program. Lazy rather than at @ref Initialize: compiling the path a host
        /// never uses cost a third of the startup and the memory of programs
        /// nothing draws with. The trade is a one-time compile on the first
        /// frame a host switches path. False if any of them failed, in which
        /// case the glow is skipped and the frame degrades to the fill.
        bool ensurePathPrograms(bool decoupled, bool scaled);
        /// Upload the static NDC quad the fullscreen passes draw. Called once
        /// from @ref Initialize: the quad is in clip space, so unlike
        /// @ref setupGeometry's it is independent of the geometry, the
        /// viewport and the resolution scale, and never needs rebuilding.
        void setupFullscreenQuad();
        void setupGeometry(const Config &config);
        /// Build @c mFillVertexArray: the geometry that BOUNDS the opaque
        /// fill, as a rectangular annulus in full-resolution rect-local pixels.
        ///
        /// The fill used to rasterise the whole viewport and let the fragment
        /// shader @c discard everything outside the band. That is the pattern
        /// @ref setupGeometry exists to avoid ("geometry bounds the far region
        /// instead of a per-fragment discard"), and it cost the same whether
        /// the band was 20 px or the entire screen - measurably, a fixed
        /// full-viewport charge on every frame with @c opaqueMode set.
        ///
        /// Builds nothing (@c mFillVertexCount 0) whenever the fill's coverage
        /// is 1 at every pixel - @c ALL by definition, and @c BOTH with both
        /// of the fill's cutoffs (@c NeonConfig::opaqueInsideCutoff /
        /// @c opaqueOutsideCutoff) disabled by arithmetic (their default state,
        /// so the common way in). Those modes need no bounding geometry either way:
        /// @ref renderOpaqueFill clears for them, and on the rare state where
        /// a clear would not clip like a draw it falls back to the static
        /// fullscreen quad, never to a ring. Both passes ask one shared
        /// predicate so they cannot disagree; see @c FillsWholeViewport.
        void setupFillGeometry(const Config &config);
        /// Build the scaled path's two FULL-RES partition arrays, from one set
        /// of box coordinates: @c mRingVertexArray, a rectangular annulus over
        /// the edge ring (GetRingWidth either side of the edge, clipped to the
        /// band the glow can still be lit in), which pass 2c re-shades at full
        /// resolution; and @c mBlitVertexArray, which pass 2b composites the
        /// reduced buffer over - the ring's complement, bounded to where the
        /// glow can be non-zero (its lit band and its fade margin), and so
        /// possibly empty.
        ///
        /// The two share their boundary vertices bit for bit, so the rasteriser
        /// gives every pixel to at most one of them - both composite
        /// premultiplied-over, and a pixel drawn by both would composite twice.
        /// Builds neither (both counts 0) at scale 1.0, where neither pass runs.
        /// Also builds the gather pass's quad, through @ref setupGatherGeometry:
        /// below 1.0 over pass 1's quad and the ring, at 1.0 under
        /// NeonConfig::decoupledGather over pass 1's quad alone, otherwise
        /// nothing. Reads @c mQuadMargin and @c mGlowOuter / @c mGlowHole, so it
        /// runs after @ref setupGeometry.
        void setupRingGeometry(const Config &config);
        /// Build @c mGatherVertexArray and @c mGatherOuter: the box
        /// @p readOuter less the hole @p readHole (half-extents, full-res px)
        /// that the shading passes read the gather at, each edge moved out by
        /// the gather's bilinear footprint, in the SCALED space of @p scale.
        void setupGatherGeometry(const glm::vec2 &readOuter, const glm::vec2 &readHole, float scale,
                                 const Config &config);
        void rebuildLoopSamples(const Config &config);
        /// Re-bake the three colour LUTs. Each wrapper self-guards, so this is
        /// called unconditionally on every config change; see the note at the
        /// definition for what does and does not dirty a LUT.
        void bakeLUTs(const Config &config);

        /// Size @c mEmissionBuffer to the emission table's fixed dimensions, in
        /// the best format the driver will give - walking the candidate list in
        /// preference order.
        ///
        /// Named for the BUFFER, not the table it carries: this allocates the
        /// resource, it does not fill it. The contents are written by
        /// @ref renderEmissionPass, which is where "table" belongs.
        ///
        /// Called ONCE, from @ref Initialize. The table's dimensions are
        /// compile-time constants and its format cannot change once settled,
        /// so there is nothing for a later call to discover - it is not on the
        /// per-frame path at all.
        ///
        /// Named for the @c Framebuffer::Resize it delegates to, and shares its
        /// semantics: creates the attachment on the first call, then no-ops on
        /// every later one where nothing has changed. What it adds is the
        /// format walk, and the buffer's own format is what records how far
        /// down the list an earlier call had to go - so a settled buffer
        /// re-requests what it already holds and Resize early-outs.
        /// @return false only if NO candidate could be allocated, in which case
        ///         there is no attachment at all - see the caller.
        bool resizeEmissionBuffer();

        /// Size @c mGatherBuffer to @p width x @p height with @p attachments
        /// colour attachments, in the best format the driver will give -
        /// RGBA16F, then RGBA8. Called every frame that gathers apart from
        /// @ref renderGatherPass; @c Framebuffer::Resize early-outs when
        /// nothing changed. The format reached is recorded in
        /// @c mGatherFormat, not in the buffer, because the buffer is released
        /// whenever no frame gathers apart.
        /// @return false only if no candidate could be allocated.
        bool resizeGatherBuffer(int width, int height, int attachments);

        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // The numbering is the pipeline order from docs/emission-prepass.md,
        // and the .cpp defines them in this same order - keep all three in
        // step. The data dependency is the real contract: pass 0 bakes the
        // table the gather reads, pass 1a's gather buffer is what pass 1 and
        // pass 2c shade from, pass 2a's fill must land before pass 2b
        // composites the glow over it, and at scale 1.0 pass 1 IS the
        // composite (it gathers itself, draws onto the target directly, and
        // neither 1a, 2b nor 2c runs - or, under NeonConfig::decoupledGather,
        // 1a runs and pass 1 reads it: @ref renderDecoupledNeonPass in place of
        // @ref renderNeonPass, still after the fill). 2b and 2c cover
        // disjoint areas, so their order between themselves is free.
        //
        // NOTE on the DIRECT path @ref Render calls pass 2a before pass 1 -
        // the one place declaration order and call order differ. Pass 1 draws
        // onto the caller's framebuffer there, and the fill has to be under
        // it. On the scaled path the order is the numbering: every offscreen
        // pass (0, 1) first, then everything on the caller's framebuffer
        // (2a, 2b, 2c), so that target is drawn in one unbroken run.

        // STATE OWNERSHIP. `Render` owns blend state - enable and func - and
        // sets it immediately before each pass that depends on it, so no pass
        // touches GL_BLEND and the whole blend timeline reads in one place.
        // A pass owns its shader, and a pass that RETARGETS the framebuffer
        // restores it (an excursion, unlike a mode). Preconditions each pass
        // relies on are stated in its @pre below.
        //
        // The passes take the pieces of the frame transform they actually use.
        // @ref Render derives them once, in SCALED space, so the gather quad
        // and the sample positions agree; the opaque fill is the exception and
        // takes the viewport height, since it always draws at full resolution
        // on the caller's framebuffer.

        /// Make the segment + arc UBOs current and bind them. Called before the
        /// emission pre-pass because BOTH passes read them: the pre-pass to
        /// bake the per-sample emission, the main pass for the continuous
        /// filament gate.
        ///
        /// Repacks only when @c mLightBlocksDirty says the config moved; the
        /// bind is unconditional. See the definition for why the two are
        /// treated differently.
        /// @pre @c mEffectiveSegments is current for @p config - i.e.
        ///      @ref OnConfigChanged has run for any change since the last
        ///      frame, which the effect guarantees by calling Update before
        ///      Render. This method deliberately does not refill it.
        void packLightBlocks(const Config &config);

        /// The pack half of @ref packLightBlocks, split out so the gate reads
        /// as one branch rather than wrapping forty lines of std140 packing.
        void packLightBlockData(const Config &config);

        /// Whether @c mEmissionBuffer's contents still describe
        /// (@p time, @p config), i.e. whether pass 0 has to run at all.
        ///
        /// The table is a pure function of (si, uTime, config) - the same
        /// invariant the pre-pass itself rests on - and the buffer is
        /// allocated once for the renderer's lifetime, so a frame that moves
        /// neither input can read what is already in it. The pre-pass hoists
        /// the gather's fragment-invariant half out of every FRAGMENT; this is
        /// what hoists it out of every FRAME as well.
        ///
        /// Two things can move it:
        ///   - @c uTime, which reaches neon-emission.frag exactly once, as
        ///     `si - uTime * uHueRotationRate`. At a rate of 0 time drops out
        ///     of the table altogether, so a still ring rebakes nothing however
        ///     the clock runs; at any other rate every distinct time does.
        ///   - @c mEmissionDirty, which covers everything else. See its
        ///     declaration for what sets it.
        bool isEmissionTableStale(float time, const Config &config) const;

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer, at the clamped sample count so texel i here is
        /// sample i in the gather. Retargets the framebuffer and viewport, so
        /// it restores both before returning - see docs/emission-prepass.md.
        ///
        /// Records what it baked (@c mEmissionDirty, @c mEmissionTime) on the
        /// way out, so @ref isEmissionTableStale reads a snapshot written by the
        /// only thing that ever writes the buffer.
        /// @pre Blending disabled - a table write is not a composite.
        /// @pre @c mEmissionBuffer is allocated, which @ref Initialize
        ///      guarantees for the renderer's lifetime - hence no failure to
        ///      report and nothing to allocate here.
        void renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Make sure @c mGlowCoverBuffer is allocated, allocating it if not, at
        /// the coverage table's fixed dimensions in the best format the driver
        /// will give. Called only on a frame that will bake the table, beside
        /// @ref ensureGlowCoverProgram - so a host whose ring is lit uniformly
        /// never holds the table's 1 MB - and the buffer is released again
        /// when the layer is disabled (@ref OnConfigChanged).
        ///
        /// The format walk is @ref resizeEmissionBuffer's, but resumed from
        /// @c mGlowCoverFormat rather than from the buffer's own format, since
        /// a released buffer has none - the walk of @ref resizeGatherBuffer.
        /// A fresh allocation holds undefined texels, so it sets
        /// @c mGlowCoverDirty.
        /// @return false only if NO candidate could be allocated, in which case
        ///         the frame draws the fill alone, as for a failed program.
        bool ensureGlowCoverBuffer();

        /// Pass 0b: bake the glow coverage table into @c mGlowCoverBuffer -
        /// for each piece of the emitter, the arcs' and the segments' coverage
        /// over that piece's own extent, weighted by the halo's and the bloom's
        /// kernel about a fragment's foot on it (neon-glow-cover.frag, V20 and
        /// V21 in docs/review-findings.md).
        /// Retargets the framebuffer and viewport, so it restores both before
        /// returning, and clears @c mGlowCoverDirty on the way out.
        /// @pre Blending disabled, and the light blocks packed this frame.
        void renderGlowCoverPass(const Config &config);

        /// The transform and the rect's shape - uMVP, uRectSize,
        /// uCornerRadius - for @p shader at @p scale: everything the gather
        /// pass (neon-gather.frag) reads besides its own inputs, and the first
        /// thing @ref uploadNeonUniforms uploads for every other program.
        /// @pre @p shader is in use.
        void uploadShapeUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale, const Config &config);

        /// Every SHADING uniform the neon programs read, for @p shader at
        /// @p scale, fading against @p quadMargin (px in the same space). One
        /// copy for every program built from neon.frag, so the variants cannot
        /// drift apart in what they are told. Not for neon-gather.frag, which
        /// shades nothing - it takes @ref uploadShapeUniforms alone. The
        /// gather's own inputs - the sample block, the count and the emission
        /// table - are not here: only the gathering programs have them, and
        /// @ref bindGatherInputs binds them.
        /// @pre @p shader is in use.
        void uploadNeonUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale,
                                float time, float quadMargin, const Config &config);

        /// Bind the gather loop's own inputs - the sample block, the count and
        /// the emission table - for @p shader, which is in use. Only the
        /// programs that run the loop have them: the direct path's and the
        /// gather pass's.
        void bindGatherInputs(ShaderProgram &shader, const Config &config);

        /// Bind @c mGatherBuffer's attachments for @p shader (a
        /// NEON_READS_GATHER program, in use), with the affine map from the
        /// pass's vPos onto the buffer's uv.
        void bindGatherBuffer(ShaderProgram &shader, const glm::vec2 &uvScale, const glm::vec2 &uvOffset);

        /// Pass 1a, scaled path only: the gather, into @c mGatherBuffer
        /// (@p gatherWidth x @p gatherHeight texels at GetGatherScale), over
        /// @c mGatherVertexArray, drawn through @p mvp - the gather region's
        /// projection from pass 1's scaled rect-local space. Takes no time:
        /// the loop reads time only through the emission table. Leaves the
        /// buffer bound; @ref Render restores the target.
        /// @pre Blending disabled - the buffer is data.
        /// @return false if the buffer could not be allocated, in which case
        ///         every later glow pass must be skipped.
        bool renderGatherPass(const glm::mat4 &mvp, int gatherWidth, int gatherHeight, float scale,
                              const Config &config);

        /// Pass 1: the neon on the tight glow quad. Reads the emission table
        /// produced by @ref renderEmissionPass (directly, or through the
        /// gather pass), so it must run after it.
        ///
        /// Draws either straight onto the bound framebuffer (@p scaled false),
        /// gathering and shading with @c mNeonShader, or into
        /// @c mScaledBuffer (@p scaled true), @p bufWidth x @p bufHeight
        /// texels drawn through @p mvp - its region's projection - shading
        /// with @c mNeonShadeShader from @c mGatherBuffer through the uv map
        /// @p gatherUVScale / @p gatherUVOffset. The scaled path also clears
        /// that buffer and leaves it bound, and @ref Render restores the
        /// target before pass 2a.
        /// @pre On the direct path, premultiplied-over blending: the glow
        ///      composites over what is already on the target. On the scaled
        ///      path, blending DISABLED: the buffer was just cleared and the
        ///      quad covers each texel once, so over would only have added
        ///      zero, for a destination read per texel.
        /// @return false if the scaled target could not be allocated, in which
        ///         case nothing was drawn and passes 2b and 2c must be skipped
        ///         too - they would otherwise composite a stale or undefined
        ///         buffer.
        bool renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight, bool scaled,
                            const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset,
                            float time, const Config &config);

        /// Pass 1 at 1.0 under NeonConfig::decoupledGather: the glow quad, at
        /// full resolution on the caller's framebuffer through the direct
        /// path's @p mvp, shaded by @c mNeonRingShader from pass 1a's gather
        /// (read through @p gatherUVScale / @p gatherUVOffset, the gather
        /// region's map from full-res rect-local px) instead of walking the
        /// loop per fragment. In place of @ref renderNeonPass, never with it.
        /// @pre Premultiplied-over blending; pass 1a succeeded and the
        ///      caller's target is restored.
        void renderDecoupledNeonPass(const glm::mat4 &mvp, const glm::vec2 &gatherUVScale,
                                     const glm::vec2 &gatherUVOffset, float time, const Config &config);

        /// Pass 2a: opaque-mode background fill (its band ring, or a clear), at
        /// FULL resolution on the caller's framebuffer regardless of the
        /// resolution scale - it is a flat shape from an analytic SDF, so
        /// scaling it would only cost it its clean edges. The fragment shader
        /// reads @c gl_FragCoord, so the shape is still derived in window
        /// space - the transform only places the bounding geometry, and the
        /// viewport is what both are expressed in. Caller guards on
        /// @c opaqueMode != NONE.
        ///
        /// Draws @c mFillVertexArray (the band ring from
        /// @ref setupFillGeometry) for every mode whose coverage is shaped.
        /// A fill that covers every pixel at coverage 1 runs no shader: it is
        /// a scissored @c glClear, bounded by the intersection of the queried
        /// viewport with the host's own scissor. That substitution is dropped -
        /// for the fullscreen quad, shader and all - when @c GL_STENCIL_TEST
        /// or @c GL_DEPTH_TEST is enabled, since a clear ignores both and would
        /// paint through a mask the host set up to clip this pass.
        void renderOpaqueFill(int viewportWidth, int viewportHeight, const Config &config);

        /// Pass 2b: bilinear composite of the scaled buffer onto the caller's
        /// framebuffer, AND the one-sided glow cut and the inside/outside
        /// cutoffs. Only runs when the scaled path did - on the direct path
        /// @ref renderNeonPass owns the cut, because there the gather already
        /// runs at the destination rate. Covers what can still be lit outside
        /// the edge ring (@c mBlitVertexArray), which pass 2c draws - and
        /// draws nothing when that is empty.
        ///
        /// The cut lives here rather than in the gather because the gather's
        /// output is upsampled: an edge drawn at resolutionScale is smeared
        /// 1/scale destination pixels each way, across the line as well as
        /// along the lit side. Draws through @p mvp, the full-res transform
        /// @ref Render builds once for this pass and pass 2c together - the
        /// partition depends on both using one matrix. @p centerFull is the
        /// rect centre in gl_FragCoord's y-up space, mirrored as
        /// @ref renderOpaqueFill mirrors it.
        /// @pre Premultiplied-over blending, and the caller's framebuffer and
        ///      full-resolution viewport are restored.
        void renderBlitPass(const glm::mat4 &mvp, const glm::vec2 &centerFull, const glm::vec2 &uvScale,
                            const glm::vec2 &uvOffset, const Config &config);

        /// Pass 2c: the edge ring. Re-shades the ring at FULL resolution with
        /// @c mNeonRingShader, which takes the gather's result from
        /// @c mGatherBuffer instead of running the gather - so the filament,
        /// the cut and the cutoffs near the line come out as the direct path
        /// draws them, while the expensive loop ran once, coarsely. Reads it
        /// through @p gatherUVScale / @p gatherUVOffset, the gather region's
        /// map from full-res rect-local px. Draws @c mRingVertexArray, which
        /// shares its edges with what pass 2b drew, through the same @p mvp.
        /// @pre Premultiplied-over blending; the caller's framebuffer and
        ///      full-resolution viewport are restored; pass 1 succeeded.
        void renderRingPass(const glm::mat4 &mvp, const glm::vec2 &gatherUVScale,
                            const glm::vec2 &gatherUVOffset, float time, const Config &config);

    private:
        /// Bits of @c mFailedPrograms, one per lazily built program.
        static constexpr unsigned int PROGRAM_PLAIN = 1u << 0;
        static constexpr unsigned int PROGRAM_GATHER = 1u << 1;
        static constexpr unsigned int PROGRAM_SHADE = 1u << 2;
        static constexpr unsigned int PROGRAM_BLIT = 1u << 3;
        static constexpr unsigned int PROGRAM_RING = 1u << 4;
        static constexpr unsigned int PROGRAM_GLOW_COVER = 1u << 5;

        Config mCurrentConfig;
        ShaderProgram mNeonShader;                                     ///< neon.frag: gather and shade, direct path. Built on first draw.
        ShaderProgram mNeonGatherShader;                               ///< neon-gather.frag: the scaled path's gather pass. Built on first draw.
        ShaderProgram mNeonShadeShader;                                ///< neon.frag + NEON_READS_GATHER: the scaled path's pass 1. Built on first draw.
        ShaderProgram mNeonRingShader;                                 ///< The same source, its own program: full-res shading from the gather - the edge ring below 1.0, pass 1 at 1.0 decoupled. See ensurePathPrograms.
        ShaderProgram mEmissionShader;                                 ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mGlowCoverShader;                                ///< Glow coverage pre-pass (neon-glow-cover.frag).
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mBlitShader;                                     ///< Scaled-path upscale composite (neon-blit.frag). Built on first draw.
        VertexArray mGlowVertexArray{"NeonRenderer.Glow"};             ///< Tight glow quad (rect + glow reach), in scaled space.
        VertexArray mFullscreenVertexArray{"NeonRenderer.Fullscreen"}; ///< NDC quad: emission bake, and the ALL-mode opaque fill when a clear cannot stand in.
        VertexArray mFillVertexArray{"NeonRenderer.Fill"};             ///< Opaque-fill band ring (rect +- the fill's cutoffs), in FULL-RES rect-local px.
        VertexArray mRingVertexArray{"NeonRenderer.Ring"};             ///< Scaled path: the edge ring's annulus, FULL-RES rect-local px.
        VertexArray mBlitVertexArray{"NeonRenderer.BlitArea"};         ///< Scaled path: the lit area outside the ring, same space.
        VertexArray mGatherVertexArray{"NeonRenderer.GatherArea"};     ///< The gather pass's quad - pass 1's and the ring's, padded - in SCALED rect-local px. Below 1.0, and at 1.0 decoupled.
        /// Vertices in @c mGlowVertexArray: 6 for the plain quad, 24 when the
        /// glow is bounded from the inside and @ref setupGeometry cuts a hole.
        /// See the note there for which settings do that.
        int mGlowVertexCount = 6;
        /// Vertex count in @c mFillVertexArray - 24 for a ring (8 triangles),
        /// 0 when there is no ring and the fullscreen quad is used instead.
        /// Written by @ref setupFillGeometry, read by @ref renderOpaqueFill,
        /// and doubles as the "is it built" flag so the two cannot disagree.
        int mFillVertexCount = 0;
        int mRingVertexCount = 0;     ///< 0 at scale 1.0; see setupRingGeometry.
        int mBlitVertexCount = 0;     ///< 0 at scale 1.0, and whenever nothing outside the ring can be lit.
        int mGatherVertexCount = 0;   ///< 0 at scale 1.0 unless decoupledGather; see setupRingGeometry.
        glm::vec2 mGlowOuter{0.0f};   ///< Pass 1's quad, half-extents in FULL-RES px; setupGeometry.
        glm::vec2 mGlowHole{0.0f};    ///< Its hole, the same; 0 when it has none.
        glm::vec2 mGatherOuter{0.0f}; ///< The gather quad's half-extents, FULL-RES px; setupRingGeometry.
        glm::vec2 mScaledOuter{0.0f}; ///< What the blit reads of mScaledBuffer, the same; setupRingGeometry.

        /// Set at the end of @ref Initialize. Gates the rebuilds in
        /// @ref OnConfigChanged, which can run before it; the neon.frag programs
        /// cannot be that gate any more, since they are built on first draw.
        bool mInitialized = false;
        /// @c PROGRAM_* bits of the lazily built programs that failed - see
        /// @ref buildNeonProgram.
        unsigned int mFailedPrograms = 0;

        /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
        /// block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonRenderer.SegmentBlock"};
        /// Backs neon.frag's std140 `LoopSamplesBlock` - vec4[NUM_LOOP_SAMPLES]
        /// where .xy holds the perimeter point in scaled rect-local pixels.
        /// Always allocated at full size; only the first @c uNumSamples entries
        /// are filled, and the shader stops there.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};
        /// Backs neon.frag's std140 `ArcBlock` (uArcCount + uArcs[MAX_ARCS]).
        UniformBuffer mArcBlock{"NeonRenderer.ArcBlock"};

        float mQuadMargin = 0.0f;     ///< Draw-quad margin (scaled px from rect edge); shader fades the bloom out by here.
        float mRingQuadMargin = 0.0f; ///< The same margin at scale 1.0, in full-res px - what the edge ring fades against.

        /// Baked colour ring (@c NeonConfig::gradientLutSize x 1 RGBA8, sampled
        /// at v = 0.5). The wrapper owns the bake, the cross-fade and the guard
        /// behind them - see @ref GradientRingLUT.
        GradientRingLUT mGradientLUT;

        /// Per-segment gradient atlas (SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS),
        /// one row per segment. The wrapper owns the bake, the dirty check and
        /// the snapshot behind it - see @ref SpanAtlasLUT.
        SpanAtlasLUT<SegmentBoost> mSegmentLUT;
        /// Reusable scratch for the merged transient+preserved segment list
        /// (Config::FillEffectiveSegments). Held as a member so the per-frame
        /// UBO pack / dirty check do no heap allocation after warmup.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Per-arc gradient atlas (ARC_LUT_WIDTH x MAX_ARCS), one row per arc.
        /// Same shape and purpose as mSegmentLUT.
        SpanAtlasLUT<Arc> mArcLUT;

        /// Perimeter emission table, NEON_MAX_LOOP_SAMPLES x 2 (see
        /// neon-emission.frag for the row packing). Written by
        /// @ref renderEmissionPass and read by the gather with texelFetch.
        /// Rebuilt only when @ref isEmissionTableStale says its inputs moved -
        /// the buffer is allocated once and nothing else writes it, so its
        /// contents survive between frames.
        ///
        /// Also remembers, on the renderer's behalf, whether the driver would
        /// give it RGBA16F: @ref renderEmissionPass asks a live buffer for the
        /// format it already holds, so the RGBA8 fallback sticks without a flag
        /// here to say so.
        Framebuffer mEmissionBuffer{"NeonRenderer.Emission"};

        /// Glow coverage table, GLOW_COVER_WIDTH wide by four bands of
        /// GLOW_COVER_ROWS, each holding one straight and one corner, which
        /// share its columns in proportion to their lengths (GetGlowCoverSplit):
        /// for each piece of the emitter and each fragment position round it,
        /// how lit the arcs and the segments make that piece as each layer sees
        /// it - see neon-glow-cover.frag, laid out in neon-pieces.glsl. Written by @ref renderGlowCoverPass, read by every
        /// neon.frag program with one filtered fetch per piece. Allocated on
        /// the first frame that bakes it (@ref ensureGlowCoverBuffer), never
        /// for a ring lit uniformly, and released when the layer is disabled;
        /// its lengths are ratios that scale together, so both resolution
        /// paths share it.
        Framebuffer mGlowCoverBuffer{"NeonRenderer.GlowCover"};

        /// Index into the glow coverage table's format list of the best format
        /// the driver has not refused. Only ever advances - see
        /// @ref ensureGlowCoverBuffer.
        size_t mGlowCoverFormat = 0;

        /// Pass 1's target on the scaled path: the composited colour, one RGBA8
        /// attachment at the reduced scale, covering what the blit reads
        /// (@c mScaledOuter) and never more than the whole reduced viewport -
        /// see GetBufferRegion. Never touched at @c resolutionScale 1.0 - not
        /// allocated, not bound, not blitted - so the full-res path pays
        /// nothing for its existence.
        Framebuffer mScaledBuffer{"NeonRenderer.Scaled"};

        /// The gather pass's target - on the scaled path, and at 1.0 under
        /// NeonConfig::decoupledGather - at GetGatherScale, coarser than
        /// @c mScaledBuffer for any rect much bigger than a thumbnail, over the
        /// gather quad's box (@c mGatherOuter) clipped to the viewport: the
        /// gathered colour and arc coverage, plus a second attachment for the
        /// segments' only when there are segments, RGBA16F where the driver
        /// renders to it and RGBA8 where not. Read by pass 1 and the edge ring;
        /// released whenever no frame gathers apart (UsesGatherBuffer).
        Framebuffer mGatherBuffer{"NeonRenderer.Gather"};

        /// Index into the gather buffer's format list of the best format the
        /// driver has not refused. Only ever advances - see
        /// @ref resizeGatherBuffer.
        size_t mGatherFormat = 0;

        /// Everything but time that can invalidate @c mEmissionBuffer.
        ///
        /// Set by @ref OnConfigChanged on ANY config change - deliberately not
        /// a narrow gate, because the table reads a wide slice of the config
        /// (hueRotationRate, numSamples, all three LUTs, and both light UBOs),
        /// and a missed field here is a silently stale ring rather than a
        /// rebuild that costs one small pass.
        ///
        /// Also set from @ref Update when @c GradientRingLUT::Tick re-uploads
        /// mid-cross-fade: the ring texture moves there with no config change
        /// to announce it.
        ///
        /// Starts true - the buffer holds undefined texels until the first
        /// bake, and no config change is guaranteed before the first frame.
        bool mEmissionDirty = true;
        /// The @c time @ref renderEmissionPass last baked at. Only meaningful
        /// while @c hueRotationRate is non-zero; at 0 the table does not
        /// depend on time and this is not consulted.
        float mEmissionTime = 0.0f;

        /// Whether @c mGlowCoverBuffer has to be re-baked. Unlike
        /// @c mEmissionDirty this is NOT set on every config change: the bake
        /// reads the two light blocks (the arcs and the effective segments),
        /// the rect's width, height, corner radius and winding, and the glow
        /// radius - every one of them in @ref renderGlowCoverPass or the
        /// blocks it binds - so @ref OnConfigChanged gates it on exactly
        /// those, as it gates @c mLightBlocksDirty. The bake is the costly
        /// pass (0.14-0.27 ms on an AMD Radeon Pro 5300M), and "any change"
        /// re-ran it every frame under an intensity or colour animation, or an
        /// animation of another LAYER's fields, none of which it reads.
        ///
        /// Accumulated, never assigned, for @c mLightBlocksDirty's reasons, and
        /// cleared only by the bake - so a change made while the bake is
        /// skipped (a ring lit uniformly) is still pending when it next runs.
        /// Also set by @ref ensureGlowCoverBuffer on a fresh allocation. Never
        /// depends on time. Starts true - the buffer holds undefined texels
        /// until the first bake.
        bool mGlowCoverDirty = true;

        /// Whether @c mSegmentBlock / @c mArcBlock still hold the current
        /// config. Cleared by @ref packLightBlocks once it has repacked.
        ///
        /// Unlike @c mEmissionDirty this is NOT set on every config change:
        /// the blocks are packed from @c mEffectiveSegments and
        /// @c NeonConfig::arcs and nothing else, so @ref OnConfigChanged gates
        /// it on exactly those two. It accumulates rather than being assigned,
        /// because that call can run more than once before the next
        /// @ref Render - see the note there.
        ///
        /// Starts true because @ref Initialize does not pack - the first
        /// @ref Render is what fills them.
        bool mLightBlocksDirty = true;
    };
}

#endif
