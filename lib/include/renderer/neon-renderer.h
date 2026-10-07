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
#include <cstdint>
#include <vector>

namespace EdgeLighting
{
    /// The neon renderer.
    ///
    /// Draws a tight quad over the rect + glow-reach margin and runs one
    /// fragment shader (neon.frag) that composes filament + halo + bloom from
    /// an analytic rounded-box SDF and what the gather pass left in its
    /// buffer.
    ///
    /// The gather (neon-gather.frag) is a loop over
    /// @c NeonConfig::numSamples perimeter samples (positions live in a UBO),
    /// run at its own coarse scale, that costs two @c texelFetch calls per
    /// sample into @c uEmission - the table baked by the emission pre-pass -
    /// or ONE where the config carries no segments, since the pre-pass then
    /// writes row 1 as all zeros and the shader takes a second, shorter loop
    /// body. That branch is on a uniform and sits OUTSIDE the loop; see the
    /// note at the loop in neon-gather.frag for why inside was measured
    /// slower than not branching at all. The per-sample arc scan, segment
    /// loop and filtered LUT reads that used to run in it are all
    /// fragment-invariant and moved to @c neon-emission.frag, which is why its
    /// cost no longer scales with the arc or segment count. See
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
    /// ONE path at every @c NeonConfig::resolutionScale, 1.0 included (I57;
    /// there was a direct path at 1.0 that shaded straight onto the target,
    /// removed once no shipped config used 1.0). The gather runs ALONE
    /// (neon-gather.frag) into @c mGatherBuffer, at a scale set by its own
    /// smoothness rather than by @c resolutionScale - about 2 texels per
    /// colour kernel, so typically far coarser (@ref GetGatherScale). Then
    /// neon.frag, which reads that result back, shades @c mScaledBuffer at the
    /// requested fraction - full size at 1.0 - which is bilinear-blitted back
    /// over the area that can still be lit outside a thin ring around the rect
    /// edge, and the ring is re-shaded at full resolution. The loop - ~95% of
    /// the neon's cost when every pixel ran it - runs on a few thousand texels,
    /// while the filament, the cut and the cutoffs near the line come out at
    /// full resolution. Both buffers cover only the part of the frame their
    /// readers reach (@ref GetBufferRegion). See
    /// docs/neon-resolution-scale-plan.md sections 12 and 13.
    ///
    /// With no segments and a colour-stop alpha time cannot move, the shading
    /// is factored: neon.frag's output is tonemap(col * Fa) with only the
    /// gathered hue col changing, so pass 1f bakes Fa into @c mFieldBuffer
    /// once the config has held for a frame, and pass 1c (neon-field.frag)
    /// composites it with the hue in pass 1b's place, into @c mScaledBuffer -
    /// under a rotating hue, the frames that would otherwise redraw pass 1b
    /// with only the time moved, 1.1-2.0x on them, and under an intensity
    /// animation, scaled (I56). The ring has a field of its own (pass 1r / 2r),
    /// still frames included. A frame whose config just changed shades
    /// directly. See docs/neon-perf-plan.md section 11, and I46, I50, I52 and
    /// I56 in docs/review-findings.md.
    ///
    /// Every pixel-valued uniform is multiplied by the scale (a no-op at 1.0)
    /// and the shader converts its own full-res px constants with
    /// @c uResolutionScale; the ring takes them at 1.0, and @c uBlitOwnsCut
    /// says which passes leave the cut and the cutoffs to the blit.
    ///
    /// The neon programs and the blit are built on the first frame
    /// (@ref ensurePathPrograms): the gather, the shading twice - one object
    /// per target - and the blit. The fields' three programs are built on the
    /// first frame a config they can serve is drawn (@ref ensureFieldPrograms,
    /// @ref ensureRingFieldPrograms), and the glow coverage bake on the first
    /// frame that bakes the table (@ref ensureGlowCoverProgram).
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
        /// Everything pass 0 reads besides the time, AS THE PASS SEES IT: its
        /// two value uniforms, and a version number for each texture and
        /// uniform block it binds. See @ref isEmissionTableStale.
        typedef struct EmissionInputs
        {
            float hueRotationRate = 0.0f;     ///< uHueRotationRate.
            int numSamples = 0;               ///< uNumSamples, clamped as the pass uploads it.
            uint32_t gradientUploads = 0;     ///< mGradientLUT.GetUploadCount() - uGradientLUT.
            uint32_t segmentAtlasUploads = 0; ///< mSegmentLUT.GetUploadCount() - uSegmentLUT.
            uint32_t arcAtlasUploads = 0;     ///< mArcLUT.GetUploadCount() - uArcLUT.
            uint32_t segmentBlockUploads = 0; ///< mSegmentBlock.GetUploadCount() - SegmentBlock.
            uint32_t arcBlockUploads = 0;     ///< mArcBlock.GetUploadCount() - ArcBlock.

            bool operator==(const EmissionInputs &o) const
            {
                return hueRotationRate == o.hueRotationRate && numSamples == o.numSamples &&
                       gradientUploads == o.gradientUploads && segmentAtlasUploads == o.segmentAtlasUploads &&
                       arcAtlasUploads == o.arcAtlasUploads && segmentBlockUploads == o.segmentBlockUploads &&
                       arcBlockUploads == o.arcBlockUploads;
            }
            bool operator!=(const EmissionInputs &o) const { return !(*this == o); }
        } EmissionInputs;

