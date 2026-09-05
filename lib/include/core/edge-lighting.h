#ifndef _EDGE_LIGHTING_EDGE_LIGHTING_H_
#define _EDGE_LIGHTING_EDGE_LIGHTING_H_

#include "core/config.h"
#include "animation/clock.h"
#include "animation/animation.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>

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

        /// @brief Render all active renderers in registration order.
        /// @param viewportWidth  Current framebuffer width in pixels.
        /// @param viewportHeight Current framebuffer height in pixels.
        void Render(int viewportWidth, int viewportHeight);

        /// @brief Replace the base configuration and notify all renderers.
        ///
        /// Renderers are notified from HERE only while no animation is
        /// attached, because only then is the base config also the composited
        /// one. With animations attached the notification is left to the next
        /// @ref Update, which is the first point this frame's overlays exist -
        /// notifying twice would hand the renderers the bare base first and
        /// the real composite immediately after. @ref GetActiveConfig
        /// therefore still reports the LAST composite until that Update runs.
        ///
        /// @param config New base configuration to apply.
        void SetConfig(const Config &config);

        /// @brief The base (authored) configuration.
        const Config &GetConfig() const;

        /// @brief The active (composited) configuration - base + animation overlays.
        const Config &GetActiveConfig() const;

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
        /// immediately, so its first frame is not a blank one.
        ///
        /// @param renderer Shared pointer to a @ref BaseRenderer subclass.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// @brief Access the shared clock for play/pause/time control.
        Clock &GetClock();
        const Clock &GetClock() const;

    private:
        void refreshActiveConfig();

    private:
        Config mBaseConfig;   ///< Authored config - what SetConfig sets.
        Config mActiveConfig; ///< Base + animation overlays - forwarded to renderers.
        Clock mClock;
        std::unique_ptr<AnimationManager> mAnimationManager;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
        /// Set once @ref Initialize has run, so @ref AddRenderer knows whether
        /// a newly registered renderer still has an Initialize coming or has
        /// missed it and must be initialised immediately.
        bool mInitialized = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EDGE_LIGHTING_H_
