#ifndef _EDGE_LIGHTING_BASE_RENDERER_H_
#define _EDGE_LIGHTING_BASE_RENDERER_H_

#include "core/config.h"
#include "core/buffer-registry.h"
#include <memory>
#include <utility>

namespace EdgeLighting
{
    class ResourceContext;
    class ShaderProgram;

    /// Which visual layer a renderer draws - its identity in
    /// @ref EdgeLightingEffect's layer calls (@ref EdgeLightingEffect::AddRenderer
    /// by layer, @ref EdgeLightingEffect::SetLayerOrder, ...).
    ///
    /// One bit per layer, numbered exactly like the C ABI's
    /// @c el_renderer_flags_e - a wall of static_asserts in capi-internal.h
    /// holds the two together, so append a new layer on the next free bit
    /// below DEBUG and mirror it there. DEBUG holds bit 30 so no content layer
    /// ever renumbers it.
    ///
    /// @c CUSTOM is for a host's own @ref BaseRenderer subclass. The layer
    /// calls cannot name one, so they leave custom renderers exactly where
    /// they are and reorder only the layers around them.
    typedef enum class RendererLayer : uint32_t
    {
        CUSTOM = 0,
        NEON = 1 << 0,
        DROPLETS = 1 << 1,
        LENS_FLARE = 1 << 2,
        SPOTLIGHT = 1 << 3,
        DEBUG = 1 << 30
    } RendererLayer;

    /// Abstract base class for all renderers in the EdgeLighting pipeline.
    ///
    /// Each renderer draws a single visual layer (stroke, wireframe, etc.)
    /// and is driven by the shared @ref EdgeLightingEffect update/render loop.
    class BaseRenderer
    {
    public:
        BaseRenderer() = default;
        virtual ~BaseRenderer() = default;

        /// Called once after construction to set up shaders and geometry.
        /// @return false if initialization fails (shader compile/link errors).
        virtual bool Initialize() = 0;

        /// Called every frame before @ref Render.
        /// @param deltaTime  Seconds since the last frame.
        /// @param time       Accumulated wall-clock time in seconds (paused when the animation is paused).
        /// @param config     Current active configuration.
        virtual void Update(float deltaTime, float time, const Config &config) = 0;

        /// Draws the renderer's visual layer.
        ///
        /// @pre The GL viewport is @c (0, 0, viewportWidth, viewportHeight).
        ///      Renderers may rely on this and may restore it by reconstruction
        ///      after an offscreen pass, rather than by querying GL_VIEWPORT.
        ///
        ///      This is not merely a tidiness convention - a **sub-viewport is
        ///      not supported**, because the shaders bake the assumption in.
        ///      Several read @c gl_FragCoord, which is in WINDOW coordinates,
        ///      and compare it against uniforms the CPU computes as if the
        ///      viewport origin were (0, 0) - e.g. black-rect.frag's
        ///      @c localPos = gl_FragCoord.xy - uRectCenter, where uRectCenter
        ///      comes from @c Config::geometry::position. Under a viewport at
        ///      origin (x, y) every such comparison is off by exactly that
        ///      origin, so the shape would render displaced. Supporting
        ///      sub-viewports means threading an origin through to those
        ///      uniforms, not just restoring the viewport more carefully.
        ///
        ///      The framebuffer, by contrast, is NOT assumed: it may be the
        ///      default framebuffer or a real FBO (see @c OffscreenCapture), so
        ///      a multi-pass renderer captures it with @c Framebuffer::GetBoundId
        ///      and restores exactly that.
        ///
        /// @param viewportWidth  Current framebuffer width.
        /// @param viewportHeight Current framebuffer height.
        /// @param time       Accumulated wall-clock time in seconds (paused when the animation is paused).
        /// @param config     Current active configuration.
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) = 0;

        /// Called when the configuration changes (e.g. on key press).
        /// @param config  The new configuration to adapt to.
        virtual void OnConfigChanged(const Config &config) = 0;

        /// The layer this renderer draws. Pure virtual on purpose: a renderer
        /// that forgot to say which layer it is would otherwise be silently
        /// unreachable by every layer call. A host's own renderer returns
        /// @ref RendererLayer::CUSTOM.
        virtual RendererLayer GetLayer() const = 0;

        /// Where this renderer publishes its offscreen buffers, and finds
        /// those of the layers drawn before it - see @ref BufferRegistry.
        /// @c EdgeLightingEffect::AddRenderer hands every renderer the
        /// effect's own; a renderer driven without an effect has none, and
        /// publishes and finds nothing.
        void SetBufferRegistry(std::shared_ptr<BufferRegistry> registry) { mBufferRegistry = std::move(registry); }

        /// What this renderer shares with the other renderers of its effect -
        /// see @ref ResourceContext. @c EdgeLightingEffect::AddRenderer hands
        /// every renderer the effect's own, before it initialises a late one;
        /// a renderer driven without an effect has none, and builds
        /// everything for itself.
        void SetResourceContext(std::shared_ptr<ResourceContext> context) { mResourceContext = std::move(context); }

    protected:
        /// A program linking the vertex stage @p vertSrc and the fragment
        /// stage @p fragSrc - with @p fragDefines spliced in after its
        /// #version line when given (@c ShaderCache::WithDefines) - each taken
        /// from the context's @c ShaderCache. A stage any renderer of the
        /// effect compiled already is linked, not compiled again, and a stage
        /// whose compile failed is not compiled again either: the program
        /// comes back invalid, and the failure was logged once, by that first
        /// compile. Both sources must be @c ShaderSource constants, since the
        /// cache keeps the pointers.
        ///
        /// Always a program object of its own: the cache shares stages, never
        /// programs (see @c ShaderCache). Without a context both stages are
        /// compiled for this program alone, as before the cache existed.
        ShaderProgram BuildProgram(const char *vertSrc, const char *fragSrc, const char *name,
                                   const char *fragDefines = nullptr) const;

        /// Publish attachment @p attachment of @p buffer as a TABLE - a buffer
        /// that maps onto no area of the frame - if this renderer has a
        /// registry. Call it from @ref Render, once the buffer holds what this
        /// frame used. Listed under the Framebuffer's own name.
        void publishBuffer(const Framebuffer &buffer, int attachment = 0)
        {
            PublishedBuffer record;
            record.buffer = &buffer;
            record.attachment = attachment;
            publish(record);
        }

        /// The same, for a buffer whose texels span the box @p origin,
        /// @p size of the caller's framebuffer (px, from its lower-left
        /// corner, y up) - the box its producer reads it over.
        void publishBuffer(const Framebuffer &buffer, const glm::vec2 &origin, const glm::vec2 &size,
                           int attachment = 0)
        {
            PublishedBuffer record;
            record.buffer = &buffer;
            record.attachment = attachment;
            record.placed = true;
            record.origin = origin;
            record.size = size;
            publish(record);
        }

        /// The registry, or nullptr - for a layer that reads what the layers
        /// before it published this frame.
        const BufferRegistry *getBufferRegistry() const { return mBufferRegistry.get(); }

    private:
        void publish(const PublishedBuffer &record)
        {
            if (mBufferRegistry)
            {
                mBufferRegistry->Publish(record);
            }
        }

        std::shared_ptr<BufferRegistry> mBufferRegistry;
        std::shared_ptr<ResourceContext> mResourceContext;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
