#ifndef _EDGE_LIGHTING_DEBUG_RENDERER_H_
#define _EDGE_LIGHTING_DEBUG_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "util/gradient-lut.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// The pipeline's one diagnostic layer. Everything it draws is a
    /// visualisation of state the other renderers consume - none of it is part
    /// of the effect - so a shipping host can simply not register it and pay
    /// nothing (no shader compiles, no geometry, no draws).
    ///
    /// Three independent overlays, each behind its own flag in
    /// @ref DebugConfig, drawn in this order:
    ///
    ///  1. **Wireframe box** - 1 px @c GL_LINE_LOOP around the target
    ///     rectangle. Blending off for the duration.
    ///  2. **Gradient LUT strip** - the baked colour ring as a horizontal
    ///     strip at the geometry centre. Drawn unblended so it stays readable
    ///     over the tone-mapped glow.
    ///  3. **Markers** - one glyph per marked thing, in straight alpha
    ///     blending for the anti-aliased edges. Three families, each with its
    ///     own shape and its own flag, so they read apart when several are on:
    ///     a disc per colour stop (on the perimeter), a chevron per arc bound
    ///     (just outside it), and a diamond per segment position (just
    ///     inside it).
    ///
    /// Register it last: it composites over the finished frame, so the box and
    /// the markers stay visible on top of the neon rather than under it.
    ///
    /// The ring for overlay 2 is baked here from @c Config::neon (stops,
    /// blend space, transition duration) through the shared @ref GradientLUT,
    /// which is the same class the neon renderers bake theirs with - the strip
    /// therefore shows exactly what the neon shader samples, cross-fade
    /// included, without either renderer having to hand a texture over.
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
        /// (Re)builds both geometry-derived overlays: the line loop around the
        /// rect and the centred LUT strip quad.
        void setupGeometry(const Config &config);

        /// Overlay 1 - the bounding box. Toggles GL_BLEND off for the draw and
        /// back on after.
        void renderWireframe(const Config &config, const glm::mat4 &mvp);
        /// Overlay 2 - the baked colour ring as a strip at the geometry
        /// centre. Draws unblended; leaves GL_BLEND disabled for the caller
        /// to restore.
        void renderGradientLUTStrip(const Config &config, float time, const glm::mat4 &mvp);
        /// Overlay 3 - every enabled marker family. Switches to straight alpha
        /// blending for the anti-aliased edges and binds the marker shader
        /// once for all of them; the three draw* helpers below assume that
        /// setup and only issue their own draws.
        void renderMarkers(const Config &config, const glm::mat4 &proj,
                           const glm::vec2 &center);
        /// A disc per colour stop, in that stop's own colour, sitting on the
        /// perimeter.
        void drawColorStopMarkers(const Config &config, const glm::mat4 &proj,
                                  const glm::vec2 &center, float radius);
        /// A chevron per arc bound, offset outside the perimeter and rotated
        /// to point into the lit span (start green, end red). Full-perimeter
        /// arcs have no boundary, so they get a start marker only.
        void drawArcMarkers(const Config &config, const glm::mat4 &proj,
                            const glm::vec2 &center, float radius);
        /// One arc chevron at perimeter position @p t. @p forward orients it
        /// along the direction of travel (the start bound) or against it (the
        /// end bound) - either way it ends up pointing into the lit span.
        void drawArcBound(const Config &config, const glm::mat4 &proj,
                          const glm::vec2 &center, float radius, float t,
                          bool forward, const glm::vec4 &color);
        /// A diamond per segment centre, offset inside the perimeter, in the
        /// segment's own first stop colour (white when it inherits the base
        /// gradient). Reads the merged pool via SegmentUtils, matching what
        /// the neon renderers draw.
        void drawSegmentMarkers(const Config &config, const glm::mat4 &proj,
                                const glm::vec2 &center, float radius);
        /// Issues one marker draw: scales (and rotates) the unit quad to
        /// @p radius at @p localPt, which is a rect-local point already
        /// shifted by any perimeter offset. @p angle is the glyph's rotation
        /// in radians (only the chevron uses it).
        void drawMarker(const glm::mat4 &proj, const glm::vec2 &center,
                        const glm::vec2 &localPt, float radius, float angle,
                        int shape, const glm::vec4 &color);

    private:
        Config mCurrentConfig;

        ShaderProgram mWireframeShader;                       ///< Bounding box (wireframe.frag).
        ShaderProgram mLUTStripShader;                        ///< LUT strip (neon-lut-debug.frag).
        ShaderProgram mMarkerShader;                          ///< Stop / arc / segment marker glyphs (debug-marker.frag).
        VertexArray mWireframeVertexArray{"Debug.Wireframe"}; ///< 4-vertex line loop around the rect.
        VertexArray mLUTStripVertexArray{"Debug.LUTStrip"};   ///< Small centred quad for the LUT strip.
        VertexArray mMarkerVertexArray{"Debug.Marker"};       ///< Unit quad ([-1,+1]) scaled per marker.
        glm::vec2 mLUTStripHalfSize{0.0f};                    ///< Half extents of the strip in local px (matches mLUTStripVertexArray).

        /// Reusable scratch for the merged transient+preserved segment list
        /// (SegmentUtils::FillEffectiveSegments), so drawing the segment
        /// markers does no per-frame heap allocation after warmup.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Own copy of the neon colour ring, baked from Config::neon. See the
        /// class comment for why the strip bakes rather than borrows.
        GradientLUT mGradientLUT;
    };
}

#endif // _EDGE_LIGHTING_DEBUG_RENDERER_H_
