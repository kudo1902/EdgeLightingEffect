#ifndef _EDGE_LIGHTING_BASE_RENDERER_H_
#define _EDGE_LIGHTING_BASE_RENDERER_H_

#include "core/config.h"
#include "core/shared-buffer-registry.h"
#include <memory>

namespace EdgeLighting
{

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
        /// @param time       Accumulated clock time in seconds (paused when the clock is paused). A
        ///                   double, since an effect may run for days: never upload it to a
        ///                   shader as a float - reduce it on the CPU, in double, to the
        ///                   bounded phase the shader needs. A phase driven by a
        ///                   config rate is a @ref PhaseAccumulator, which
        ///                   integrates the rate (animation/clock.h): `time *
        ///                   rate` races whenever the rate moves.
        /// @param config     Current active configuration.
        virtual void Update(float deltaTime, double time, const Config &config) = 0;

        /// Phase 1 of a frame: draw whatever this layer draws OFFSCREEN - its
        /// own buffers, and any @ref SharedBuffer it produces - and nothing on
        /// the caller's framebuffer. @ref EdgeLightingEffect::Render runs it
        /// for EVERY layer before it runs any layer's @ref Render, so:
        ///   - a reader's Render sees this frame's shared images wherever it
        ///     sits in the layer order;
        ///   - every layer's offscreen work lands before the first draw to the
        ///     caller's target, which a tile-based GPU then draws in one
        ///     unbroken run instead of storing and reloading its tiles round
        ///     each offscreen excursion.
        /// Leaves the caller's framebuffer and viewport bound as it found them,
        /// and the blend state as @ref Render does. Same arguments as
        /// @ref Render, for the same frame. Does nothing by default: a layer
        /// with no offscreen work, or one that keeps it in Render, need not
        /// override it.
        virtual void RenderOffscreen(int viewportWidth, int viewportHeight, double time, const Config &config)
        {
            (void)viewportWidth;
            (void)viewportHeight;
            (void)time;
            (void)config;
        }

        /// Phase 2 of a frame: draws the renderer's visual layer onto the
        /// caller's framebuffer, after every layer's @ref RenderOffscreen.
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
        /// @param time       Accumulated clock time in seconds (paused when the clock is paused). A
        ///                   double, since an effect may run for days: never upload it to a
        ///                   shader as a float - reduce it on the CPU, in double, to the
        ///                   bounded phase the shader needs. A phase driven by a
        ///                   config rate is a @ref PhaseAccumulator, which
        ///                   integrates the rate (animation/clock.h): `time *
        ///                   rate` races whenever the rate moves.
        /// @param config     Current active configuration.
        virtual void Render(int viewportWidth, int viewportHeight, double time, const Config &config) = 0;

        /// Called when the configuration changes (e.g. on key press).
        /// @param config  The new configuration to adapt to.
        virtual void OnConfigChanged(const Config &config) = 0;

        /// The layer this renderer draws. Pure virtual on purpose: a renderer
        /// that forgot to say which layer it is would otherwise be silently
        /// unreachable by every layer call. A host's own renderer returns
        /// @ref RendererLayer::CUSTOM.
        virtual RendererLayer GetLayer() const = 0;

    protected:
        /// The shared-buffer registry of the effect this renderer is
        /// registered with, or nullptr for one no effect holds. A producer
        /// claims its slot there (from its Initialize); a reader takes another
        /// layer's buffer there in its Render - see @ref SharedBufferRegistry.
        /// Set before @ref Initialize and the first @ref OnConfigChanged.
        const std::shared_ptr<SharedBufferRegistry> &GetSharedBufferRegistry() const
        {
            return mSharedBufferRegistry;
        }

    private:
        /// Only the effect hands a renderer its registry, once, as it registers
        /// it - so no renderer can be pointed at another effect's after it has
        /// claimed a slot in its own.
        friend class EdgeLightingEffect;

        /// Called by @ref EdgeLightingEffect::AddRenderer, before Initialize.
        /// Held by shared ownership, so a renderer that outlives its effect
        /// still holds a live registry.
        void setSharedBufferRegistry(const std::shared_ptr<SharedBufferRegistry> &registry)
        {
            mSharedBufferRegistry = registry;
        }

    private:
        std::shared_ptr<SharedBufferRegistry> mSharedBufferRegistry; ///< See @ref GetSharedBufferRegistry.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
