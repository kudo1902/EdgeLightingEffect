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
    /// is bilinear-blitted back. That draw is @ref RenderOffscreen's - the
    /// effect runs it for every layer before any layer composites, so the
    /// caller's target is drawn in one run - and the blit is @ref Render's.
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

        /// Phase 1: below scale 1.0, the flare into @c mScaledBuffer - sized,
        /// bound and cleared here, under no host scissor - and nothing on the
        /// caller's framebuffer. At 1.0, or with the layer off, nothing.
        /// Restores the framebuffer and viewport it was handed, and leaves
        /// @c GL_BLEND on with @c GL_SRC_ALPHA, @c GL_ONE_MINUS_SRC_ALPHA, as
        /// @ref Render does.
        virtual void RenderOffscreen(int viewportWidth, int viewportHeight, float time,
                                     const Config &config) override;

        /// Phase 2, onto the caller's framebuffer, premultiplied over: the
        /// scaled buffer blitted back below 1.0, the flare itself at 1.0. A
        /// host that calls it without RenderOffscreen for the same frame
        /// (EdgeLightingEffect never does) gets that phase run first, here.
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;
        virtual RendererLayer GetLayer() const override { return RendererLayer::LENS_FLARE; }

    private:
        /// What @ref RenderOffscreen leaves for @ref Render in the same frame.
        typedef struct FrameState
        {
            bool prepared = false;    ///< RenderOffscreen ran for this frame and Render has not consumed it.
            glm::ivec2 viewport{0};   ///< The viewport it ran for.
            bool scaledReady = false; ///< @c mScaledBuffer holds this frame's flare: the blit draws.
        } FrameState;

        bool setupShaders();
        void setupGeometry();

        /// Bake the per-ghost distance + colour table into @c mGhostBlock.
        ///
        /// Split out from @ref Render, where it used to run every frame, so
        /// the bake happens where its inputs change. It reads exactly three
        /// fields - @c ghostOffset, @c ghostColor, @c ghostTint - and produces
        /// FLARE_GHOST_COUNT vec4s from ten sin/cos pairs plus a hash per
        /// ghost.
        void bakeGhostBlock(const LensFlareConfig &flare);

        /// The flare's own draw, into whatever is bound: @p target px - the
        /// scaled buffer's size, or the viewport's at 1.0 - at @p scale of the
        /// @p viewportWidth x @p viewportHeight viewport, whose px the sun's
        /// position is in. Sets the program, its uniforms and the ghost block's
        /// binding; the caller sets the target and the blend.
        void drawFlare(const glm::ivec2 &target, float scale, int viewportWidth, int viewportHeight, float time,
                       const Config &config);

    private:
        ShaderProgram mFlareShader; ///< The flare itself (lens-flare.frag).
        /// Scaled path only: composites the scaled buffer back at full res.
        /// Built unconditionally rather than lazily - a shader compile in the
        /// middle of a frame, the first time someone drags the scale slider
        /// off 1.0, is a stall exactly where it will be blamed on the scale.
        /// (Reuses the neon blit shader - identical job.)
        ShaderProgram mBlitShader;
        /// Allocated only when the scaled path first runs; at scale 1.0 this
        /// stays empty and costs nothing. Drawn by @ref RenderOffscreen, read
        /// by @ref Render.
        Framebuffer mScaledBuffer{"LensFlare.Scaled"};
        /// Per-ghost distance + colour table (std140 GhostBlock). Filled by
        /// @ref bakeGhostBlock when its three inputs move, and bound every
        /// frame by @ref drawFlare.
        UniformBuffer mGhostBlock{"LensFlare.GhostBlock"};
        VertexArray mVertexArray{"LensFlare"};

        /// The flare config behind the current @c mGhostBlock contents.
        ///
        /// A whole @c LensFlareConfig rather than the three fields the bake
        /// reads: it holds no vectors, so copying and comparing it is a few
        /// dozen bytes, and a struct that already carries its own
        /// @c operator== cannot fall out of step with itself the way three
        /// hand-picked members would. Deliberately NOT a whole @c Config -
        /// nothing outside @c Config::lensFlare reaches the ghost table.
        LensFlareConfig mCurrentFlare;
        /// Whether @c mGhostBlock has been filled for @c mCurrentFlare. Starts
        /// false so the first bake is unconditional however the renderer is
        /// brought up - @ref Initialize and @ref OnConfigChanged both honour it.
        bool mGhostsBaked = false;
        /// What this frame's @ref RenderOffscreen left for @ref Render - see
        /// @ref FrameState.
        FrameState mFrame;
    };
}

#endif