        /// Everything pass 1a reads besides the region it draws and the quad
        /// it draws over: what neon-gather.frag is handed
        /// (@ref uploadShapeUniforms, @ref bindGatherInputs) and the buffer it
        /// draws into. The table and the sample block by version, as
        /// @ref EmissionInputs keys them. See @ref currentGatherInputs.
        typedef struct GatherInputs
        {
            uint32_t emissionBakes = 0; ///< mEmissionBakes - what uEmission holds.
            uint32_t sampleUploads = 0; ///< mLoopSamplesBlock.GetUploadCount() - LoopSamplesBlock.
            int numSamples = 0;         ///< uNumSamples, clamped as the pass uploads it.
            float scale = 0.0f;         ///< The resolution scale - the space of the shape and the projection.
            glm::vec2 rectSize{0.0f};   ///< uRectSize.
            float cornerRadius = 0.0f;  ///< uCornerRadius.
            int attachments = 0;        ///< 2 with segments (uSegmentCount > 0 picks the loop body), else 1.
            glm::ivec2 viewport{0};     ///< The viewport the region's texel grid is anchored to, and clipped to.
            glm::vec2 center{0.0f};     ///< The rect's centre in it, full-res px, y up - where that grid and clip fall on the rect.

            bool operator==(const GatherInputs &o) const
            {
                return emissionBakes == o.emissionBakes && sampleUploads == o.sampleUploads &&
                       numSamples == o.numSamples && scale == o.scale && rectSize == o.rectSize &&
                       cornerRadius == o.cornerRadius && attachments == o.attachments && viewport == o.viewport &&
                       center == o.center;
            }
            bool operator!=(const GatherInputs &o) const { return !(*this == o); }
        } GatherInputs;

        /// Build the programs both paths use - the emission pre-pass and the
        /// opaque fill. The rest are built on the first frame; see @ref ensurePathPrograms -
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
        /// Make sure every program the glow draws with is built, building any
        /// that are not: neon-gather.frag, neon.frag twice (pass 1b and the
        /// ring - one program object per target) and the blit. Lazy rather
        /// than at @ref Initialize, so a host that never enables the glow
        /// never compiles them. False if any of them failed, in which case the
        /// glow is skipped and the frame degrades to the fill.
        bool ensurePathPrograms();
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
        /// Build the two FULL-RES partition arrays, from one set
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
        /// It also builds the gather pass's quad
        /// (@c mGatherVertexArray) and the two region boxes, @c mGatherOuter and
        /// @c mScaledOuter. Reads @c mQuadMargin, so it runs after
        /// @ref setupGeometry.
        void setupRingGeometry(const Config &config);
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
        /// RGBA16F, then RGBA8. Called from @ref renderGatherPass on every
        /// frame that runs pass 1a, at every scale; @c Framebuffer::Resize
        /// early-outs when nothing changed. The format reached is recorded in
        /// @c mGatherFormat, not in the buffer, because the buffer is released
        /// when the layer is disabled.
        /// @return false only if no candidate could be allocated.
        bool resizeGatherBuffer(int width, int height, int attachments);

        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // The numbering is the pipeline order from docs/emission-prepass.md,
        // and the .cpp defines them in this same order - keep all three in
        // step. The data dependency is the real contract: pass 0 bakes the
        // table the gather reads, pass 1a's gather buffer is what pass 1b and
        // pass 2c shade from, pass 2a's fill must land before pass 2b
        // composites the glow over it. 2b and 2c cover disjoint areas, so
        // their order between themselves is free. Every offscreen pass (0, 0b,
        // 1a, 1b, and the fields' bakes) runs first, then everything on the
        // caller's framebuffer, so that target is drawn in one unbroken run.

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

        /// Whether @c mEmissionBuffer's contents still describe this frame,
        /// i.e. whether pass 0 has to run at all.
        ///
        /// The table is a pure function of what @ref renderEmissionPass hands
        /// neon-emission.frag - the same invariant the pre-pass itself rests on
        /// - and the buffer is allocated once for the renderer's lifetime and
        /// written by nothing else, so a frame whose inputs all match the last
        /// bake's can read what is already in it. The pre-pass hoists the
        /// gather's fragment-invariant half out of every FRAGMENT; this is what
        /// hoists it out of every FRAME as well.
        ///
        /// Stale when any of three things holds:
        ///   - the table was never baked (@c mEmissionBaked);
        ///   - an input other than time moved (@ref EmissionInputs): the two
        ///     value uniforms compared by value, and the three LUTs and two
        ///     light blocks by their upload counts. Version numbers on the GL
        ///     objects the pass binds, NOT a list of the config fields that
        ///     feed them - so a config change that moves none of them (an
        ///     intensity, bloom, glow, geometry or other-layer animation)
        ///     re-bakes nothing, and nothing that does move one can be missed
        ///     by a gate falling out of step with the shader. A cross-fade
        ///     frame moves the ring's count like a re-bake does.
        ///   - @c uTime moved at a non-zero hue rate. It reaches the shader
        ///     exactly once, as `si - uTime * uHueRotationRate`, so at a rate
        ///     of 0 it drops out of the table altogether.
        ///
        /// It used to be "any config change" (@c mEmissionDirty), deliberately
        /// wide because a field list would drift from the shader; the version
        /// numbers have no field list. See docs/emission-prepass.md section 3.
        /// @pre @ref packLightBlocks has run this frame: the blocks' counts
        ///      are part of the key, and only the pack moves them.
        bool isEmissionTableStale(float time, const Config &config) const;

