#ifndef _EDGE_LIGHTING_DROPLETS_RENDERER_H_
#define _EDGE_LIGHTING_DROPLETS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"

namespace EdgeLighting
{
    /// Rain-on-glass droplets renderer ("wet window pane").
    ///
    /// Each frame it snapshots the current framebuffer into an internal
    /// texture with glCopyTexSubImage2D, then draws a quad over the rect
    /// interior whose fragment shader (droplets.frag) repaints the snapshot
    /// as wet glass: frost blur (mip LOD of the snapshot) everywhere, plus
    /// grid-hashed trickling droplets that refract the snapshot sharply.
    ///
    /// Because the snapshot is taken at Render() time, everything drawn
    /// before this renderer - the demo background and the neon glow layers -
    /// shows through the pane. Register it last so the glow gets frosted and
    /// lensed by the drops.
    ///
    /// Parameters come from @c Config::droplets; the pane is clipped to the
    /// rounded rect of @c Config::geometry.
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
