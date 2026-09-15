#ifndef _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
#define _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

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
    /// no framebuffer capture, no offscreen buffer, no LUT, no resolution
    /// scale. What it is is the whole term stack in spotlight.frag, evaluated
    /// over the smallest geometry that can contain it.
    ///
    /// **The geometry is the interesting part.** Each lamp gets a strip of
    /// @c SPOT_STRIP_SEGMENTS quads that hugs the region where that lamp
    /// writes anything at all, and every lamp's strip goes into one VBO drawn
    /// with one @c glDrawArrays. The bound is SOLVED, not guessed: the
    /// renderer inverts spotlight.frag's own falloff to find where it drops
    /// below one 8-bit step (@ref SolveConeAcross), and stops there. Bounding
    /// the same cones with oriented boxes instead rasterised roughly ten times
    /// the area for an identical image.
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
        bool setupShaders();

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
        /// animation driving any lamp scalar it therefore runs every frame:
        /// that is ~100 transcendental calls and a <= 34 KB
        /// @c glBufferSubData, which is the number to look at first if this
        /// layer ever shows up in a profile.
        void buildStrips(const SpotlightConfig &spotlight);

    private:
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray{"SpotlightRenderer"};

        /// The spotlight config behind the current VBO contents. A whole
        /// @c SpotlightConfig rather than hand-picked fields: it carries its
        /// own @c operator== and so cannot fall out of step with itself.
        /// Deliberately NOT a whole @c Config - nothing outside
        /// @c Config::spotlight reaches the strips.
        SpotlightConfig mCurrentSpotlight;

        int mVertexCount = 0;       ///< Vertices @ref buildStrips last wrote.
        bool mBufferReady = false;  ///< Whether @ref ensureBuffer has run.
        /// Whether the VBO has been filled for @c mCurrentSpotlight. Starts
        /// false so the first build is unconditional however the renderer is
        /// brought up - @ref Initialize and @ref OnConfigChanged both honour it.
        bool mBuilt = false;
    };
}

#endif // _EDGE_LIGHTING_SPOTLIGHT_RENDERER_H_
