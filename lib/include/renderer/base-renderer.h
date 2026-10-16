#ifndef _EDGE_LIGHTING_BASE_RENDERER_H_
#define _EDGE_LIGHTING_BASE_RENDERER_H_

#include "core/config.h"
#include "core/buffer-registry.h"
#include "core/read-list.h"
#include <memory>
#include <vector>

namespace EdgeLighting
{
    class BaseLUT;
    class EdgeLightingEffect;
    class Framebuffer;
    class ResourceContext;
    class ShaderProgram;
    class VertexArray;

    /// A renderer's own index of a buffer it registered
    /// (@ref BaseRenderer::RegisterBuffer) - stable for the renderer's life,
    /// whichever registry, if any, lists the buffer.
    typedef int BufferHandle;

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
        /// Defined in the .cpp, like the destructor: a constructor destroys the
        /// members on a throw, and @c mOwnFullscreenQuad's @c VertexArray is
        /// only forward-declared here, so an inline one would stop a host's
        /// subclass compiling from this header alone.
        BaseRenderer();
        /// Unregisters every buffer this renderer registered, so no record
        /// outlives the Framebuffer it points into.
        virtual ~BaseRenderer();

        BaseRenderer(const BaseRenderer &) = delete;
        BaseRenderer &operator=(const BaseRenderer &) = delete;

        /// Called once after construction to set up shaders and geometry.
        /// @return false if initialization fails (shader compile/link errors).
        virtual bool Initialize() = 0;

        /// Called every frame before @ref Render.
        /// @param deltaTime  Seconds since the last frame.
        /// @param time       Accumulated wall-clock time in seconds (paused when the animation is paused).
        /// @param config     Current active configuration.
        virtual void Update(float deltaTime, float time, const Config &config) = 0;

        /// Declares every buffer this renderer will bind this frame, by key,
        /// and in which phase: @c out.Add(this, key, ReadPhase::...). Called by
        /// @c EdgeLightingEffect::Render at the top of every frame, before any
        /// @ref Prepare, with the frame's config. Nothing by default - a
        /// renderer that reads no other renderer's buffer declares nothing.
        ///
        /// Declare exactly what the frame will bind - the read list is how a
        /// producer knows a product is wanted (@ref IsBufferWanted), how the
        /// effect orders a @ref Prepare after the one that makes what it reads
        /// (@ref PrepareSchedule), and how a read of a key nothing produces
        /// gets logged. A read declared and not bound costs a product made for
        /// nothing; a read bound and not declared may find the product not
        /// made, or not made yet.
        virtual void CollectReads(const Config & /*config*/, ReadList & /*out*/) const {}

