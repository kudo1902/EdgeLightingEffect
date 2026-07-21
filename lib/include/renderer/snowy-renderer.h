#ifndef _EDGE_LIGHTING_SNOWY_RENDERER_H_
#define _EDGE_LIGHTING_SNOWY_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Snowfall renderer.
    ///
    /// Draws a fullscreen quad whose fragment shader (snowy.frag) paints
    /// grid-hashed soft flakes drifting downward through a band along the
    /// rounded-rect perimeter (the same DropLayer trick droplets.frag uses,
    /// minus trails). No accumulation - flakes just appear randomly and
    /// fall through.
    ///
    /// Parameters come from @c Config::snowy; the band's side comes from
    /// @c Config::neon::glowSide and its geometry from @c Config::geometry.
    class SnowyRenderer : public BaseRenderer
    {
    public:
        SnowyRenderer() = default;
        virtual ~SnowyRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry();

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray{"SnowyRenderer"}; ///< Fullscreen NDC quad; the shader masks it to the band.
    };
}

#endif // _EDGE_LIGHTING_SNOWY_RENDERER_H_
