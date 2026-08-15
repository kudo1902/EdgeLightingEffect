#ifndef _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
#define _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"
#include "gl/framebuffer.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// Half-resolution single-pass neon renderer for edge devices.
    ///
    /// Two-pass approach:
    ///   Pass 1 - render the neon shader (highp, up to @c NEON_MAX_LOOP_SAMPLES
    ///             gather samples per fragment) into a half-resolution RGBA8 FBO.
    ///   Pass 2 - bilinear blit the half-res FBO to the full-res backbuffer.
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
    /// standard single-pass @ref NeonRenderer), so switching between them
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
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        void rebuildGradientLUT(const Config &config);
        /// Quantise a float LUT (@p lutSize * 4 RGBA) to RGBA8 and upload it
        /// to mGradientLUT.
        void uploadGradientLUT(const std::vector<float> &lut, int lutSize);
        /// See NeonRenderer::rebuildSegmentLUT - same atlas shape (one row
        /// per segment) so the shader's sampling code stays identical.
        void rebuildSegmentLUT(const Config &config);
        /// See NeonRenderer::rebuildArcLUT - same atlas shape (one row per
        /// arc) so the shader's sampling code stays identical.
        void rebuildArcLUT(const Config &config);

        /// See NeonRenderer::packLightBlocks - both passes read these blocks,
        /// so they are packed before the emission pre-pass runs.
        void packLightBlocks(const Config &config);

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer, at @c optimizedNeon.numSamples so texel i here
        /// is sample i in the main pass. Restores framebuffer + viewport.
        void renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

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
        bool mEmissionIsFloat = false;      ///< False when the RGBA8 fallback was taken.
        bool mEmissionFormatLogged = false; ///< Keeps the fallback warning to one line.
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

        Texture2D mGradientLUT;
        /// Per-segment gradient atlas (see NeonRenderer::mSegmentLUT).
        Texture2D mSegmentLUT;
        /// Snapshot of the last-baked segments so OnConfigChanged only
        /// re-uploads when they actually differ.
        std::vector<SegmentBoost> mBakedSegments;
        /// Reusable scratch for the merged transient+preserved segment list
        /// (Config::FillEffectiveSegments); avoids per-frame heap allocation.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Per-arc gradient atlas (see NeonRenderer::mArcLUT).
        Texture2D mArcLUT;
        std::vector<Arc> mBakedArcs;

        // --- Gradient cross-fade -------------------------------------------
        // Same shape as NeonRenderer: bake into mLUTTarget, snapshot the
        // currently-shown ring into mLUTFrom, and let Update() blend
        // From->Target into mLUTDisplay over colorTransitionDuration seconds.
        // Extra wrinkle vs. the single-pass renderer: gradientLutSize is
        // runtime-tunable, so when it changes the buffer size changes too and
        // we can't lerp element-wise - we snap in that case (mLUTBakedSize
        // tracks the current width so we can detect the mismatch).
        std::vector<float> mLUTTarget;
        std::vector<float> mLUTFrom;
        std::vector<float> mLUTDisplay;
        int mLUTBakedSize = 0;      ///< Width (texels) of the buffers above; 0 until first bake.
        bool mHasBakedLUT = false;  ///< False until the first bake seeds the buffers.
        bool mFading = false;       ///< True while a cross-fade is in flight.
        float mFadeElapsed = 0.0f;  ///< Seconds into the current fade.
        float mFadeDuration = 0.0f; ///< Snapshot of the duration for this fade.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
