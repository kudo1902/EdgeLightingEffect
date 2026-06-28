#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
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

        /// Must match NEON_NUM_SAMPLES in lib/shaders/neon.frag.
        static constexpr int NUM_LOOP_SAMPLES = 128;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        ShaderProgram mFillShader;                         ///< Cheap black fill for opaque mode.
        VertexArray mVertexArray{"NeonRenderer"};          ///< Tight glow quad (rect + earlyOut).
        VertexArray mFullVertexArray{"NeonRenderer.Full"}; ///< Viewport-covering quad for the opaque fill.

        std::vector<glm::vec2> mLoopSamples;
        float mSampleSpacing = 0.0f;

        /// Per-loop-sample gather data (rgb = colour, a = weight), rebuilt each
        /// frame and uploaded as uSampleData[]. See ColorUtils::BuildSampleData.
        std::vector<glm::vec4> mSampleData;

        float mQuadMargin = 0.0f; ///< Draw-quad margin (px from rect edge); shader fades the bloom out by here.
    };
}

#endif
