#ifndef _EDGE_LIGHTING_WIREFRAME_RENDERER_H_
#define _EDGE_LIGHTING_WIREFRAME_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Renders a minimal 4-vertex GL_LINE_LOOP bounding-box overlay
    /// around the target rectangle.
    ///
    /// @see WireframeConfig for configuration options.
    class WireframeRenderer : public BaseRenderer
    {
    public:
        WireframeRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float headPos, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float headPos, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void buildGeometry(const Config &config);

        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray;
    };
}

#endif