        /// The pass-0 inputs other than time, as @ref renderEmissionPass would
        /// upload and bind them now.
        EmissionInputs currentEmissionInputs(const Config &config) const;

        /// Pass 1a's inputs besides its region and quad, as
        /// @ref renderGatherPass would hand them over now, for a viewport of
        /// @p viewportWidth x @p viewportHeight.
        /// @pre Pass 0 has run this frame if it is going to: the table's
        ///      version is part of the key.
        GatherInputs currentGatherInputs(const Config &config, int viewportWidth, int viewportHeight) const;

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer, at the clamped sample count so texel i here is
        /// sample i in the gather. Retargets the framebuffer and viewport, so
        /// it restores both before returning - see docs/emission-prepass.md.
        ///
        /// Records what it baked (@c mEmissionInputs, @c mEmissionTime,
        /// @c mEmissionBaked) on the way out, so @ref isEmissionTableStale
        /// reads a snapshot written by the only thing that ever writes the
        /// buffer.
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
        ///
        /// Two channels (RG16F, 0.5 MB) when the config has no segments
        /// (@p segments false), whose coverage would fill .b / .a with zeros;
        /// four (RGBA16F, 1 MB) when it has - reallocating a two-channel table
        /// the first frame segments appear, and keeping four after they go,
        /// so a transient segment does not reallocate it every time.
        /// @return false only if NO candidate could be allocated, in which case
        ///         the frame draws the fill alone, as for a failed program.
        bool ensureGlowCoverBuffer(bool segments);

        /// Pass 0b: bake the glow coverage table into @c mGlowCoverBuffer -
        /// for each piece of the emitter, the arcs' and the segments' coverage
        /// over that piece's own extent, weighted by the halo's and the bloom's
        /// kernel about a fragment's foot on it (neon-glow-cover.frag, V20 and
        /// V21 in docs/review-findings.md).
        /// Retargets the framebuffer and viewport, so it restores both before
        /// returning, and clears @c mGlowCoverDirty on the way out. Bakes only
        /// the pieces @c mGlowCoverDirty names, by drawing its quad onto their
        /// rectangles of the table - all of it in one draw when all are dirty.
        /// @pre Blending disabled, and the light blocks packed this frame.
        void renderGlowCoverPass(const Config &config);