        /// Draws the renderer's visual layer onto the caller's framebuffer: the
        /// on-screen half of the frame (@ref RenderOnscreen), after this
        /// frame's offscreen half (@ref RenderOffscreen) - which it runs itself
        /// first unless the effect's @ref Prepare ran it for this frame, so a
        /// renderer drawn on its own, without an effect, draws its whole frame.
        /// Not virtual: the guard is here, so no renderer can forget it.
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
        void Render(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Called when the configuration changes (e.g. on key press).
        /// @param config  The new configuration to adapt to.
        virtual void OnConfigChanged(const Config &config) = 0;

        /// The layer this renderer draws. Pure virtual on purpose: a renderer
        /// that forgot to say which layer it is would otherwise be silently
        /// unreachable by every layer call. A host's own renderer returns
        /// @ref RendererLayer::CUSTOM.
        virtual RendererLayer GetLayer() const = 0;

        /// What this renderer shares with the other renderers of its effect -
        /// see @ref ResourceContext. @c EdgeLightingEffect::AddRenderer hands
        /// every renderer the effect's own, before it initialises a late one;
        /// a renderer driven without an effect has none, and builds
        /// everything for itself.
        ///
        /// Moves this renderer's buffer records with it: they leave the old
        /// context's registry and join the new one's (a key the new registry
        /// holds already is refused there - see @ref BufferRegistry::Register).
        /// nullptr takes them out of every registry.
        void SetResourceContext(std::shared_ptr<ResourceContext> context);

    protected:
        /// What @ref Prepare draws: this renderer's offscreen passes, into
        /// buffers of its own. Nothing by default - a renderer that draws only
        /// onto the caller's framebuffer, or a host's own renderer that still
        /// does everything in @ref RenderOnscreen, leaves it alone and works as
        /// before; it only misses the ordering. Keep whatever the on-screen
        /// passes need from it in members.
        virtual void RenderOffscreen(int /*viewportWidth*/, int /*viewportHeight*/, float /*time*/,
                                     const Config & /*config*/)
        {
        }

        /// What @ref Render draws once the frame's offscreen half has run: this
        /// renderer's passes onto the caller's framebuffer, and nothing
        /// offscreen. Every other renderer's offscreen half may have run since
        /// this one's, so it sets every piece of GL state it reads - blend,
        /// texture and uniform-block bindings - rather than inheriting it.
        virtual void RenderOnscreen(int viewportWidth, int viewportHeight, float time, const Config &config) = 0;

        /// A program linking the vertex stage @p vertSrc and the fragment
        /// stage @p fragSrc - with @p fragDefines spliced in after its
        /// #version line when given (@c ShaderCache::WithDefines) - each taken
        /// from the context's @c ShaderCache. A stage any renderer of the
        /// effect compiled already is linked, not compiled again, and a stage
        /// whose compile failed is not compiled again either: the program
        /// comes back invalid, and the failure was logged once, by that first
        /// compile. The cache matches stages by their text, so the sources
        /// need only live through this call.
        ///
        /// Always a program object of its own: the cache shares stages, never
        /// programs (see @c ShaderCache). Without a context both stages are
        /// compiled for this program alone, as before the cache existed.
        ShaderProgram BuildProgram(const char *vertSrc, const char *fragSrc, const char *name,
                                   const char *fragDefines = nullptr) const;

        /// Lists attachment @p attachment of @p buffer in the context's
        /// registry, under the Framebuffer's name - "[n]" after it for
        /// attachment n > 0 - for the debug view, a host UI and, when
        /// @p visibility is @c PUBLIC, other layers. Call it once per buffer,
        /// from the constructor, where the Framebuffer member already has its
        /// name: the record is kept by this renderer and goes to whichever
        /// context it is handed (@ref SetResourceContext), and is removed with
        /// it. Registering an unallocated buffer is fine - it is listed, never
        /// current, until it has a texture.
        /// @returns the handle @ref MarkBufferCurrent takes.
        BufferHandle RegisterBuffer(const Framebuffer &buffer, int attachment = 0,
                                    BufferVisibility visibility = BufferVisibility::DEBUG);

        /// The same under @p key rather than the Framebuffer's name - for a
        /// PRODUCT, whose key names its contract rather than the buffer that
        /// happens to hold it today (the neon's @c NeonRenderer::GLOW_KEY).
        BufferHandle RegisterBuffer(const Framebuffer &buffer, const char *key, BufferVisibility visibility,
                                    int attachment = 0);

        /// The same, for a LUT, under @p key - a LUT has no name of its own.
        /// Listed, never current, until its first bake uploads an image.
        BufferHandle RegisterBuffer(const BaseLUT &lut, const char *key,
                                    BufferVisibility visibility = BufferVisibility::DEBUG);

        /// Marks buffer @p handle current this frame as a TABLE - a buffer
        /// that maps onto no area of the frame: its content describes this
        /// frame, written by it (@p changed) or a cache checked and found to
        /// hold. Call it from @ref RenderOffscreen - records go current in the
        /// offscreen half, before any layer's @ref Render, so every reader
        /// finds them wherever it sits in the order; a mark from
        /// @ref RenderOnscreen comes after the readers ahead of this layer -
        /// once the buffer holds what the frame used, and only then: a buffer
        /// a failed pass left behind is not marked. One store; nothing
        /// without a context.
        void MarkBufferCurrent(BufferHandle handle, bool changed);

        /// The same, for a buffer whose texels span the box @p origin,
        /// @p size of the caller's framebuffer (px, from its lower-left
        /// corner, y up) - the box its producer reads it over.
        void MarkBufferCurrent(BufferHandle handle, const glm::vec2 &origin, const glm::vec2 &size, bool changed);

        /// The context's registry, or nullptr without one - for a layer that
        /// reads what other layers made.
        const BufferRegistry *GetBufferRegistry() const;

        /// The fullscreen NDC quad (@c ResourceContext::GetFullscreenQuad), for
        /// a pass that covers its whole target with an identity MVP - six
        /// vertices, attribute 0, @c GL_TRIANGLES. The context's, shared by
        /// every renderer of the effect; without a context, one of this
        /// renderer's own, built on the first call. Ask for it at draw time
        /// rather than keeping the reference: it changes with the context.
        const VertexArray &GetFullscreenQuad();

        /// Whether any renderer of the effect declared a read of buffer
        /// @p handle this frame (@ref CollectReads) - for a producer that
        /// makes a product only on demand. Valid from the top of the frame,
        /// so in @ref RenderOffscreen and @ref Render. False without a
        /// context, and for a buffer the registry refused (its key taken by
        /// another renderer's): no other renderer can read it.
        bool IsBufferWanted(BufferHandle handle) const;

    private:
        friend class EdgeLightingEffect;

        /// The OFFSCREEN half of a frame: every pass this renderer draws into
        /// buffers of its own (@ref RenderOffscreen), and nothing onto the
        /// caller's framebuffer. @c EdgeLightingEffect::Render calls it on
        /// every renderer before it calls @ref Render on any, so the caller's
        /// target is drawn in one unbroken run per frame - on a tiling GPU each
        /// switch away from it and back stores its tiles out and loads them in
        /// again - and so any renderer's @ref Render can read what any
        /// renderer's Prepare made, wherever the two sit in the compositing
        /// order (docs/resource-sharing-plan.md 3.3).
        ///
        /// Takes the arguments the frame's @ref Render will take. Restores the
        /// framebuffer and viewport it was handed, and hands back blending on
        /// with @c GL_SRC_ALPHA, @c GL_ONE_MINUS_SRC_ALPHA when it drew, as
        /// @ref Render does. A renderer with nothing offscreen does nothing
        /// here.
        ///
        /// The effect's alone, which calls it and then this frame's @ref Render
        /// on every renderer, so a Prepare is always consumed by the Render of
        /// its own frame. Public, it let a caller prepare a frame, skip its
        /// Render, and have the next frame's lone Render composite the stale
        /// buffers. A host drawing a renderer itself calls @ref Render, which
        /// runs both halves.
        void Prepare(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// The context this renderer is in, or nullptr - for the effect, which
        /// hands a renderer that fails to initialise after joining it back to
        /// the context it came from (another effect's, when the host shares
        /// one renderer between two).
        const std::shared_ptr<ResourceContext> &getResourceContext() const { return mResourceContext; }

        /// One buffer this renderer registered: what it was registered with,
        /// and its slot in the current context's registry
        /// (@c BufferRegistry::INVALID_SLOT with no context, or when that
        /// registry refused the key).
        typedef struct OwnedBuffer
        {
            char key[BUFFER_KEY_CAPACITY] = {};  ///< Empty when the key did not fit: never listed.
            const Framebuffer *buffer = nullptr; ///< Or...
            const BaseLUT *lut = nullptr;        ///< ...this: exactly one is set.
            int attachment = 0;
            BufferVisibility visibility = BufferVisibility::DEBUG;
            int slot = BufferRegistry::INVALID_SLOT;
        } OwnedBuffer;

        /// What the three @ref RegisterBuffer overloads share: keys @p owned
        /// from @p name (plus "[n]" for @p keyAttachment n > 0), logging a key
        /// that does not fit, lists it, keeps it and returns its handle.
        BufferHandle addOwned(OwnedBuffer owned, const char *name, int keyAttachment);
        /// Lists @p owned in the context's registry, if there is a context.
        void registerOwned(OwnedBuffer &owned);
        /// Takes every record this renderer owns out of the context's
        /// registry, if there is a context.
        void unregisterOwned();

    private:
        /// Every buffer this renderer registered, indexed by its
        /// @ref BufferHandle. Filled by the constructor, so no frame allocates.
        std::vector<OwnedBuffer> mOwnedBuffers;
        std::shared_ptr<ResourceContext> mResourceContext;
        /// The fullscreen quad of a renderer drawn without a context - see
        /// @ref GetFullscreenQuad. Null until one is asked for.
        std::unique_ptr<VertexArray> mOwnFullscreenQuad;
        /// Set by @ref Prepare, consumed by the same frame's @ref Render:
        /// whether this frame's offscreen passes have run.
        bool mPrepared = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
