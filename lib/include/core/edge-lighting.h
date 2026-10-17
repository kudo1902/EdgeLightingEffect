#ifndef _EDGE_LIGHTING_EDGE_LIGHTING_H_
#define _EDGE_LIGHTING_EDGE_LIGHTING_H_

#include "core/config.h"
#include "animation/clock.h"
#include "animation/animation.h"
#include "renderer/base-renderer.h"
#include "core/read-list.h"
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace EdgeLighting
{
    class AnimationManager;

    /// @brief Top-level orchestrator for the edge-lighting effect.
    ///
    /// Owns:
    /// - a base @ref Config (authored - what @ref SetConfig writes),
    /// - an active @ref Config (base + animation overlays - what renderers see),
    /// - a time @ref Clock, and
    /// - an internal @ref AnimationManager plus a vector of renderers.
    ///
    /// ## Config split
    ///
    /// Callers write to the base config via @ref SetConfig / @ref GetConfig -
    /// this is the authored / slider-driven value. Each @ref Update ticks
    /// the clock, advances every attached animation by the clock delta,
    /// then rebuilds the active config as base + overlay and hands it to
    /// the renderers. Read the composited value with @ref GetActiveConfig
    /// (useful for UI slider hints that follow animated values).
    ///
    /// ## Animation policy
    ///
    /// Attach animations to the effect via @ref Attach - the manager owns
    /// the run loop, states, and end-action dispatch. Modulator composition
    /// (oscillators / easing / sequences) still lives outside in the
    /// @ref Modulator family; the animation subclasses (@ref IntensityPulse,
    /// @ref ArcWipe, @ref FieldBoundAnimation, ...) wrap those into @ref Config
    /// writes.
    class EdgeLightingEffect
    {
    public:
        EdgeLightingEffect();
        ~EdgeLightingEffect();

        /// @brief Initialise all registered renderers.
        ///
        /// A renderer that fails is logged and dropped from the list, so the
        /// rest of the effect still runs.
        ///
        /// Call this ONCE. There is no per-renderer guard, so a second call
        /// re-initialises every renderer already in the list: recompiling its
        /// shaders and reallocating every GL object it owns. Nothing leaks
        /// (the wrappers are RAII), but nothing is saved either. Renderers
        /// registered after this point do not need a second call - @ref
        /// AddRenderer initialises them on the spot.
        ///
        /// @returns false if any renderer failed to initialise.
        bool Initialize();

        /// @brief Advance animation time and propagate updates to renderers.
        /// @param deltaTime Seconds since the last frame.
        void Update(float deltaTime);

        /// @brief Draw every renderer, in two phases: each one's offscreen
        ///        passes (@ref BaseRenderer::Prepare), then each one onto the
        ///        caller's framebuffer (@ref BaseRenderer::Render).
        ///
        /// First every renderer declares the buffers it will read
        /// (@ref BaseRenderer::CollectReads). @c Render runs in compositing
        /// order; @c Prepare too, except that one whose passes read another
        /// renderer's buffer runs after that renderer's (@ref PrepareSchedule).
        /// @param viewportWidth  Current framebuffer width in pixels.
        /// @param viewportHeight Current framebuffer height in pixels.
        void Render(int viewportWidth, int viewportHeight);

        /// @brief Replace the base configuration and notify all renderers.
        ///
        /// Writes the BASE config, then recomposes and notifies through the
        /// same path @ref Update uses. With animations attached that composite
        /// carries the overlays at their CURRENT values - this frame's advance
        /// happens in the next @ref Update, which recomposes and notifies
        /// again. Callers following the documented Update-then-Render contract
        /// therefore see one notification per frame from Update, and one extra
        /// on frames where they also changed the base mid-animation.
        ///
        /// @param config New base configuration to apply.
        void SetConfig(const Config &config);

        /// @brief The base (authored) configuration.
        const Config &GetConfig() const;

        /// @brief The active (composited) configuration - base + animation overlays.
        const Config &GetActiveConfig() const;

        /// @brief The offscreen buffers the renderers own - keys, sizes,
        ///        channels, placement, and which are current - for a UI that
        ///        offers them to @c DebugConfig::bufferView.
        ///
        /// Every buffer a registered renderer registered is listed, from the
        /// moment the renderer is added, whether or not anything selected it;
        /// @ref BufferRegistry::IsCurrent says which the last @ref Render drew
        /// or used. A record READS its Framebuffer live: the texture, its size
        /// and its format are what they are now - texture 0 and 0 x 0 for a
        /// buffer released or never allocated, a new size after an @ref Update
        /// or @ref SetConfig reallocated it. Only the placement describes the
        /// frame it was last marked in. Records are owned by their renderers
        /// and leave the list with them, so nothing listed points into a
        /// renderer that is gone. See @ref BufferRegistry.
        const BufferRegistry &GetBufferRegistry() const;

        /// @brief Declare that the HOST binds the buffer under @p key after
        ///        each @ref Render, as a renderer declares a read in
        ///        @c BaseRenderer::CollectReads.
        ///
        /// A producer makes an on-demand product only while something reads
        /// it (@c BaseRenderer::IsBufferWanted) - the neon's shared buffer
        /// (@c NeonRenderer::COMPOSITE_KEY) is one - and a host is not a
        /// renderer, so it has no @c CollectReads to say so from. This is that
        /// declaration: added to every frame's reads, in the RENDER phase,
        /// with no reader (the schedule orders no @c Prepare after it), until
        /// @ref RemoveHostRead. Read the record through @ref GetBufferRegistry
        /// once @ref Render returns. The read diagnostics log a key that names
        /// no buffer, and a read of a @c DEBUG buffer - an intermediate its
        /// producer may change with any optimisation - as a renderer's would
        /// be: read a @c PUBLIC product.
        ///
        /// @returns false for an empty key, one longer than any buffer's
        ///          (@c BUFFER_KEY_CAPACITY), or one already declared.
        bool AddHostRead(std::string_view key);

        /// @brief Withdraw a read @ref AddHostRead declared.
        /// @returns false if @p key was not declared.
        bool RemoveHostRead(std::string_view key);

        /// @brief Attach a standalone animation to be advanced and composited
        ///        each @ref Update. Ignores null and duplicates.
        void Attach(const AnimationPtr &animation);

        /// @brief Detach a previously attached animation (by identity).
        /// @return true if it was attached and removed.
        bool Detach(const AnimationPtr &animation);

        /// @brief The animation manager (attach list + broadcast control).
        AnimationManager &GetAnimationManager();
        const AnimationManager &GetAnimationManager() const;

        /// @brief Register a renderer to be updated and rendered each frame.
        ///
        /// Renderers registered AFTER @ref Initialize are initialised on the
        /// spot, so a late registration is not left with no shaders and a
        /// @c Render that draws with program 0. One that fails to initialise is
        /// not added. Before @ref Initialize this just records the renderer, as
        /// it always did.
        ///
        /// Either way the renderer is handed the current active config
        /// immediately, so its first frame is not a blank one. A renderer
        /// already registered is refused (logged): it would draw twice a frame.
        ///
        /// The renderer joins this effect's @c ResourceContext, its buffer
        /// records with it. One that fails to initialise - here, or in
        /// @ref Initialize - goes back to the context it was in before, if
        /// any: a renderer the host shares with another effect stays listed
        /// there, where it is still drawn.
        ///
        /// @param renderer Shared pointer to a @ref BaseRenderer subclass.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// @brief The registered renderers, in compositing order (first drawn
        ///        first, so the last one sits on top).
        ///
        /// Reflects what @ref Initialize left behind: a renderer that failed to
        /// initialise has already been dropped from this list.
        const std::vector<std::shared_ptr<BaseRenderer>> &GetRenderers() const;

        /// @brief Re-order the registered renderers without re-initialising any
        ///        of them.
        ///
        /// @p order must be a PERMUTATION of @ref GetRenderers - every
        /// registered renderer exactly once, nothing else. Anything else is
        /// refused and leaves the current order untouched, so this can only
        /// move layers, never add or drop one (use @ref AddRenderer for that).
        ///
        /// Takes effect on the next @ref Render. Nothing else depends on the
        /// order: @ref Update and the config notifications reach every renderer
        /// regardless, and no renderer reads another's output except through
        /// the framebuffer, which is exactly what the order decides - and
        /// through @ref GetBufferRegistry, whose records go current in
        /// @c Prepare. Every @c Prepare runs before any @c Render, so a
        /// @c Render finds every layer's products current whatever the order;
        /// and a @c Prepare that declares a read (@c CollectReads) runs after
        /// that buffer's producer, not in this order.
        ///
        /// @returns false (and changes nothing) if @p order is not a
        ///          permutation of the registered renderers.
        bool SetRendererOrder(const std::vector<std::shared_ptr<BaseRenderer>> &order);

        /// @name Layers
        ///
        /// The same list addressed by @ref RendererLayer instead of by
        /// pointer, so a host can build and reorder the stack without holding
        /// on to the renderers. Every order here is BOTTOM FIRST.
        ///
        /// These calls see only the library's own layers. A renderer whose
        /// @ref BaseRenderer::GetLayer is @ref RendererLayer::CUSTOM is left
        /// out of @ref GetLayerOrder and keeps its position through every
        /// reorder; the named layers move around it, within the slots they
        /// already occupy. Use @ref SetRendererOrder to move a custom one.
        ///@{

        /// The renderer for @p layer, or nullptr for @ref RendererLayer::CUSTOM
        /// or a value that names no layer.
        static std::shared_ptr<BaseRenderer> CreateRenderer(RendererLayer layer);

        /// The default compositing order, bottom first: neon, droplets, lens
        /// flare, spotlight, debug. The debug overlays go last because they
        /// annotate the layers under them.
        static const std::vector<RendererLayer> &GetDefaultLayerOrder();

        /// Whether @p order is a well-formed layer list: every entry a named
        /// layer (not CUSTOM, not an unknown value), none repeated. Logs the
        /// first problem.
        static bool IsValidLayerList(const std::vector<RendererLayer> &order);

        /// Creates the renderer for @p layer and registers it through
        /// @ref AddRenderer, with the same initialise-if-late contract.
        /// @returns false if @p layer names no renderer, is already
        ///          registered, or failed to initialise.
        bool AddRenderer(RendererLayer layer);

        /// The named layers, bottom first. Custom renderers are skipped, and a
        /// layer whose renderer failed to initialise is absent - this is what
        /// @ref Render draws.
        std::vector<RendererLayer> GetLayerOrder() const;

        /// Re-orders the named layers. @p order must name every layer in
        /// @ref GetLayerOrder exactly once and nothing else; anything else is
        /// refused and changes nothing. Same cost and timing as
        /// @ref SetRendererOrder.
        bool SetLayerOrder(const std::vector<RendererLayer> &order);

        ///@}

        /// @brief Access the shared clock for play/pause/time control.
        Clock &GetClock();
        const Clock &GetClock() const;

    private:
        void refreshActiveConfig();
        /// The context @p renderer was in when it joined before
        /// @ref Initialize (@c mPriorContexts), forgotten here; nullptr if it
        /// had none, or that context is gone.
        std::shared_ptr<ResourceContext> takePriorContext(const BaseRenderer *renderer);

    private:
        Config mBaseConfig;   ///< Authored config - what SetConfig sets.
        Config mActiveConfig; ///< Base + animation overlays - forwarded to renderers.
        /// Scratch the animated composite is built in, held as a member so the
        /// per-frame path does no heap allocation.
        ///
        /// A local `Config active = mBaseConfig;` copy-CONSTRUCTS, which
        /// allocates fresh storage for every vector the config owns - the base
        /// colour stops, both segment pools, the arcs, and each of their own
        /// stop lists. Measured with a global operator new counter and one
        /// animation attached, that was 3 allocations and 3 frees per frame on
        /// the default config and 19 of each at the segment / arc caps, every
        /// frame, forever. Copy-ASSIGNING into a warm member instead reuses the
        /// capacity already there: 0 allocations.
        ///
        /// Not a throughput win - it measured ~1 microsecond a frame on a
        /// desktop allocator, which is nothing next to the GPU frame. It is
        /// here for the targets this library is actually aimed at, where ~1100
        /// malloc/free pairs a second is a fragmentation and jitter source
        /// rather than a cost in cycles.
        ///
        /// @ref refreshActiveConfig SWAPS this with @c mActiveConfig rather
        /// than assigning, which is what keeps the capacity: the scratch comes
        /// back owning the buffers the previous active config held, already the
        /// right size for the next frame's copy.
        Config mScratchConfig;
        /// The order @ref Render calls @c Prepare in, from the frame's reads -
        /// see @ref PrepareSchedule.
        PrepareSchedule mPrepareSchedule;
        /// The keys the host declared it reads (@ref AddHostRead), added to
        /// every frame's reads. Changed only by the host, so a frame
        /// allocates nothing for it.
        std::vector<std::string> mHostReads;
        /// Moves whenever a renderer joins or leaves @c mRenderers - what the
        /// schedule's read diagnostics compare, rather than the renderers'
        /// addresses, which a new renderer can reuse.
        uint64_t mRendererGeneration = 0;
        /// The context each renderer added before @ref Initialize was in when
        /// it joined - another effect's, for a renderer the host shares between
        /// two - so @ref Initialize can hand one that fails back to it rather
        /// than to none. Weak: this effect must not keep the other's context
        /// alive. Emptied by @ref Initialize; a late renderer keeps its own on
        /// the stack in @ref AddRenderer.
        std::vector<std::pair<const BaseRenderer *, std::weak_ptr<ResourceContext>>> mPriorContexts;
        Clock mClock;
        std::unique_ptr<AnimationManager> mAnimationManager;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
        /// What the renderers share - the compiled shader stages and the
        /// records of their offscreen buffers (see @ref ResourceContext).
        /// Handed to every renderer @ref AddRenderer registers, before it
        /// initialises a late one, so it outlives this effect for one a host
        /// still holds.
        std::shared_ptr<ResourceContext> mResourceContext;
        /// Set once @ref Initialize has run, so @ref AddRenderer knows whether
        /// a newly registered renderer still has an Initialize coming or has
        /// missed it and must be initialised immediately.
        bool mInitialized = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EDGE_LIGHTING_H_