        /// The transform and the rect's shape - uMVP, uRectSize,
        /// uCornerRadius - for @p shader at @p scale: everything the gather
        /// pass (neon-gather.frag) reads besides its own inputs, and the first
        /// thing @ref uploadNeonUniforms uploads for every other program.
        /// @pre @p shader is in use.
        void uploadShapeUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale, const Config &config);

        /// Every SHADING uniform the neon programs read, for @p shader at
        /// @p scale, fading against @p quadMargin and skipping corner arcs past
        /// @p cornerSkip (px in the same space, both). One
        /// copy for every program built from neon.frag, so the variants cannot
        /// drift apart in what they are told. Not for neon-gather.frag, which
        /// shades nothing - it takes @ref uploadShapeUniforms alone. The
        /// gather's own inputs - the sample block, the count and the emission
        /// table - are not here: only the gathering programs have them, and
        /// @ref bindGatherInputs binds them. @p blitOwnsCut is true for a pass
        /// the blit composites (pass 1b and its field bake), which leaves the
        /// one-sided cut and the cutoffs to it, false for the edge ring and its
        /// field bake, which apply both after the grade (uBlitOwnsCut).
        /// @pre @p shader is in use.
        void uploadNeonUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale, float time,
                                float quadMargin, float cornerSkip, bool blitOwnsCut, const Config &config);

        /// Bind the gather loop's own inputs - the sample block, the count and
        /// the emission table - for @p shader, which is in use. Only the
        /// gather pass runs the loop, so only it has them.
        void bindGatherInputs(ShaderProgram &shader, const Config &config);

        /// Bind @c mGatherBuffer's attachments for @p shader (a neon.frag
        /// program, in use), with the affine map from the
        /// pass's vPos onto the buffer's uv.
        void bindGatherBuffer(ShaderProgram &shader, const glm::vec2 &uvScale, const glm::vec2 &uvOffset);

        /// Pass 1a, both paths: the gather, into @c mGatherBuffer
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

        /// Size @c mScaledBuffer to @p bufWidth x @p bufHeight texels, bind it
        /// and clear it - pass 1b's target, whether the shading draws it
        /// (@ref renderNeonPass) or the field's composite does
        /// (@ref renderFieldCompositePass). The caller lifts the host's
        /// scissor first and keeps it lifted through its draw (see
        /// renderNeonPass).
        /// @return false if the buffer could not be allocated, in which case
        ///         nothing was bound and the caller must draw nothing.
        bool bindScaledTarget(int bufWidth, int bufHeight);

        /// Pass 1b: the neon on the tight glow quad, shaded from the gather
        /// pass's result, so it must run after it - into @c mScaledBuffer,
        /// @p bufWidth x @p bufHeight texels drawn through @p mvp (its region's
        /// projection) with @c mNeonShadeShader, reading @c mGatherBuffer
        /// through the uv map @p gatherUVScale / @p gatherUVOffset. Clears that
        /// buffer and leaves it bound; @ref Render restores the target before
        /// pass 2a.
        /// @pre Blending DISABLED: the buffer was just cleared and the quad
        ///      covers each texel once, so over would only have added zero, for
        ///      a destination read per texel.
        /// @return false if the reduced target could not be allocated, in
        ///         which case nothing was drawn and passes 2b and 2c must be
        ///         skipped too - they would otherwise composite a stale or
        ///         undefined buffer.
        bool renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight, const glm::vec2 &gatherUVScale,
                            const glm::vec2 &gatherUVOffset, float time, const Config &config);

        // --- The shading, factored: the hue-invariant field ----------------
        // On a config with no segments whose colour-stop alpha cannot move
        // with time, pass 1b's output is tonemap(col * Fa), with only the
        // gathered hue col changing from frame to frame (neon-field.frag) - the
        // blit applies the cut and the cutoffs after it. So the shading splits
        // in two: 1f bakes Fa once the config has held for a frame, offscreen,
        // after the gather; 1c composites it with the gathered hue in pass 1b's
        // place, into the reduced buffer, under a rotating hue (1.1-2.0x on
        // those frames on an AMD Radeon Pro 5300M, I50) and an intensity
        // animation (I56).

        /// When a field is baked and when it is read - the schedule the
        /// shading's field (@c mField) and the edge ring's (@c mRingField)
        /// share. A field outlives the frame it was baked on; a config change
        /// (@ref OnConfigChanged calls @ref Invalidate), the viewport and a
        /// gradient ring upload - a cross-fade frame - invalidate it.
        ///
        /// The bake is LAZY: only a config that has held for a frame is baked
        /// (@c settled), so an animation that changes the config every frame
        /// keeps shading directly and never pays a bake it cannot reuse - a
        /// bake AND a composite on every frame measured 5-7% slower than
        /// shading directly.
        ///
        /// Except on the FIRST frame a renderer draws a config the field can
        /// serve, which builds the field's programs and bakes it at once,
        /// while still shading directly. Building alone is not enough: this
        /// driver finishes a program's compile on its first DRAW, in the
        /// target and blend state it draws with, so a bake program only linked
        /// there left a ~125 ms stall on the bake frame after it (~155 ms all
        /// told, AMD Radeon Pro 5300M). And that frame still shades directly
        /// so that the shading program's first draw lands there too -
        /// compositing instead deferred THAT stall to the first config change.
        /// The price is one bake an animated config may not reuse, once per
        /// renderer.
        ///
        /// The neon's INTENSITY is the one config change that does not
        /// invalidate a field (I56): neon.frag's output is near enough
        /// I * G with G fixed while only the intensity moves, so a field baked
        /// at intensity I0 serves intensity I as Fa * I / I0 (@c Decision::gain).
        /// Near enough, not exactly: the bloom's reach and the quad's fade and
        /// hole also follow the intensity, and the field holds them as they
        /// were at I0 - up to 2 levels on the default look across
        /// IntensityPulse's 0.4-1.0, 6 on a 1.5 bloom (docs/neon-reduced-scale-
        /// plan.md, D1). So only while the intensity is MOVING, and only below
        /// I0, whose quad covers every dimmer one's; a brighter frame bakes
        /// again at its own intensity, so a pulse settles on its peak after
        /// one cycle; and the frame the intensity stops on bakes again too,
        /// exactly - a held config never shows a scaled field.
        typedef struct LazyBake
        {
            /// What @ref Decide says a frame does with the field.
            typedef struct Decision
            {
                bool use = false;  ///< Composite the field in the shading's place.
                bool bake = false; ///< Bake it first, after the gather it reads.
                float gain = 1.0f; ///< What the composite multiplies the field by: I / I0, exactly 1 unless scaled.
            } Decision;

            bool current = false;         ///< The buffer describes this frame up to its intensity: nothing else invalidated it since its bake.
            bool settled = false;         ///< Nothing invalidated it on the last frame, so a bake now would be reused.
            bool unavailable = false;     ///< Not on this driver (a program failed, or no half-float target): shade directly.
            glm::ivec2 viewport{0};       ///< The viewport it was decided for.
            uint32_t gradientUploads = 0; ///< The gradient ring's upload count it was decided for.
            float intensity = 0.0f;       ///< NeonConfig::intensity it was baked at (I0).

            /// The schedule's step for a frame whose config the field can
            /// serve, drawn at @p frameViewport with the gradient ring at
            /// @p frameGradientUploads and the neon at @p frameIntensity, which
            /// a config change moved since the last frame if
            /// @p intensityMoving; @p firstFrame when the field's programs were
            /// not built before this frame.
            Decision Decide(const glm::ivec2 &frameViewport, uint32_t frameGradientUploads, bool firstFrame,
                            float frameIntensity, bool intensityMoving);

            /// The config changed: what the buffer holds is stale, and the
            /// next frame is not settled.
            void Invalidate()
            {
                current = false;
                settled = false;
            }

            /// A bake ran, at @p bakedIntensity: what the buffer holds is
            /// current if it @p baked, and the field is never used again on
            /// this driver if not.
            void OnBaked(bool baked, float bakedIntensity)
            {
                current = baked;
                unavailable = !baked;
                intensity = bakedIntensity;
            }
        } LazyBake;

        /// Make sure the field's two programs - neon.frag with NEON_FIELD_BAKE,
        /// and neon-field.frag - are built, building them if not. Called on
        /// every frame whose config the field can serve, so they are built on
        /// the first such frame - which shades directly, and is compiling its
        /// path's programs too - rather than stalling the bake frame after it.
        /// A failure is recorded in @c mFailedPrograms; the field is then never
        /// used and the shading draws as before.
        bool ensureFieldPrograms();

        /// Pass 1f: bake the field into @c mFieldBuffer, @p width x @p height
        /// texels, drawn through @p mvp - the projection of the reduced buffer's
        /// region at @p scale, which pass 1c reads it over - reading the gather
        /// through the map @p gatherUVScale / @p gatherUVOffset pass 1b reads
        /// it through. R16F, Fa alone. Leaves the buffer bound;
        /// @ref Render restores the target.
        /// @pre Blending disabled; pass 1a has run or its buffer is current.
        /// @return false if the buffer could not be allocated - the field is
        ///         then never used.
        bool renderFieldPass(const glm::mat4 &mvp, int width, int height, const glm::vec2 &gatherUVScale,
                             const glm::vec2 &gatherUVOffset, float scale, float time, const Config &config);

        /// Pass 1c: in pass 1b's place, the glow quad drawn with
        /// neon-field.frag - the field times the gathered hue, tone-mapped -
        /// into pass 1b's target, @c mScaledBuffer at @p bufWidth x
        /// @p bufHeight, sized, bound and cleared here. The same arguments as
        /// @ref renderNeonPass; the field is multiplied by @p gain
        /// (@c LazyBake::Decision::gain).
        /// @pre Blending disabled.
        /// @return false if the reduced buffer could not be allocated.
        bool renderFieldCompositePass(const glm::mat4 &mvp, int bufWidth, int bufHeight,
                                      const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset, float gain);

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

        /// Pass 2b: bilinear composite of the reduced buffer onto the caller's
        /// framebuffer, AND the one-sided glow cut and the inside/outside
        /// cutoffs, at full resolution, for what pass 1b drew (the ring applies
        /// its own). Covers what can still be lit outside
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
        /// the cut and the cutoffs near the line come out at full resolution,
        /// while the expensive loop ran once, coarsely. Reads it
        /// through @p gatherUVScale / @p gatherUVOffset, the gather region's
        /// map from full-res rect-local px. Draws @c mRingVertexArray, which
        /// shares its edges with what pass 2b drew, through the same @p mvp.
        /// @pre Premultiplied-over blending; the caller's framebuffer and
        ///      full-resolution viewport are restored; pass 1 succeeded.
        void renderRingPass(const glm::mat4 &mvp, const glm::vec2 &gatherUVScale,
                            const glm::vec2 &gatherUVOffset, float time, const Config &config);

        // --- Pass 2c, factored: the edge ring's field ----------------------
        // The ring is re-shaded on EVERY frame, still frames included: it draws onto the caller's framebuffer, so there is
        // nothing to reuse. With no segments its output is
        // mask * tonemap(col * Fa) - the mask being the one-sided cut and the
        // cutoffs, which the ring applies after its tone map at full
        // resolution, and a function of the pixel's position alone - so it
        // splits like pass 1b's: pass 1r bakes Fa over the ring alone, at full
        // resolution and packed into its four strips (@ref RingFieldLayout),
        // once the config has held for a frame; pass 2r composites it with the
        // gathered hue in pass 2c's place, and multiplies the mask back in.
        // 1.4-1.9x on still frames and 1.2-1.6x on hue frames on an AMD Radeon
        // Pro 5300M, within 1/255 (I52); the mask since I53.

        /// How the ring's pixels are packed into @c mRingFieldBuffer. The ring
        /// is an axis-aligned annulus (setupRingGeometry's PushAnnulus), so a
        /// box of pixels minus a hole: its bottom and top strips are stored as
        /// they are, one above the other, and its left and right strips
        /// TRANSPOSED above those, so each runs along the atlas's width. Every
        /// texel sits on one of the viewport's pixels, so the composite reads
        /// the texel of its own pixel.
        /// neon-field.frag (NEON_FIELD_RING) reads this layout and
        /// @ref renderRingFieldPass bakes it: change the three together.
        typedef struct RingFieldLayout
        {
            glm::vec2 origin{0.0f};  ///< Rect-local full-res px of the box's lower-left pixel corner.
            glm::ivec2 size{0};      ///< The box, in px: the ring's outer box, clipped to the viewport.
            glm::ivec4 hole{0};      ///< Box px the ring never draws, [x0, x1) x [y0, y1) as x0, y0, x1, y1.
            glm::ivec2 rows{0};      ///< First atlas row of the left strip and of the right strip.
            glm::ivec2 atlas{0};     ///< The buffer's size, in texels.
        } RingFieldLayout;

        /// The packing for a ring of outer box @p ringOuter and hole
        /// @p ringHole (half-extents, full-res px) centred at @p centerFull in
        /// a viewport of @p viewportWidth x @p viewportHeight. The hole is
        /// CONSERVATIVE - only pixels whose centre lies more than a pixel
        /// inside it - so every pixel the ring can draw falls in a strip.
        static RingFieldLayout computeRingFieldLayout(const glm::vec2 &ringOuter, const glm::vec2 &ringHole,
                                                      const glm::vec2 &centerFull, int viewportWidth,
                                                      int viewportHeight);

        /// Make sure the ring field's composite (neon-field.frag with
        /// NEON_FIELD_RING) and the bake (@ref ensureFieldPrograms) are built.
        /// A failure is recorded in @c mFailedPrograms; the ring is then shaded
        /// directly.
        bool ensureRingFieldPrograms();

        /// Pass 1r: bake the ring's field into @c mRingFieldBuffer, laid out
        /// as @c mRingFieldLayout - the ring's shading uploaded exactly as
        /// @ref renderRingPass uploads it, with the hue at 1, drawn once per
        /// strip through that strip's atlas rows as the viewport and a
        /// projection onto them (transposed for the two side strips). Reads
        /// the gather through the full-res map @p gatherUVScale /
        /// @p gatherUVOffset. Leaves the buffer bound; @ref Render restores the
        /// target.
        /// @pre Blending disabled; pass 1a has run or its buffer is current.
        /// @return false if the buffer could not be allocated.
        bool renderRingFieldPass(const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset, float time,
                                 const Config &config);

        /// Pass 2r: in pass 2c's place, @c mRingVertexArray drawn through
        /// @p mvp - the matrix the blit drew with, so the partition holds -
        /// with the ring field's composite: the field times @p gain times the
        /// gathered hue, tone-mapped, times the one-sided cut and the cutoffs,
        /// which it takes from @p config as the ring's own shading does.
        /// @pre Premultiplied-over blending; the caller's framebuffer and
        ///      full-resolution viewport are restored.
        void renderRingFieldCompositePass(const glm::mat4 &mvp, const glm::vec2 &gatherUVScale,
                                          const glm::vec2 &gatherUVOffset, float gain, const Config &config);

    private:
        /// Bits of @c mFailedPrograms, one per lazily built program.
        static constexpr unsigned int PROGRAM_GATHER = 1u << 1;
        static constexpr unsigned int PROGRAM_SHADE = 1u << 2;
        static constexpr unsigned int PROGRAM_BLIT = 1u << 3;
        static constexpr unsigned int PROGRAM_RING = 1u << 4;
        static constexpr unsigned int PROGRAM_GLOW_COVER = 1u << 5;
        static constexpr unsigned int PROGRAM_FIELD = 1u << 6;
        static constexpr unsigned int PROGRAM_FIELD_COMPOSITE = 1u << 7;
        static constexpr unsigned int PROGRAM_RING_FIELD_COMPOSITE = 1u << 8;

        Config mCurrentConfig;
        ShaderProgram mNeonGatherShader;                               ///< neon-gather.frag: the gather pass, both paths. Built on first draw.
        ShaderProgram mNeonShadeShader;                                ///< neon.frag: pass 1b, into the reduced buffer. Built on first draw.
        ShaderProgram mNeonRingShader;                                 ///< The same source, its own program: the edge ring. See ensurePathPrograms.
        ShaderProgram mEmissionShader;                                 ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mGlowCoverShader;                                ///< Glow coverage pre-pass (neon-glow-cover.frag).
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mBlitShader;                                     ///< Scaled-path upscale composite (neon-blit.frag). Built on first draw.
        ShaderProgram mNeonFieldShader;                                ///< neon.frag + NEON_FIELD_BAKE: pass 1f. Built on the first field-eligible frame.
        ShaderProgram mFieldCompositeShader;                           ///< neon-field.frag: pass 1c. Built with it.
        ShaderProgram mRingFieldCompositeShader;                       ///< neon-field.frag + NEON_FIELD_RING: pass 2r. Its own object - it draws the caller's framebuffer, blended, in the frame pass 1c draws the reduced buffer unblended. Built on the first ring-field-eligible frame.
        VertexArray mGlowVertexArray{"NeonRenderer.Glow"};             ///< Tight glow quad (rect + glow reach), in scaled space.
        VertexArray mFullscreenVertexArray{"NeonRenderer.Fullscreen"}; ///< NDC quad: emission bake, and the ALL-mode opaque fill when a clear cannot stand in.
        VertexArray mFillVertexArray{"NeonRenderer.Fill"};             ///< Opaque-fill band ring (rect +- the fill's cutoffs), in FULL-RES rect-local px.
        VertexArray mRingVertexArray{"NeonRenderer.Ring"};             ///< The edge ring's annulus, FULL-RES rect-local px.
        VertexArray mBlitVertexArray{"NeonRenderer.BlitArea"};         ///< The lit area outside the ring, same space.
        VertexArray mGatherVertexArray{"NeonRenderer.GatherArea"};     ///< Both paths: the gather pass's quad - pass 1's and the ring's, padded - in SCALED rect-local px.
        /// Vertices in @c mGlowVertexArray: 6 for the plain quad, 24 when the
        /// glow is bounded from the inside and @ref setupGeometry cuts a hole -
        /// by a cutoff, the one-sided cut, or the glow's own reach on a rect
        /// larger than twice it. See the note there.
        int mGlowVertexCount = 6;
        /// Vertex count in @c mFillVertexArray - 24 for a ring (8 triangles),
        /// 0 when there is no ring and the fullscreen quad is used instead.
        /// Written by @ref setupFillGeometry, read by @ref renderOpaqueFill,
        /// and doubles as the "is it built" flag so the two cannot disagree.
        int mFillVertexCount = 0;
        int mRingVertexCount = 0;     ///< See setupRingGeometry.
        int mBlitVertexCount = 0;     ///< 0 whenever nothing outside the ring can be lit.
        int mGatherVertexCount = 0;   ///< The gather pass's quad, both paths; see setupRingGeometry.
        glm::vec2 mGlowOuter{0.0f};   ///< Pass 1's quad, half-extents in FULL-RES px; setupGeometry.
        glm::vec2 mGlowHole{0.0f};    ///< Its hole, the same; 0 when it has none.
        glm::vec2 mGatherOuter{0.0f}; ///< The gather quad's half-extents, FULL-RES px; setupRingGeometry.
        glm::vec2 mGatherHole{0.0f};  ///< Its hole, the same; 0 on an axis where it has none.
        float mGlowArea = 0.0f;       ///< The glow quad's area, FULL-RES px, unclipped; setupGeometry. See FieldFillsRegion.
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
        float mRingQuadMargin = 0.0f; ///< The same margin at scale 1.0 with no guard band, in full-res px - what the edge ring fades against.
        float mCornerSkip = 0.0f;     ///< uCornerSkip for pass 1 (scaled px) - see GetCornerSkip.
        float mRingCornerSkip = 0.0f; ///< The same at scale 1.0, in full-res px, for the edge ring.
        glm::vec2 mRingOuter{0.0f};   ///< The edge ring's outer box, half-extents in full-res px; setupRingGeometry.
        glm::vec2 mRingHole{0.0f};    ///< Its hole, the same way.
        /// How deep inside the edge the glow can still write a non-zero pixel,
        /// FULL-RES px - the interior counterpart of @c mQuadMargin, which
        /// @ref setupGeometry cuts the quad's hole at and @ref setupRingGeometry
        /// bounds the blit with. See GetGlowInnerReach.
        float mGlowInnerReach = 0.0f;
        /// The emission bound mGlowInnerReach was solved for (GetGlowEmissionBound).
        /// It moves with the arcs' intensities and the segments' boosts, which
        /// are not otherwise geometry inputs - @ref OnConfigChanged compares
        /// against it rather than rebuilding on every arc or segment change.
        float mGlowEmission = 0.0f;

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
        /// for a ring lit uniformly, and released when the layer is disabled
        /// or once nothing has read it for GLOW_COVER_RELEASE_SECONDS
        /// (@ref Update); two channels while the config has no segments. Its
        /// lengths are ratios that scale together, so both resolution paths
        /// share it.
        Framebuffer mGlowCoverBuffer{"NeonRenderer.GlowCover"};

        /// Index into the glow coverage table's format list of the best format
        /// the driver has not refused. Only ever advances - see
        /// @ref ensureGlowCoverBuffer.
        size_t mGlowCoverFormat = 0;

        /// The hue-invariant field (pass 1f): Fa, R16F, on the reduced buffer's
        /// own region and grid, half that buffer's size (0.84 MB at 0.5 for a
        /// 960 x 540 rect at 1080p). Released with the conditions that want it
        /// - see OnConfigChanged - and never allocated for a config that cannot
        /// use it (segments, or a hue that does not rotate with the intensity
        /// still).
        Framebuffer mFieldBuffer{"NeonRenderer.Field"};
        /// When @c mFieldBuffer is baked and read - see @ref LazyBake.
        LazyBake mField;
        /// How pass 1c finds a fragment's texel of the field it was baked as -
        /// floor((vPos - origin) * texelScale), both in vPos's scaled
        /// rect-local px: the reduced buffer's origin and texels per scaled px.
        glm::vec2 mFieldOrigin{0.0f};
        glm::vec2 mFieldTexelScale{1.0f};

        /// The edge ring's field (pass 1r): Fa, R16F, at full resolution over
        /// the ring alone, packed as @c mRingFieldLayout. Only for a config
        /// @ref Render finds eligible (no segments, and a
        /// colour-stop alpha time cannot move). Released with the conditions
        /// that want it - see OnConfigChanged.
        Framebuffer mRingFieldBuffer{"NeonRenderer.RingField"};
        RingFieldLayout mRingFieldLayout{}; ///< The packing it was baked with.
        /// When @c mRingFieldBuffer is baked and read - see @ref LazyBake.
        LazyBake mRingField;
        /// A config change since the last frame moved NeonConfig::intensity.
        /// Set by @ref OnConfigChanged, consumed by the next @ref Render: the
        /// fields serve a scaled intensity only while it moves (LazyBake).
        bool mIntensityMoving = false;

        /// Seconds of frame time @c mGlowCoverBuffer has gone unread - the ring
        /// lit uniformly, or the layer off. @ref Update releases the table at
        /// GLOW_COVER_RELEASE_SECONDS (docs/neon-perf-plan.md item 10).
        float mGlowCoverUnreadSeconds = 0.0f;

        /// Pass 1b's target: the composited colour, one RGBA8 attachment at the
        /// reduced scale - full size at 1.0 - covering what the blit reads
        /// (@c mScaledOuter) and never more than the whole reduced viewport -
        /// see GetBufferRegion. Released when the layer is disabled.
        Framebuffer mScaledBuffer{"NeonRenderer.Scaled"};

        /// The gather pass's target, at GetGatherScale -
        /// coarser than @c mScaledBuffer for any rect much bigger than a
        /// thumbnail - over the gather quad's box (@c mGatherOuter) clipped
        /// to the viewport: the gathered colour and arc coverage, plus a
        /// second attachment for the segments' only when there are segments,
        /// RGBA16F where the driver renders to it and RGBA8 where not. Read by
        /// pass 1 and the edge ring; released when the layer is disabled.
        Framebuffer mGatherBuffer{"NeonRenderer.Gather"};

        /// Index into the gather buffer's format list of the best format the
        /// driver has not refused. Only ever advances - see
        /// @ref resizeGatherBuffer.
        size_t mGatherFormat = 0;

        /// What pass 1a last drew into @c mGatherBuffer, so a frame whose
        /// gather would draw the same values can skip it (Render). Its inputs
        /// (@ref GatherInputs), the quad it covered - outer box and hole, as
        /// @c mGatherOuter / @c mGatherHole were then - and the region it was
        /// laid out over (GetBufferRegion's origin, size and texels). Every
        /// texel is a function of its position and those inputs alone, on a
        /// texel grid anchored to the viewport rather than to the region, so
        /// while the inputs hold the buffer answers any quad inside the one it
        /// covered: readers then read it through the region it was drawn over.
        /// That is what an animation of the glow's reach - an intensity or
        /// bloom pulse, a glow-radius or cutoff sweep - needs, since it moves
        /// the quad every frame and nothing the gather computes.
        bool mGatherDrawn = false;
        GatherInputs mGatherInputs;
        glm::vec2 mGatherDrawnOuter{0.0f};
        glm::vec2 mGatherDrawnHole{0.0f};
        glm::vec2 mGatherDrawnOrigin{0.0f};
        glm::vec2 mGatherDrawnSize{0.0f};
        glm::ivec2 mGatherDrawnTexels{0};

        /// The inputs other than time that @ref renderEmissionPass last baked
        /// @c mEmissionBuffer from - see @ref isEmissionTableStale.
        EmissionInputs mEmissionInputs;
        /// Whether @ref renderEmissionPass has written the table at all. False
        /// until the first bake: the buffer holds undefined texels until then,
        /// whatever @c mEmissionInputs happens to hold.
        bool mEmissionBaked = false;
        /// The @c time @ref renderEmissionPass last baked at. Only meaningful
        /// while @c hueRotationRate is non-zero; at 0 the table does not
        /// depend on time and this is not consulted.
        float mEmissionTime = 0.0f;
        /// How many times @ref renderEmissionPass has written
        /// @c mEmissionBuffer - the table's version, for @ref GatherInputs.
        uint32_t mEmissionBakes = 0;

        /// Whether the offscreen phase's buffers - @c mGatherBuffer, and below
        /// 1.0 @c mScaledBuffer - still hold exactly what this frame's passes
        /// 1a and 1b would draw into them, so @ref Render can skip both and
        /// never leave the caller's framebuffer.
        ///
        /// Both passes are pure functions of the config, the viewport, the two
        /// tables they read (emission, glow coverage) and - only through the
        /// hue rotation - the time; both buffers persist between frames and
        /// nothing else writes them. So: cleared by @ref OnConfigChanged on ANY
        /// change to the neon's config or the geometry (pass 1b reads too much
        /// of the former for a narrower gate to be worth its risk), but not on
        /// another layer's, which nothing here reads and an animation of which
        /// would otherwise clear it every frame; set by @ref Render after an
        /// offscreen phase that drew both; and honoured only on a frame whose
        /// emission table is current (@ref isEmissionTableStale, which carries the time
        /// and the ring's cross-fade), whose glow coverage table needs no bake,
        /// and whose viewport is @c mOffscreenViewport. Measured
        /// byte-identical; 2-6x on a still frame at scale 0.5 on an Apple M2
        /// Pro (docs/neon-perf-plan.md, item 4).
        bool mOffscreenCurrent = false;
        /// The viewport, px, @c mOffscreenCurrent's buffers were drawn for -
        /// the regions they cover are placed in it (GetBufferRegion).
        glm::ivec2 mOffscreenViewport{0};

        /// Which pieces of @c mGlowCoverBuffer have to be re-baked: bit 2b is
        /// band b's straight, bit 2b + 1 its corner, all eight when the shape
        /// moves. NOT set on every config change, as the emission table's
        /// flag used to be: the bake
        /// reads the two light blocks (the arcs and the effective segments),
        /// the rect's width, height, corner radius and winding, and the glow
        /// radius - every one of them in @ref renderGlowCoverPass or the
        /// blocks it binds - so @ref OnConfigChanged gates it on exactly
        /// those, as it gates @c mLightBlocksDirty. The bake is the costly
        /// pass (0.14-0.27 ms on an AMD Radeon Pro 5300M), and "any change"
        /// re-ran it every frame under an intensity or colour animation, or an
        /// animation of another LAYER's fields, none of which it reads.
        ///
        /// Per PIECE because each piece's texels integrate the lights over that
        /// piece alone: a changed arc or segment dirties only the pieces its
        /// old and new supports reach (GetGlowCoverDirtyPieces), so a segment
        /// travelling along one straight re-bakes that band rather than the
        /// table - 1.18-1.91x on such frames on an Apple M2 Pro
        /// (docs/neon-perf-plan.md, item 5). The shape, the winding and the
        /// glow radius dirty all eight, as do the brightest arc's intensity
        /// and any light covering the whole ring.
        ///
        /// Accumulated, never assigned, for @c mLightBlocksDirty's reasons, and
        /// cleared only by the bake - so a change made while the bake is
        /// skipped (a ring lit uniformly) is still pending when it next runs.
        /// Also set whole by @ref ensureGlowCoverBuffer on a fresh allocation.
        /// Never depends on time. Starts with every piece - the buffer holds
        /// undefined texels until the first bake.
        uint32_t mGlowCoverDirty = 0xFFu; ///< GLOW_COVER_ALL_PIECES in the .cpp.

        /// Whether @c mSegmentBlock / @c mArcBlock still hold the current
        /// config. Cleared by @ref packLightBlocks once it has repacked.
        ///
        /// NOT set on every config change, as the emission table's flag used
        /// to be: the blocks are packed from @c mEffectiveSegments and
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
