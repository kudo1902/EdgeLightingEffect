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
    /// Full-resolution neon renderer.
    ///
    /// Draws a tight quad over the rect + glow-reach margin and runs one
    /// fragment shader that composes filament + halo + bloom.
    ///
    /// Per-fragment work: an analytic rounded-box SDF, plus a gather loop over
    /// @c NEON_MAX_LOOP_SAMPLES perimeter samples (positions live in a UBO)
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
        // --- Setup and config-driven rebuilds (not per-frame) --------------
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        /// Re-bake the three colour LUTs. Each wrapper self-guards, so this is
        /// called unconditionally on every config change; see the note at the
        /// definition for what does and does not dirty a LUT.
        void bakeLUTs(const Config &config);

        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // The numbering is the pipeline order from docs/emission-prepass.md,
        // and the .cpp defines them in this same order - keep all three in
        // step. The data dependency is the real contract: pass 0 bakes the
        // table pass 2 reads, and pass 1's fill must land before pass 2
        // composites the glow over it.
        //
        // NOTE @ref Render executes pass 0 AFTER pass 1 here, which is the one
        // place declaration order and call order differ. The emission table
        // feeds only pass 2, so deferring it past the opaqueOnly early-out
        // lets fill-only debug mode skip a UBO upload and a draw it would
        // never sample. NeonOptimizedRenderer has no such inversion - there
        // both are inside the same guard, so it runs 0, 1, 2a, 2b in order.

        // STATE OWNERSHIP. `Render` owns blend state - enable and func - and
        // sets it immediately before each pass that depends on it, so no pass
        // touches GL_BLEND and the whole blend timeline reads in one place.
        // A pass owns its shader, and a pass that RETARGETS the framebuffer
        // restores it (an excursion, unlike a mode). Preconditions each pass
        // relies on are stated in its @pre below.
        //
        // The passes take the pieces of the frame transform they actually use.
        // @ref Render derives them once: `center` reaches the screen by two
        // routes - folded into @p mvp for the glow, and added to each stop's
        // perimeter point for the debug markers - so deriving it in one place
        // is what keeps the markers aligned with the glow they annotate.

        /// Pack the segment + arc UBOs. Called before the emission pre-pass
        /// because BOTH passes read them: the pre-pass to bake the per-sample
        /// emission, the main pass for the continuous filament gate.
        /// @pre @c mEffectiveSegments is current for @p config - i.e.
        ///      @ref OnConfigChanged has run for any change since the last
        ///      frame, which the effect guarantees by calling Update before
        ///      Render. This method deliberately does not refill it.
        void packLightBlocks(const Config &config);

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer. Retargets the framebuffer and viewport, so it
        /// restores both before returning - see docs/emission-prepass.md.
        /// @pre Blending disabled - a table write is not a composite.
        /// @return false if no emission target could be allocated at all (both
        ///         the RGBA16F and the RGBA8 attempt failed). @c Framebuffer::Resize
        ///         destroys the attachment on its failure path, so there is no
        ///         stale table left to fall back on - the caller must SKIP the
        ///         gather rather than let it texelFetch texture 0.
        bool renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Pass 1: opaque-mode background fill on a fullscreen NDC quad. The
        /// fragment shader shapes coverage from an analytic rounded-box SDF
        /// read off gl_FragCoord. Caller guards on @c opaqueMode != NONE.
        void renderOpaqueFill(const glm::vec2 &center, const Config &config);

        /// Pass 2: the neon gather on the tight glow quad. Reads the emission
        /// table produced by @ref renderEmissionPass, so it must run after it.
        void renderNeonPass(const glm::mat4 &mvp, float time, const Config &config);

        /// Debug pass: the baked gradient LUT as a strip at the geometry
        /// centre. Caller guards on @c showGradientLUT.
        /// @pre Blending disabled - the strip overwrites the glow beneath it.
        void renderGradientLUTStrip(const glm::mat4 &mvp, float time, const Config &config);

        /// Debug pass: one filled disc per colour stop at its perimeter
        /// position. Caller guards on @c showColorStops + a non-empty list.
        /// @pre Straight-alpha blending, for the discs' anti-aliased edges.
        /// @note Takes @p proj, not the composed mvp: every marker gets its own
        ///       model matrix, so it needs the projection un-premultiplied.
        void renderColorStopMarkers(const glm::mat4 &proj, const glm::vec2 &center,
                                    float halfWidth, float halfHeight, const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        ShaderProgram mEmissionShader;                                 ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mLUTDebugShader;                                 ///< Debug LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                               ///< Debug per-stop marker (neon-stop-marker.frag).
        VertexArray mVertexArray{"NeonRenderer"};                      ///< Tight glow quad (rect + glow reach).
        VertexArray mFullVertexArray{"NeonRenderer.Full"};             ///< Viewport-covering quad for the opaque fill.
        VertexArray mLUTStripVertexArray{"NeonRenderer.LUTStrip"};     ///< Small centred quad for the LUT debug strip.
        VertexArray mStopMarkerVertexArray{"NeonRenderer.StopMarker"}; ///< Unit quad ([-1,+1]) used to draw each stop marker.
        glm::vec2 mLUTStripHalfSize{0.0f};                             ///< Half extents of the LUT strip in local px (matches mLUTStripVertexArray).

        /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
        /// block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonRenderer.SegmentBlock"};
        /// Backs neon.frag's std140 `LoopSamplesBlock` - vec4[NUM_LOOP_SAMPLES]
        /// where .xy holds the perimeter point in rect-local pixels.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};
        /// Backs neon.frag's std140 `ArcBlock` (uArcCount + uArcs[MAX_ARCS]).
        UniformBuffer mArcBlock{"NeonRenderer.ArcBlock"};

        float mQuadMargin = 0.0f; ///< Draw-quad margin (px from rect edge); shader fades the bloom out by here.

        /// Baked colour ring (GRADIENT_LUT_SIZE x 1 RGBA8, sampled at v = 0.5).
        /// The wrapper owns the bake, the cross-fade and the guard behind them -
        /// see @ref GradientRingLUT.
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
        Framebuffer mEmissionBuffer{"NeonRenderer.Emission"};
        /// Latched the first time the driver refuses an RGBA16F emission
        /// target, after which the renderer asks only for RGBA8. Not just a
        /// log guard: @c Framebuffer::Resize treats a format change as a
        /// reallocation, so re-requesting 16F every frame would churn the
        /// texture + FBO twice per frame. RGBA8 is a working fallback, but it
        /// clamps arc intensities and stacked segment boosts above 1.0.
        bool mEmissionFloatUnavailable = false;

        /// Latches for the "more arcs / segments than the shader can hold"
        /// warnings, so the message lands once per overflow rather than once
        /// per frame. Cleared when the count drops back under the cap, so a
        /// host that overflows again is told again.
        bool mArcOverflowLogged = false;
        bool mSegmentOverflowLogged = false;
    };
}

#endif
