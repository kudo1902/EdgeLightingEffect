#ifndef _EDGE_LIGHTING_DROPLETS_RENDERER_H_
#define _EDGE_LIGHTING_DROPLETS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Rain-on-glass droplets renderer.
    ///
    /// Paints self-lit droplets into a REGION of the rounded-rect family:
    /// an outer rounded rect minus an inner one (@c DropletsConfig::outer,
    /// @c inner), or just the outer when @c hasInner is false. A perimeter
    /// band is the concentric case - @c inner on the rect, @c outer dilated by
    /// the thickness - and a filled pane is the one-boundary case.
    ///
    /// **The geometry is this layer's OWN.** It reads neither
    /// @c Config::geometry nor @c NeonConfig::glowSide, so moving or resizing
    /// the glow does not move the rain; a host that wants them to agree copies
    /// the numbers across. The single read of another layer's config is
    /// @c NeonConfig::glowSideSoftness, which feathers the region's boundary -
    /// a look knob, not geometry.
    ///
    /// The draw geometry is four strips bounding the region - not the viewport,
    /// and not the rect either. A band is thin, so a fullscreen quad rasterised
    /// millions of fragments that computed a region coordinate and discarded;
    /// the ring rasterises roughly what it shades. This pass's cost is
    /// therefore a function of the REGION, which the host sets directly: a
    /// 24 px band costs ~68k invocations where a filled 1920x1080 outer costs
    /// ~2.1M. The four sides are independent, since the inner shape may sit
    /// anywhere relative to the outer. @ref setupGeometry builds it; the
    /// transform in @ref Render places it. Nothing drawn changes - every
    /// fragment the ring drops was discarded by the shader anyway.
    ///
    /// **No resolution scale**, deliberately: the rims and speculars are
    /// single-pixel features. Measured against a stand-in that keeps full-res
    /// shading and loses only the blit, scale 0.5 takes the peak from 255 to
    /// 166 and leaves zero pixels at or above 200. The region is the cost knob
    /// instead.
    ///
    /// The droplet field is hashed in screen space under a single global
    /// gravity, so rain falls straight down rather than circulating around the
    /// perimeter. Drop size comes from @c DropletsConfig::dropSize divided by
    /// @c lanes - a property of the RAIN, and necessarily one GLOBAL scalar,
    /// since every region-relative term in the shader is an amplitude and never
    /// a position, precisely so the grid never shears. Layer amplitudes are
    /// weighted by how vertical the local run is, so rain streaks down the
    /// sides of a band and beads along the top and bottom; a FILLED region has
    /// no run direction at all and streaks everywhere.
    ///
    /// Drops are self-lit: a faint tinted body plus a crescent rim and a
    /// specular dot. No framebuffer capture, no refraction - the smooth neon
    /// gradient this region usually lives on has nothing worth refracting.
    ///
    /// Parameters come from @c Config::droplets. See
    /// @c docs/droplets-region-comparison.md for the region model and the
    /// measurements behind it.
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
        VertexArray mVertexArray{"DropletsRenderer"}; ///< Region ring (or one quad when there is no hole to cut).
        int mVertexCount = 6;                         ///< Vertices @ref setupGeometry last built: 24 for a ring, 6 for a quad.
    };
}

#endif // _EDGE_LIGHTING_DROPLETS_RENDERER_H_
