#ifndef _EDGE_LIGHTING_DEBUG_RENDERER_H_
#define _EDGE_LIGHTING_DEBUG_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "renderer/gradient-ring-lut.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{
    /// Debug annotations for the neon layer.
    ///
    /// Three overlays, each behind its own flag in @ref DebugConfig, drawn over
    /// whatever is already on the target:
    ///   - the baked colour ring as a horizontal STRIP at the rect centre
    ///     (@c showGradientLUT),
    ///   - one filled DISC per colour stop at its perimeter position
    ///     (@c showColorStops), and
    ///   - a 1 px sharp BOUNDING BOX around the rect (@c showWireframe), which
    ///     was its own renderer until this absorbed it.
    ///
    /// Together they answer "is what I configured what is actually being
    /// drawn?" - the strip shows the ring the shader samples, the markers show
    /// where each authored stop lands on the perimeter, and the glow underneath
    /// shows the result. All three should agree.
    ///
    /// This is a separate renderer rather than a mode of @ref NeonRenderer so
    /// that the neon pass schedule carries no debug shaders, buffers or
    /// branches at all. The cost of the split is one extra baked ring (see
    /// @c mGradientLUT); the benefit is that a host which never enables these
    /// pays nothing, and the neon renderer's passes are only the effect.
    ///
    /// REGISTER IT AFTER @ref NeonRenderer. It draws no depth and does not
    /// clear, so it annotates whatever the neon layer has already composited;
    /// registered before it, the glow would simply cover it.
    ///
    /// That placement is a deliberate BEHAVIOUR CHANGE for the bounding box.
    /// @c WireframeRenderer was registered first, so the box drew UNDER the
    /// glow and a strong bloom washed it out. Here it draws over the top,
    /// which is what a debug bounding box is for. The two cannot both be had:
    /// the other overlays annotate the glow and must follow it.
    ///
    /// Reads @c Config::debug for what to draw and @c Config::neon for what it
    /// is describing - the colour stops, the ring width and its cross-fade, the
    /// hue rotation. That is deliberate: an annotation of another layer's state
    /// has to read that layer's state.
    class DebugRenderer : public BaseRenderer
    {
    public:
        DebugRenderer() = default;
        virtual ~DebugRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        /// Upload the static unit quad every colour-stop marker is drawn from.
        /// Called once from @ref Initialize: the geometry reaches each marker
        /// through its own model matrix, so unlike @ref setupGeometry's quads
        /// the vertices never change.
        void setupStopMarkerQuad();
        /// Size the strip and the bounding box to the current geometry. The
        /// markers are not here - see @ref setupStopMarkerQuad.
        void setupGeometry(const Config &config);

        // STATE OWNERSHIP: as in NeonRenderer - `Render` owns blend state and
        // sets it before each overlay, and restores a known mode on the way out
        // because the two overlays want different ones. Neither retargets the
        // framebuffer, so neither has anything to restore there.

        /// The baked gradient LUT as a strip at the geometry centre.
        /// Caller guards on @c showGradientLUT.
        /// @pre Blending disabled - the strip overwrites the glow beneath it.
        void renderGradientLUTStrip(const glm::mat4 &mvp, float time, const Config &config);

        /// One filled disc per colour stop at its perimeter position.
        /// Caller guards on @c showColorStops + a non-empty list.
        /// @pre Straight-alpha blending, for the discs' anti-aliased edges.
        /// @note Takes @p proj, not the composed mvp: every marker gets its own
        ///       model matrix, so it needs the projection un-premultiplied.
        void renderColorStopMarkers(const glm::mat4 &proj, const glm::vec2 &center,
                                    float halfWidth, float halfHeight, const Config &config);

        /// The 1 px bounding box. Caller guards on @c showWireframe.
        /// @pre Blending disabled - a 1 px line wants no coverage blending, and
        ///      this is what the old WireframeRenderer did.
        void renderWireframe(const glm::mat4 &mvp, const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mLUTDebugShader;                                  ///< LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                                ///< Per-stop marker (neon-stop-marker.frag).
        ShaderProgram mWireframeShader;                                 ///< Bounding box (wireframe.frag).
        VertexArray mLUTStripVertexArray{"DebugRenderer.LUTStrip"};     ///< Small centred quad, sized to the rect.
        VertexArray mStopMarkerVertexArray{"DebugRenderer.StopMarker"}; ///< Unit quad ([-1,+1]), scaled per marker.
        VertexArray mWireframeVertexArray{"DebugRenderer.Wireframe"};   ///< 4 corners, drawn as GL_LINE_LOOP.
        glm::vec2 mLUTStripHalfSize{0.0f};                              ///< Half extents of the strip in local px (matches mLUTStripVertexArray).

        /// This renderer's OWN copy of the baked colour ring, from the same
        /// @c Config::neon inputs @ref NeonRenderer bakes from - so the strip
        /// previews the ring the glow is actually sampling, cross-fade and all,
        /// without either renderer reaching into the other.
        ///
        /// The duplicate bake is the price of the separation: one extra
        /// gradientLutSize x 1 RGBA8 texture, baked only when the stops change
        /// (@ref GradientRingLUT self-guards) and only while this renderer is
        /// registered. Sharing the neon renderer's instead would mean handing
        /// out a reference to its internals and ordering the two renderers'
        /// bakes against each other - far more coupling than a 1 KB texture is
        /// worth.
        GradientRingLUT mGradientLUT;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_DEBUG_RENDERER_H_
