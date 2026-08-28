#ifndef _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
#define _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"
#include "renderer/span-atlas-lut.h"
#include "renderer/gradient-ring-lut.h"
#include <vector>

namespace EdgeLighting
{
    /// Half-resolution neon renderer for edge devices.
    ///
    /// Four passes. The numbering matches docs/emission-prepass.md and the
    /// declaration order further down, and here it is also the call order:
    ///   Pass 0  - bake the perimeter emission table into @c mEmissionBuffer.
    ///             Shares @c neon-emission.frag verbatim with @ref NeonRenderer.
    ///   Pass 1  - render the neon shader (highp, iterating
    ///             @c OptimizedNeonConfig::numSamples gather samples per
    ///             fragment) into a half-resolution RGBA8 FBO.
    ///   Pass 2a - opaque-mode black fill, drawn full-res on the backbuffer.
    ///   Pass 2b - bilinear blit the half-res FBO over it.
    ///
    /// Passes 0 and 1 sit inside the fill-only debug guard; 2a and 2b are the
    /// full-res backbuffer passes.
    ///
    /// The perf wins are the half-res FBO, the runtime-tunable
    /// @c OptimizedNeonConfig::numSamples slider (the shader iterates only
    /// that many perimeter samples per fragment), and the baked colour LUTs
    /// (base gradient + per-segment atlas + per-arc atlas) - not reduced
    /// precision. The shader uses highp: on desktop GLES (ANGLE) mediump =
    /// fp16, which overflows on the large fragment coordinates and produced
    /// NaN "noise dots".
    ///
    /// Visual parameters are read from @c Config::neon (shared with the
    /// full-resolution @ref NeonRenderer), so switching between them
    /// for comparison is a one-click toggle. @c Config::optimizedNeon carries
    /// the tuning knobs specific to this renderer (resolution scale, sample
    /// count, LUT size, half-res debug toggle).
    class NeonOptimizedRenderer : public BaseRenderer
    {
    public:
        NeonOptimizedRenderer() = default;
        virtual ~NeonOptimizedRenderer() = default;

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
        /// called unconditionally on every config change. See
        /// NeonRenderer::bakeLUTs.
        void bakeLUTs(const Config &config);

        // STATE OWNERSHIP: same rule as NeonRenderer - `Render` owns blend
        // state and sets it before each pass; a pass that retargets the
        // framebuffer restores it. See that header for the full note.
        //
        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // Same convention as NeonRenderer, and here declaration order, .cpp
        // definition order and @ref Render's call order all agree: 0, 1, 2a,
        // 2b. Passes 0 and 1 sit inside the fill-only guard; 2a/2b are the
        // full-res backbuffer passes. See docs/emission-prepass.md.

        /// See NeonRenderer::packLightBlocks - both passes read these blocks,
        /// so they are packed before the emission pre-pass runs.
        /// @pre @c mEffectiveSegments is current for @p config - see
        ///      NeonRenderer::packLightBlocks. Not refilled here.
        void packLightBlocks(const Config &config);

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer, at @c optimizedNeon.numSamples so texel i here
        /// is sample i in the main pass. Restores framebuffer + viewport.
        /// @pre Blending disabled - a table write is not a composite.
        /// @return false if no emission target could be allocated at all (both
        ///         the RGBA16F and the RGBA8 attempt failed). @c Framebuffer::Resize
        ///         destroys the attachment on its failure path, so there is no
        ///         stale table left to fall back on - the caller must SKIP the
        ///         gather rather than let it texelFetch texture 0.
        bool renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Pass 1: the neon gather at @c resolutionScale into
        /// @c mHalfResBuffer. Retargets the framebuffer and viewport; the
        /// caller restores them before the backbuffer passes.
        /// @pre Premultiplied-over blending, so the quad lands in the cleared
        ///      transparent FBO as premultiplied colour + coverage alpha.
        /// @returns false if the half-res target could not be allocated, in
        ///          which case nothing was drawn and Pass 2b must be skipped
        ///          too - it would otherwise composite a stale frame.
        bool renderHalfResNeonPass(int viewportHeight, int bufWidth, int bufHeight,
                                   float time, const Config &config);

        /// Pass 2a: opaque-mode background fill, drawn full-res on the
        /// backbuffer. Caller guards on @c opaqueMode != NONE.
        void renderOpaqueFill(int viewportHeight, const Config &config);

        /// Pass 2b: bilinear composite of the half-res FBO onto the
        /// backbuffer. Caller guards on the fill-only debug mode.
        void renderBlitPass(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mNeonShader;      // Pass 1 - half-res neon
        ShaderProgram mBlackRectShader; // Opaque-mode fullscreen black fill
        ShaderProgram mBlitShader;      // Pass 2 - upscale to full-res
        ShaderProgram mEmissionShader;  // Pass 0 - perimeter emission table
        Framebuffer mHalfResBuffer{"NeonOptimized.HalfRes"};
        /// Perimeter emission table, NEON_MAX_LOOP_SAMPLES x 2. Shares
        /// neon-emission.frag with NeonRenderer; see docs/emission-prepass.md.
        Framebuffer mEmissionBuffer{"NeonOptimized.Emission"};
        /// Latched RGBA8 fallback - see NeonRenderer for why it must latch.
        bool mEmissionFloatUnavailable = false;

        /// Latches for the "more arcs / segments than the shader can hold"
        /// warnings, so the message lands once per overflow rather than once
        /// per frame. Cleared when the count drops back under the cap, so a
        /// host that overflows again is told again.
        bool mArcOverflowLogged = false;
        bool mSegmentOverflowLogged = false;
        VertexArray mNeonVertexArray{"NeonOpt.Pass1"};
        VertexArray mBlitVertexArray{"NeonOpt.Blit"};

        /// Backs neon-optimized.frag's std140 `SegmentBlock` (DALi-compatible
        /// uniform block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonOpt.SegmentBlock"};
        /// Backs neon-optimized.frag's std140 `LoopSamplesBlock` - vec4 array
        /// where .xy holds the perimeter point in FBO pixels.
        UniformBuffer mLoopSamplesBlock{"NeonOpt.LoopSamplesBlock"};
        /// Backs neon-optimized.frag's std140 `ArcBlock` (uArcCount + uArcs[]).
        UniformBuffer mArcBlock{"NeonOpt.ArcBlock"};

        float mQuadMargin = 0.0f; ///< Scaled/FBO-space margin to the Pass-1 quad edge (shader soft-fade).

        /// Baked colour ring, width from @c OptimizedNeonConfig::gradientLutSize
        /// (see @ref GradientRingLUT, which also owns the cross-fade).
        GradientRingLUT mGradientLUT;
        /// Per-segment gradient atlas (see NeonRenderer::mSegmentLUT). Same
        /// atlas shape as the full-res renderer - fixed width rather than the
        /// tunable gradientLutSize, since a segment's span is short enough that
        /// extra resolution would not be visible.
        SpanAtlasLUT<SegmentBoost> mSegmentLUT;
        /// Reusable scratch for the merged transient+preserved segment list
        /// (Config::FillEffectiveSegments); avoids per-frame heap allocation.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Per-arc gradient atlas (see NeonRenderer::mArcLUT).
        SpanAtlasLUT<Arc> mArcLUT;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
