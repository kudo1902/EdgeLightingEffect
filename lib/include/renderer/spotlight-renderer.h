#ifndef _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
#define _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"

#include <vector>

namespace EdgeLighting
{
    /// Freely placed and aimed cones of light, drawn as ONE additive pass.
    ///
    /// The odd one out in this library: every other renderer parameterises its
    /// look by position along the rect's perimeter, and this one does not
    /// touch the rect at all. A lamp sits at a @c SpotLight::position in app
    /// coordinates and points at a @c SpotLight::angle, and nothing occludes
    /// it - a cone crosses the frame freely.
    ///
    /// It also emits LIGHT ONLY. No backdrop, no fixture housings, no floor,
    /// no framebuffer capture, no LUT. What it is is the whole term stack in
    /// spotlight.frag, evaluated over the smallest geometry that can contain
    /// it.
    ///
    /// **One renderer, two resolution paths**, selected by
    /// @c SpotlightConfig::resolutionScale: at 1.0 the strips draw straight
    /// onto the framebuffer this renderer was handed (no offscreen buffer, no
    /// blit); below 1.0 they draw into a buffer of that fraction of the
    /// viewport and are bilinear-blitted back.
    ///
    /// **Not one uniform differs between the paths** - fewer than the flare's
    /// two. The fragment stage reads only interpolated full-res lamp-local
    /// coordinates and flat per-lamp pixel values, so the scaling happens
    /// entirely in the viewport transform: the same ortho over the same app
    /// coordinates, rasterised into a smaller buffer. Only the render target,
    /// the blit and the buffer allocation are conditional, which is what keeps
    /// 1.0 bit-identical to the single-path renderer this grew out of.
    ///
    /// Be aware of what the scale is worth here, because it is NOT the neon's
    /// or the flare's bargain: those shade the whole viewport, so quartering
    /// their fragments always beats a blit. This pass already bounds itself to
    /// what the lamps light, so the blit's full viewport of fragments is a
    /// FIXED cost it may not earn back - measured, it wins only above about
    /// six lamps at default settings. The default is 1.0 for that reason.
    ///
    /// **The geometry is the interesting part.** Each lamp gets a strip of
    /// @c SPOT_STRIP_SEGMENTS quads that hugs the region where that lamp
    /// writes anything at all, and every lamp's strip goes into one VBO drawn
    /// with one @c glDrawArrays. The bound is SOLVED, not guessed: the
    /// renderer inverts spotlight.frag's own falloff to find where it drops
    /// below half an 8-bit step (@ref SolveConeAcross), shared across the
    /// enabled lamps, and stops there. Measured at 1280x720, a typical single
    /// lamp rasterises 12-44% of the viewport and a five-lamp fan 123%;
    /// flattening the taper into an oriented bounding box of the same solve
    /// costs 1.16x to 1.50x more for an identical image.
    ///
    /// **Per-lamp scalars ride as vertex attributes**, constant across each
    /// strip, rather than in a std140 block. There is therefore no per-index
    /// array in either shader stage - the project's no-bare-uniform-arrays
    /// rule is satisfied by construction rather than by a block and a binding
    /// point - and the vertex stage pre-rotates each corner into the lamp's
    /// frame, so the fragment program never touches a sin or a cos.
    ///
    /// **The VBO holds app coordinates verbatim** and the y-flip lives in the
    /// projection (see @ref Render), so it depends only on
    /// @c Config::spotlight and never on the viewport: a resize costs one
    /// uniform, not a rebuild.
    ///
    /// Parameters come from @c Config::spotlight. Nothing else is read - not
    /// @c Config::geometry, not @c Config::neon.
    class SpotlightRenderer : public BaseRenderer
    {
    public:
        SpotlightRenderer() = default;
        virtual ~SpotlightRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        /// One vertex of a lamp's strip. Declared here rather than in the .cpp
        /// only so @c mStripVerts below can be a member; nothing outside this
        /// class names it. The four non-position members are constant across a
        /// whole strip - see spotlight.vert for why they ride as attributes
        /// instead of sitting in a uniform block.
        typedef struct StripVertex
        {
            float pos[2];   ///< App px, top-left origin, +y down.
            float local[2]; ///< (along, across) px in the lamp's frame.
            float p0[4];    ///< tanHalfBeam, throwLength, softK, intensity.
            float p1[4];    ///< apertureWidth, bloom, bloomRadius, bloomSupport.
            float color[3]; ///< Linear RGB.
        } StripVertex;

