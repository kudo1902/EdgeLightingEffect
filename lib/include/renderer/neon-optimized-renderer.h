#ifndef _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
#define _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
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
    ///   Pass 1 — render the neon shader (mediump, 64 gather samples) into
    ///             a half-resolution RGBA8 FBO.
    ///   Pass 2 — bilinear blit the half-res FBO to the full-res backbuffer.
    ///
    /// Visual parameters are read from Config::neon (shared with the
    /// standard single-pass NeonRenderer), so switching between them for
    /// comparison is a one-click toggle.
    class NeonOptimizedRenderer : public BaseRenderer
    {
    public:
        NeonOptimizedRenderer() = default;
        virtual ~NeonOptimizedRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

        static constexpr int MAX_LOOP_SAMPLES = 64;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        void rebuildGradientLUT(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mNeonShader; // Pass 1 — half-res neon
        ShaderProgram mBlitShader; // Pass 2 — upscale to full-res
        Framebuffer mHalfResBuffer{"NeonOptimized.HalfRes"};
        VertexArray mNeonVertexArray{"NeonOpt.Pass1"};
        VertexArray mBlitVertexArray{"NeonOpt.Blit"};

        std::vector<glm::vec2> mLoopSamples;
        float mSampleSpacing = 0.0f;

        Texture2D mGradientLUT;
        std::vector<float> mLUTScratch;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_OPTIMIZED_RENDERER_H_
