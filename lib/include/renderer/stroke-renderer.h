#ifndef _EDGE_LIGHTING_STROKE_RENDERER_H_
#define _EDGE_LIGHTING_STROKE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{

    /// Renders an SDF-based stroke along rectangle edges with anti-aliased edges
    /// and optional animated moving segments with semicircular head caps.
    ///
    /// @see Config::Stroke for available configuration options.
    class StrokeRenderer : public BaseRenderer
    {
    public:
        StrokeRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);

        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray;
    };
}

#endif
