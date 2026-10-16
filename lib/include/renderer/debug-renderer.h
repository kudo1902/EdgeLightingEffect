#ifndef _EDGE_LIGHTING_DEBUG_RENDERER_H_
#define _EDGE_LIGHTING_DEBUG_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{
    /// Debug annotations for the neon layer.
    ///
    /// Four overlays, each behind its own flag in @ref DebugConfig, drawn over
    /// whatever is already on the target:
    ///   - the baked colour ring as a horizontal STRIP at the rect centre
    ///     (@c showGradientLUT),
    ///   - one filled DISC per colour stop at its perimeter position
    ///     (@c showColorStops),
    ///   - a 1 px sharp BOUNDING BOX around the rect (@c showWireframe), which
    ///     was its own renderer until this absorbed it, and
    ///   - one published OFFSCREEN BUFFER, one block per texel - in place over
    ///     the area of the frame it maps onto, or as a panel for a table
    ///     (@c bufferView).
    ///
    /// Together they answer "is what I configured what is actually being
    /// drawn?" - the strip shows the ring the shader samples, the markers show
    /// where each authored stop lands on the perimeter, and the glow underneath
    /// shows the result. All three should agree.
    ///
    /// This is a separate renderer rather than a mode of @ref NeonRenderer so
    /// that the neon pass schedule carries no debug shaders, buffers or
    /// branches at all. The strip samples the neon's own ring, which the neon
    /// registers (@c NeonRenderer::GRADIENT_RING_KEY) rather than draws; a host
    /// which never enables these pays nothing, and the neon renderer's passes
    /// are only the effect.
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
    /// is describing - the colour stops, the hue rotation. That is deliberate:
    /// an annotation of another layer's state has to read that layer's state.
    ///
    /// The strip and the buffer view are the exceptions to reading only the
    /// config. The strip samples the neon's colour ring itself, from the
    /// registry - so it shows the ring the glow samples on the same frame,
    /// cross-fade included, and a strip shown mid-fade shows the fade's
    /// current state rather than snapping to the new stops. A buffer
    /// is another renderer's output, which no config describes, so this layer
    /// finds it in the effect's @ref BufferRegistry - where it is current once
    /// its producer's offscreen half has run this frame, which every layer's
    /// has before any layer's on-screen half: any layer's buffer can be shown,
    /// wherever this one sits in the order.
    class DebugRenderer : public BaseRenderer
    {
    public:
        DebugRenderer() = default;
        virtual ~DebugRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        /// Both buffers this layer binds in @ref Render, while the overlays are
        /// on: the buffer view's selection (@c DebugConfig::bufferView), and
        /// the neon's colour ring (@c NeonRenderer::GRADIENT_RING_KEY) while
        /// the LUT strip is shown.
        virtual void CollectReads(const Config &config, ReadList &out) const override;
        virtual void OnConfigChanged(const Config &config) override;
        virtual RendererLayer GetLayer() const override { return RendererLayer::DEBUG; }

    protected:
        /// The overlays, onto the caller's framebuffer. No offscreen half.
        virtual void RenderOnscreen(int viewportWidth, int viewportHeight, float time,
                                    const Config &config) override;

    private:
        bool setupShaders();
        /// Build the buffer view's program on its first use - an overlay off
        /// by default should cost a host nothing, and a failure to build it
        /// must not fail @ref Initialize and take the other overlays with it.
        /// Tried once.
        bool ensureBufferViewShader();
        /// Size the strip and the bounding box to the current geometry. The
        /// markers and the buffer view are not here: they draw the effect's
        /// fullscreen quad (@c GetFullscreenQuad, [-1, +1] on both axes), each
        /// placed by its own model matrix.
        void setupGeometry(const Config &config);

        // STATE OWNERSHIP: as in NeonRenderer - `Render` owns blend state and
        // sets it before each overlay, and restores a known mode on the way out
        // because the overlays want different ones. None retargets the
        // framebuffer, so none has anything to restore there.

        /// @p buffer, nearest-sampled, with a 1 px outline: over the box it is
        /// placed at when it has one, else as a panel in the lower-left
        /// corner. Caller guards on @c bufferView naming a buffer current
        /// this frame.
        /// @pre Blending disabled - in place, the view replaces the frame in
        ///      its box rather than compositing over what was drawn from it.
        void renderBufferView(const glm::mat4 &proj, int viewportWidth, int viewportHeight,
                              const BufferRecord &buffer, const Config &config);

        /// The neon's colour ring, @p ring (@c NeonRenderer::GRADIENT_RING_KEY),
        /// as a strip at the geometry centre. Caller guards on
        /// @c showGradientLUT and on the ring being current this frame.
        /// @pre Blending disabled - the strip overwrites the glow beneath it.
        void renderGradientLUTStrip(const glm::mat4 &mvp, const BufferRecord &ring, float time,
                                    const Config &config);

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
        ShaderProgram mLUTDebugShader;                                ///< LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                              ///< Per-stop marker (neon-stop-marker.frag).
        ShaderProgram mWireframeShader;                               ///< Bounding box (wireframe.frag).
        ShaderProgram mBufferViewShader;                              ///< Buffer view (debug-buffer-view.frag); built on first use.
        bool mBufferViewFailed = false;                               ///< mBufferViewShader failed to build: not tried again.
        VertexArray mLUTStripVertexArray{"DebugRenderer.LUTStrip"};   ///< Small centred quad, sized to the rect.
        VertexArray mWireframeVertexArray{"DebugRenderer.Wireframe"}; ///< 4 corners, drawn as GL_LINE_LOOP.
        glm::vec2 mLUTStripHalfSize{0.0f};                            ///< Half extents of the strip in local px (matches mLUTStripVertexArray).
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_DEBUG_RENDERER_H_
