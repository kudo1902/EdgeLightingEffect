#ifndef _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_
#define _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/framebuffer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Draws a sun + hex-aperture lens flare (rays, chromatic ghosts).
    ///
    /// The flare pass is expensive per-pixel (many pows, 10-iteration ghost
    /// loop), so it renders into a half-resolution FBO first and bilinear-
    /// blits back to the main framebuffer - the 4x pixel reduction is the
    /// biggest single perf win on mobile GLES (Mali/Adreno) where fillrate
    /// and pow() throughput both bite. See lens-flare.frag for the shader-
    /// level optimisation notes.
    ///
    /// The sun rides the rect perimeter (see @c LensFlareConfig::perimeterPosition)
    /// so it animates with the same modulators the neon uses.
    ///
    /// @see LensFlareConfig for configuration options.
    class LensFlareRenderer : public BaseRenderer
    {
    public:
        LensFlareRenderer() = default;

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
        VertexArray mVertexArray{"LensFlare"};

        // Half-res render target + blit pass. Reuses NEON_VERT_SRC + NEON_BLIT_FRAG_SRC.
        Framebuffer mHalfResBuffer{"LensFlare.HalfRes"};
        ShaderProgram mBlitShader;
        VertexArray mBlitVertexArray{"LensFlare.Blit"};
    };
}

#endif
