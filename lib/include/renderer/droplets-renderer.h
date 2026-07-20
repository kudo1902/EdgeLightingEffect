#ifndef _EDGE_LIGHTING_DROPLETS_RENDERER_H_
#define _EDGE_LIGHTING_DROPLETS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"

namespace EdgeLighting
{
    /// Rain-on-glass droplets renderer.
    ///
    /// Each frame it snapshots the current framebuffer into an internal
    /// texture with glCopyTexSubImage2D, then draws a fullscreen quad whose
    /// fragment shader (droplets.frag) paints trickling droplets into a band
    /// hugging the rounded-rect perimeter.
    ///
    /// The droplet field is hashed in screen space under a single global
    /// gravity, so rain falls straight down rather than circulating around the
    /// perimeter. What the rect geometry contributes is the band mask and the
    /// droplet size, which is derived from @c DropletsConfig::bandWidth - that
    /// is what lets the effect hold up in a band only a handful of pixels
    /// thick. Layer amplitudes are weighted by how vertical the local edge is,
    /// so rain streaks down the sides and beads along the top and bottom.
    ///
    /// Because the snapshot is taken at Render() time, everything drawn
    /// before this renderer - the demo background and the neon glow layers -
    /// is what the drops refract. Register it last.
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
        /// (Re)allocate mBackgroundCapture to the viewport size, then copy the
        /// current framebuffer into it. Generates mips only when @p wantMips
        /// (the frost blur samples higher LODs; clear glass skips the cost).
        void captureBackground(int viewportWidth, int viewportHeight, bool wantMips);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray{"DropletsRenderer"}; ///< Tight quad over the rect interior.

        /// Framebuffer snapshot sampled by the pane shader.
        Texture2D mBackgroundCapture;
        int mCaptureWidth = 0;  ///< Allocated snapshot width (tracks viewport resizes).
        int mCaptureHeight = 0; ///< Allocated snapshot height.
    };
}

#endif // _EDGE_LIGHTING_DROPLETS_RENDERER_H_