        static_assert(sizeof(StripVertex) == 15 * sizeof(float),
                      "StripVertex must be tightly packed - ensureBuffer's "
                      "attribute pointers use sizeof(StripVertex) as the stride.");

        bool setupShaders();
        void setupBlitGeometry();

        /// Allocate the VBO at its CEILING size and record the attribute
        /// layout, once. Never sized to the live lamp count: that would put a
        /// reallocation on the animation path, which is the mistake the neon
        /// emission table's sizing comment already records.
        void ensureBuffer();

        /// Solve every enabled lamp's support and upload the strips.
        ///
        /// Runs from @ref OnConfigChanged (and @ref Initialize), not from
        /// @ref Render, because its inputs are exactly @c Config::spotlight -
        /// the same invariant the lens flare's ghost table rests on. Under an
        /// animation driving any lamp scalar it therefore runs EVERY FRAME,
        /// so it is worth knowing what one costs.
        ///
        /// Measured over 2,000 rebuilds, best of nine runs: **~20 us for one
        /// lamp, ~32 us for eight**, or 0.1 - 0.2% of a 16.7 ms frame. That
        /// includes the <= 34 KB @c glBufferSubData into the ceiling-sized
        /// allocation @ref ensureBuffer made once.
        ///
        /// The flatness across lamp counts is the useful part of that number.
        /// The arithmetic is **998 transcendental calls for one lamp and 7,880
        /// for eight** (counted: 705 / 5,688 @c log, 290 / 2,168 @c sqrt), so
        /// eight lamps do eight times the maths for 1.6x the time - most of
        /// what is left is the upload and the call. A profile that lands here
        /// is more likely to be showing the driver than the solve.
        ///
        /// If the solve ever IS the cost, ~88% of those calls are the
        /// @c WIDEN_SUBSAMPLES loop - 208 @ref SupportAt evaluations per lamp
        /// against 13 for the sampling it corrects. Read the comment there
        /// first: it buys a guarantee, not an image, and it has never changed
        /// a pixel in any verification scene.
        void buildStrips(const SpotlightConfig &spotlight);

    private:
        ShaderProgram mShaderProgram;
        /// Scaled path only: composites the scaled buffer back at full res.
        /// Built unconditionally rather than lazily - a shader compile in the
        /// middle of a frame, the first time someone drags the scale slider
        /// off 1.0, is a stall exactly where it will be blamed on the scale.
        /// (Reuses the neon blit shader - identical job.)
        ShaderProgram mBlitShader;
        /// Allocated only when the scaled path first runs; at scale 1.0 this
        /// stays empty and costs nothing, and it is released again as soon as
        /// a config stops asking for it.
        Framebuffer mScaledBuffer{"Spotlight.Scaled"};
        VertexArray mVertexArray{"SpotlightRenderer"};
        /// Fullscreen NDC quad for the blit. A second VAO rather than a reuse
        /// of @c mVertexArray: that one carries the five-attribute strip
        /// layout, and the blit wants a bare vec2 at location 0.
        VertexArray mBlitQuad{"Spotlight.Blit"};

        /// The spotlight config behind the current VBO contents. A whole
        /// @c SpotlightConfig rather than hand-picked fields: it carries its
        /// own @c operator== and so cannot fall out of step with itself.
        /// Deliberately NOT a whole @c Config - nothing outside
        /// @c Config::spotlight reaches the strips.
        SpotlightConfig mCurrentSpotlight;

        /// Staging for @ref buildStrips, a member rather than a local for the
        /// reason @ref ensureBuffer gives about the VBO: under an animation
        /// that method runs every frame, and a local would heap-allocate and
        /// free its 34 KB ceiling on each one. Cleared, never shrunk, so it
        /// allocates exactly once.
        std::vector<StripVertex> mStripVerts;

        int mVertexCount = 0;       ///< Vertices @ref buildStrips last wrote.
        bool mBufferReady = false;  ///< Whether @ref ensureBuffer has run.
        /// Whether the VBO has been filled for @c mCurrentSpotlight. Starts
        /// false so the first build is unconditional however the renderer is
        /// brought up - @ref Initialize and @ref OnConfigChanged both honour it.
        bool mBuilt = false;
    };
}

#endif // _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
