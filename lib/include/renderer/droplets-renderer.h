#ifndef _EDGE_LIGHTING_DROPLETS_RENDERER_H_
#define _EDGE_LIGHTING_DROPLETS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Rain-on-glass droplets renderer.
    ///
    /// Draws a band-fitted RING whose fragment shader (droplets.frag) paints
    /// self-lit droplets into a band hugging the rounded-rect perimeter.
    ///
    /// The geometry is four strips bounding the band itself - not the
    /// viewport, and not the rect either. The band is thin, so a fullscreen
    /// quad rasterised millions of fragments that computed a band coordinate
    /// and discarded; the ring rasterises roughly what it shades. This pass's
    /// cost is therefore a function of the PERIMETER and the band width, not
    /// of the rect's area and not of the display it lands on.
    /// @ref setupGeometry builds it; the transform in @ref Render places it.
    /// Nothing drawn changes - every fragment the ring drops was discarded by
    /// the shader anyway.
    ///
    /// The droplet field is hashed in screen space under a single global
    /// gravity, so rain falls straight down rather than circulating around the
    /// perimeter. What the rect geometry contributes is the band mask and the
    /// droplet size, which is derived from @c DropletsConfig::bandWidth - that
    /// is what lets the effect hold up in a band only a handful of pixels
    /// thick. Layer amplitudes are weighted by how vertical the local edge is,
    /// so rain streaks down the sides and beads along the top and bottom.
    ///
    /// Drops are self-lit: a faint tinted body plus a crescent rim and a
    /// specular dot. No framebuffer capture, no refraction - the smooth neon
    /// gradient this band lives on has nothing worth refracting anyway.
    ///
    /// Parameters come from @c Config::droplets; the band's side comes from
    /// @c Config::neon::glowSide and its geometry from @c Config::geometry.
    class DropletsRenderer : public BaseRenderer
    {
    public:
        DropletsRenderer() = default;
        virtual ~DropletsRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);

    private:
        Config mCurrentConfig; ///< Last config seen; @ref setupGeometry sizes the quad from it.
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray{"DropletsRenderer"}; ///< Band ring (or one quad when the band has no hole).
        int mVertexCount = 6;                         ///< Vertices @ref setupGeometry last built: 24 for a ring, 6 for a quad.
    };
}

#endif // _EDGE_LIGHTING_DROPLETS_RENDERER_H_
