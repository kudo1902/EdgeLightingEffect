#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    class NeonRenderer : public BaseRenderer
    {
    public:
        NeonRenderer() = default;
        virtual ~NeonRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

        /// Width of the precomputed gradient look-up texture (RGBA32F, REPEAT wrap).
        /// 256 is more than enough for any 4-stop gradient the human eye can resolve.
        static constexpr int GRADIENT_LUT_SIZE = 256;
        /// Must match NEON_NUM_SAMPLES in lib/shaders/neon.frag.
        static constexpr int NUM_LOOP_SAMPLES = 128;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        void rebuildGradientLUT(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        ShaderProgram mBlackRectShader;                    ///< Opaque-mode black background fill (black-rect.frag).
        VertexArray mVertexArray{"NeonRenderer"};          ///< Tight glow quad (rect + earlyOut).
        VertexArray mFullVertexArray{"NeonRenderer.Full"}; ///< Viewport-covering quad for the opaque fill.

        std::vector<glm::vec2> mLoopSamples;
        float mSampleSpacing = 0.0f;

        /// Baked colour ring as a 1×N RGBA32F texture (sampled with v=0.5 in the shader).
        /// Each shader sample becomes a single texture lookup instead of an in-shader stops loop + HSV blend
        Texture2D mGradientLUT;
        std::vector<float> mLUTScratch; ///< Float scratch for CPU gradient baking (GRADIENT_LUT_SIZE * 4).

        /// Loop sample positions as an N×1 RGBA32F data texture (xy = position),
        /// texelFetch'd in the shader instead of a `uniform vec2[]` array.
        Texture2D mLoopSamplesTex;
        std::vector<float> mLoopSamplesData; ///< RGBA32F scratch: (x, y, 0, 0) per sample.
        float mQuadMargin = 0.0f;       ///< Draw-quad margin (px from rect edge); shader fades the bloom out by here.
    };
}

#endif
