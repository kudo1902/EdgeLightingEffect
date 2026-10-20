#ifndef _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_
#define _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "animation/clock.h"
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
        virtual void Update(float deltaTime, double time, const Config &config) override;

        /// Phase 1: below scale 1.0, the flare into @c mScaledBuffer - sized,
        /// bound and cleared here, under no host scissor - and nothing on the
        /// caller's framebuffer. At 1.0, or with the layer off, nothing.
        /// Restores the framebuffer and viewport it was handed, and leaves
        /// @c GL_BLEND on with @c GL_SRC_ALPHA, @c GL_ONE_MINUS_SRC_ALPHA, as
        /// @ref Render does.
        virtual void RenderOffscreen(int viewportWidth, int viewportHeight, double time,
                                     const Config &config) override;

        /// Phase 2, onto the caller's framebuffer, premultiplied over: the
        /// scaled buffer blitted back below 1.0, the flare itself at 1.0. A
        /// host that calls it without RenderOffscreen for the same frame
        /// (EdgeLightingEffect never does) gets that phase run first, here.
        virtual void Render(int viewportWidth, int viewportHeight, double time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;
        virtual RendererLayer GetLayer() const override { return RendererLayer::LENS_FLARE; }

        /// With @p bypass, @ref RenderOffscreen redraws the scaled buffer on
        /// every frame instead of reusing one whose @ref FlareInputs held - a
        /// verification hook, as NeonRenderer::SetBypass is: a frame drawn with
        /// it is the reference a reused frame must match byte for byte. From
        /// the next frame on.
        void SetBypass(bool bypass) { mBypass = bypass; }
        /// @return What @ref SetBypass last set; false by default.
        bool GetBypass() const { return mBypass; }

    private:
        /// What @ref RenderOffscreen leaves for @ref Render in the same frame.
        typedef struct FrameState
        {
            bool prepared = false;    ///< RenderOffscreen ran for this frame and Render has not consumed it.
            glm::ivec2 viewport{0};   ///< The viewport it ran for.
            bool scaledReady = false; ///< @c mScaledBuffer holds this frame's flare: the blit draws.
        } FrameState;

        /// Everything the scaled pass reads, as @ref drawFlare would upload and
        /// bind it: the key @ref RenderOffscreen compares to decide whether
        /// @c mScaledBuffer already holds this frame's flare.
        ///
        /// What the pass BINDS, not a list of fields it happens to read today:
        /// the whole @c LensFlareConfig (its @c operator== covers a field added
        /// later, and every uniform but two is a function of it alone), and
        /// for the rest the values themselves - the sun's position in the
        /// buffer (the geometry and the viewport reach the shader only through
        /// it), the rays' phase (the time and @c mRotation only through it),
        /// the target's size and the ghost block's upload count. A new input
        /// to @ref drawFlare that is not a function of @c Config::lensFlare
        /// belongs here, or a frame that moved only it reuses a stale buffer.
        typedef struct FlareInputs
        {
            LensFlareConfig flare;      ///< Config::lensFlare - every uniform derived from it.
            glm::ivec2 target{0};       ///< The buffer's size, px - uResolution.
            glm::vec2 sunPos{0.0f};     ///< uSunPos, scaled into the buffer.
            float rotationTurns = 0.0f; ///< The rays' phase, turns - uRotation over 2 PI.
            uint32_t ghostUploads = 0;  ///< mGhostBlock.GetUploadCount() - GhostBlock.

            bool operator==(const FlareInputs &o) const
            {
                return flare == o.flare && target == o.target && sunPos == o.sunPos &&
                       rotationTurns == o.rotationTurns && ghostUploads == o.ghostUploads;
            }
            bool operator!=(const FlareInputs &o) const { return !(*this == o); }
        } FlareInputs;

        /// What @c mScaledBuffer holds: the record that lets a frame skip the
        /// scaled pass and blit the buffer as it stands.
        typedef struct ScaledCache
        {
            bool drawn = false; ///< The buffer holds a flare at all; false whenever it is released, reallocated or a draw fails.
            FlareInputs inputs; ///< What it was drawn from.
        } ScaledCache;

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
        void drawFlare(const glm::ivec2 &target, float scale, int viewportWidth, int viewportHeight, double time,
                       const Config &config);

        /// uSunPos: the sun on the rect's perimeter, in the
        /// @p viewportWidth x @p viewportHeight viewport's gl_FragCoord px,
        /// times @p scale. @ref drawFlare uploads it and @ref getFlareInputs
        /// keys on it, so the two cannot disagree.
        static glm::vec2 getSunPosition(float scale, int viewportWidth, int viewportHeight, const Config &config);

        /// The rays' phase at @p time, turns in [0, 1): @c mRotation advanced
        /// to @p time. Advancing it twice at one time and rate returns one
        /// phase, so the key and the draw read the same value.
        float getRotationTurns(double time, const Config &config);

        /// The scaled pass's @ref FlareInputs for this frame.
        FlareInputs getFlareInputs(const glm::ivec2 &target, float scale, int viewportWidth, int viewportHeight,
                                   double time, const Config &config);

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
        /// The rays' rotation in turns: LensFlareConfig::rotationRate - an
        /// AnimatableField - integrated over the clock's time
        /// (PhaseAccumulator). Advanced in @ref Update, read by @ref drawFlare.
        PhaseAccumulator mRotation;
        /// What @c mScaledBuffer was last drawn from - see @ref ScaledCache.
        /// Below scale 1.0 a frame whose @ref FlareInputs match it skips the
        /// draw and blits the buffer as it stands: byte-identical, since the
        /// buffer is a pure function of those inputs. At 1.0 the flare draws
        /// onto the caller's framebuffer, so there is nothing to reuse.
        ScaledCache mScaledCache;
        /// @ref SetBypass: never reuse @c mScaledBuffer.
        bool mBypass = false;
    };
}

#endif
