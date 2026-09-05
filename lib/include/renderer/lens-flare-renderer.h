#ifndef _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_
#define _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"
#include "gl/uniform-buffer.h"

namespace EdgeLighting
{
    /// Draws a sun + hex-aperture lens flare (rays, chromatic ghosts) as a
    /// fullscreen premultiplied-alpha pass. The sun rides the rect perimeter
    /// (see @c LensFlareConfig::perimeterPosition), so it moves with the
    /// geometry and animates from the same parameter space as neon segments /
    /// arcs.
    ///
    /// **One renderer, two resolution paths**, selected by
    /// @c LensFlareConfig::resolutionScale: at 1.0 the flare draws straight
    /// onto the framebuffer it was handed (no offscreen buffer, no blit);
    /// below 1.0 it draws into a buffer of that fraction of the viewport and
    /// is bilinear-blitted back.
    ///
    /// Only TWO uniforms differ between the paths - @c uResolution and
    /// @c uSunPos - because the fragment shader normalises every term by
    /// @c uResolution and is therefore scale invariant. That is what makes the
    /// merge lossless where the neon's needed a scale factor threaded into the
    /// shader: handing this one the buffer size reproduces the same picture at
    /// lower resolution rather than a differently-shaped one.
    ///
    /// @see LensFlareConfig for configuration options.
    class LensFlareRenderer : public BaseRenderer
    {
    public:
        LensFlareRenderer() = default;
        virtual ~LensFlareRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry();

    private:
        ShaderProgram mFlareShader; ///< The flare itself (lens-flare.frag).
        /// Scaled path only: composites the scaled buffer back at full res.
        /// Built unconditionally rather than lazily - a shader compile in the
        /// middle of a frame, the first time someone drags the scale slider
        /// off 1.0, is a stall exactly where it will be blamed on the scale.
        /// (Reuses the neon blit shader - identical job.)
        ShaderProgram mBlitShader;
        /// Allocated only when the scaled path first runs; at scale 1.0 this
        /// stays empty and costs nothing.
        Framebuffer mScaledBuffer{"LensFlare.Scaled"};
        /// Per-ghost distance + colour table (std140 GhostBlock). Baked by
        /// BakeGhostTable and re-uploaded per frame; the bake is ten sin/cos
        /// pairs, so it is not worth a dirty flag.
        UniformBuffer mGhostBlock{"LensFlare.GhostBlock"};
        VertexArray mVertexArray{"LensFlare"};
    };
}

#endif
