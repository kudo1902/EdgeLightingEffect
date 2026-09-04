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
    /// table baked by the emission pre-pass. The per-sample arc scan, segment
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
    ///   <1.0 - the gather draws into @c mScaledBuffer at that fraction of the
    ///          viewport and is bilinear-blitted back over the target. Costs
    ///          one buffer and one fullscreen draw, saves the gather on
    ///          (1 - scale^2) of the fragments.
    ///
    /// The paths are the same code: every pixel-valued uniform is multiplied
    /// by the scale unconditionally (a no-op at 1.0) and the shader converts
    /// its own full-res px constants with @c uResolutionScale. Only the render
    /// target, the blit and the buffer allocation are actually conditional -
    /// which is what makes a scale of exactly 1.0 bit-identical to the
    /// dedicated full-res path this class used to have as a separate fork
    /// (@c NeonOptimizedRenderer, removed).
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

    private:
        bool setupShaders();
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
        /// cutoffs disabled by arithmetic (the default cutoff state, so the
        /// common way in). Those modes need no bounding geometry either way:
        /// @ref renderOpaqueFill clears for them, and on the rare state where
        /// a clear would not clip like a draw it falls back to the static
        /// fullscreen quad, never to a ring. Both passes ask one shared
        /// predicate so they cannot disagree; see @c FillsWholeViewport.
        void setupFillGeometry(const Config &config);
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

        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // The numbering is the pipeline order from docs/emission-prepass.md,
        // and the .cpp defines them in this same order - keep all three in
        // step. The data dependency is the real contract: pass 0 bakes the
        // table pass 1 reads, pass 2a's fill must land before pass 2b
        // composites the glow over it, and at scale 1.0 pass 1 IS the
        // composite (it draws onto the target directly and 2b does not run).
        //
        // NOTE @ref Render calls pass 2a FIRST - the one place declaration
        // order and call order differ. The fill depends on nothing above it
        // and must be UNDER the glow either way, so running it first is what
        // lets one schedule serve both resolution paths: the direct path needs
        // it down before the gather composites over it, and hoisting it also
        // keeps it ahead of the @c DebugConfig::opaqueOnly early-out, so
        // fill-only mode skips a UBO upload and two draws it never samples.

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

        /// Pack the segment + arc UBOs. Called before the emission pre-pass
        /// because BOTH passes read them: the pre-pass to bake the per-sample
        /// emission, the main pass for the continuous filament gate.
        /// @pre @c mEffectiveSegments is current for @p config - i.e.
        ///      @ref OnConfigChanged has run for any change since the last
        ///      frame, which the effect guarantees by calling Update before
        ///      Render. This method deliberately does not refill it.
        void packLightBlocks(const Config &config);

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer, at the clamped sample count so texel i here is
        /// sample i in the gather. Retargets the framebuffer and viewport, so
        /// it restores both before returning - see docs/emission-prepass.md.
        /// @pre Blending disabled - a table write is not a composite.
        /// @pre @c mEmissionBuffer is allocated, which @ref Initialize
        ///      guarantees for the renderer's lifetime - hence no failure to
        ///      report and nothing to allocate here.
        void renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Pass 1: the neon gather on the tight glow quad. Reads the emission
        /// table produced by @ref renderEmissionPass, so it must run after it.
        ///
        /// Draws either straight onto the bound framebuffer (@p scaled false)
        /// or into @c mScaledBuffer (@p scaled true), in which case it also
        /// clears that buffer and leaves it bound - @ref Render restores the
        /// target before pass 2b.
        /// @pre Premultiplied-over blending. Onto the target that composites
        ///      the glow over what is already there; into the cleared
        ///      transparent buffer it leaves premultiplied colour + coverage
        ///      alpha for the blit to composite instead.
        /// @return false if the scaled target could not be allocated, in which
        ///         case nothing was drawn and pass 2b must be skipped too - it
        ///         would otherwise composite a stale or undefined buffer.
        bool renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight,
                            bool scaled, float time, const Config &config);

        /// Pass 2a: opaque-mode background fill on a fullscreen NDC quad, at
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
        /// framebuffer. Only runs when the scaled path did.
        /// @pre Premultiplied-over blending, and the caller's framebuffer and
        ///      full-resolution viewport are restored.
        void renderBlitPass();

    private:
        Config mCurrentConfig;
        ShaderProgram mNeonShader;                         ///< The neon gather (neon.frag).
        ShaderProgram mEmissionShader;                     ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mBlackRectShader;                    ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mBlitShader;                         ///< Scaled-path upscale composite (neon-blit.frag).
        VertexArray mGlowVertexArray{"NeonRenderer.Glow"};             ///< Tight glow quad (rect + glow reach), in scaled space.
        VertexArray mFullscreenVertexArray{"NeonRenderer.Fullscreen"}; ///< NDC quad: emission bake, ALL-mode opaque fill, blit.
        VertexArray mFillVertexArray{"NeonRenderer.Fill"};             ///< Opaque-fill band ring (rect +- cutoffs), in FULL-RES rect-local px.
        /// Vertex count in @c mFillVertexArray - 24 for a ring (8 triangles),
        /// 0 when there is no ring and the fullscreen quad is used instead.
        /// Written by @ref setupFillGeometry, read by @ref renderOpaqueFill,
        /// and doubles as the "is it built" flag so the two cannot disagree.
        int mFillVertexCount = 0;

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

        float mQuadMargin = 0.0f; ///< Draw-quad margin (scaled px from rect edge); shader fades the bloom out by here.

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
        /// neon-emission.frag for the row packing). Rebuilt every frame by
        /// @ref renderEmissionPass and read by the gather with texelFetch.
        ///
        /// Also remembers, on the renderer's behalf, whether the driver would
        /// give it RGBA16F: @ref renderEmissionPass asks a live buffer for the
        /// format it already holds, so the RGBA8 fallback sticks without a flag
        /// here to say so.
        Framebuffer mEmissionBuffer{"NeonRenderer.Emission"};

        /// The gather's target on the scaled path. Never touched at
        /// @c resolutionScale 1.0 - not allocated, not bound, not blitted - so
        /// the full-res path pays nothing for its existence.
        Framebuffer mScaledBuffer{"NeonRenderer.Scaled"};
    };
}

#endif
