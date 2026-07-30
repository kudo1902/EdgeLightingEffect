#ifndef _EDGE_LIGHTING_LENS_FLARE_OPTIMIZED_RENDERER_H_
#define _EDGE_LIGHTING_LENS_FLARE_OPTIMIZED_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"

namespace EdgeLighting
{
    /// Half-resolution variant of @ref LensFlareRenderer.
    ///
    /// Two-pass approach:
    ///   Pass 1 - render the identical lens-flare fragment shader into a scaled
    ///             RGBA8 FBO (@c LensFlareOptimizedConfig::resolutionScale).
    ///   Pass 2 - bilinear-blit the scaled FBO to the full-res backbuffer.
    ///
    /// The whole flare is smooth and low-frequency (soft glow, ghosts, rays),
    /// so a bilinear upscale is nearly lossless while the expensive per-pixel
    /// flare math runs over @c resolutionScale² fewer fragments (4x fewer at
    /// half res, 16x fewer at quarter res). No shader changes are needed - the
    /// shader normalises everything by @c uResolution, so it is scale-invariant;
    /// the sun position is scaled into the FBO by the same factor.
    ///
    /// Visual parameters are read from @c Config::lensFlare (shared with the
    /// full-res @ref LensFlareRenderer), so switching between them for
    /// comparison is a one-click toggle. @c Config::optimizedLensFlare carries
    /// the perf knobs specific to this renderer (resolution scale, scaled-FBO
    /// debug toggle).
    ///
    /// @see LensFlareConfig / LensFlareOptimizedConfig for configuration options.
    class LensFlareOptimizedRenderer : public BaseRenderer
    {
    public:
        LensFlareOptimizedRenderer() = default;
        virtual ~LensFlareOptimizedRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry();

    private:
        Config mCurrentConfig;
        ShaderProgram mFlareShader; // Pass 1 - flare into the scaled FBO
        ShaderProgram mBlitShader;  // Pass 2 - upscale to full res
        Framebuffer mScaledBuffer{"LensFlareOpt.Scaled"};
        VertexArray mVertexArray{"LensFlareOpt"};
    };
}

#endif // _EDGE_LIGHTING_LENS_FLARE_OPTIMIZED_RENDERER_H_
