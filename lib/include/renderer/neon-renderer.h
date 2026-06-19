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
        /// Must match NEON_NUM_SAMPLES in lib/shaders/neon.frag.
        static constexpr int NUM_LOOP_SAMPLES = 128;

        NeonRenderer() = default;
        virtual ~NeonRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);

        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray;

        std::vector<glm::vec2> mLoopSamples;
        float mSampleSpacing = 0.0f;

        // Reusable buffers for colour-stop array uploads.
        std::vector<float> mStopPositions;
        std::vector<glm::vec4> mStopColors;
    };
}

#endif
