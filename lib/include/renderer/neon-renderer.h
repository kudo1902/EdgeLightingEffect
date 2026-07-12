#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
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

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        void rebuildGradientLUT(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mLUTDebugShader;                                 ///< Debug LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                               ///< Debug per-stop marker (neon-stop-marker.frag).
        VertexArray mVertexArray{"NeonRenderer"};                      ///< Tight glow quad (rect + earlyOut).
        VertexArray mFullVertexArray{"NeonRenderer.Full"};             ///< Viewport-covering quad for the opaque fill.
        VertexArray mLUTStripVertexArray{"NeonRenderer.LUTStrip"};     ///< Small centred quad for the LUT debug strip.
        VertexArray mStopMarkerVertexArray{"NeonRenderer.StopMarker"}; ///< Unit quad ([-1,+1]) used to draw each stop marker.
        glm::vec2 mLUTStripHalfSize{0.0f};                             ///< Half extents of the LUT strip in local px (matches mLUTStripVertexArray).

        /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
        /// block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonRenderer.SegmentBlock"};
        /// Backs neon.frag's std140 `LoopSamplesBlock` — vec4[NUM_LOOP_SAMPLES]
        /// where .xy holds the perimeter point in rect-local pixels.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};

        float mSampleSpacing = 0.0f;
        float mQuadMargin = 0.0f; ///< Draw-quad margin (px from rect edge); shader fades the bloom out by here.

        /// Baked colour ring as a 1×N RGBA32F texture (sampled with v=0.5 in the shader).
        /// Each shader sample becomes a single texture lookup instead of an in-shader stops loop + HSV blend
        Texture2D mGradientLUT;
        std::vector<float> mLUTScratch; ///< Float scratch for CPU gradient baking (LUT width * 4).
    };
}

#endif
