#ifndef _NEON_MULTI_PASS_RENDERER_H_
#define _NEON_MULTI_PASS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// 4-pass neon renderer:
    ///   1. Bar      : write the premultiplied perimeter colour band to an FBO.
    ///   2. Blur H   : horizontal Gaussian blur of the bar into a second FBO.
    ///   3. Blur V   : vertical Gaussian blur into a third FBO (final halo).
    ///   4. Composite: sharp filament from the bar + smooth halo from the
    ///                 blurred FBO + tone-map. No closest-point projection,
    ///                 so no medial-axis seams.
    class NeonMultiPassRenderer : public BaseRenderer
    {
    public:
        NeonMultiPassRenderer() = default;
        virtual ~NeonMultiPassRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);

        Config mCurrentConfig;

        // --- Framebuffers (one per pass output) ---
        Framebuffer mBarBuffer{"NeonMultiPass.Bar"};
        Framebuffer mBlurHBuffer{"NeonMultiPass.BlurH"};
        Framebuffer mBlurVBuffer{"NeonMultiPass.BlurV"};

        // --- Shaders (one per stage) ---
        ShaderProgram mBarShader;        // Pass 1
        ShaderProgram mBlurShader;       // Passes 2 + 3 (separable Gaussian)
        ShaderProgram mCompositeShader;  // Pass 4

        // --- Geometry ---
        VertexArray mBarVertexArray;     ///< Tight quad: Pass 1 only rasterises near the perimeter.
        VertexArray mGlowVertexArray;    ///< Wide quad: blur + composite cover the bloom extent.

        // --- Reusable buffers for colour-stop array uploads ---
        std::vector<float>     mStopPositions;
        std::vector<glm::vec4> mStopColors;
    };
}

#endif
