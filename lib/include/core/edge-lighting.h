#ifndef _EDGE_LIGHTING_EDGE_LIGHTING_H_
#define _EDGE_LIGHTING_EDGE_LIGHTING_H_

#include "core/config.h"
#include "animation/clock.h"
#include "animation/animation-manager.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>

namespace EdgeLighting
{
    /// @brief Top-level orchestrator for the edge-lighting effect.
    ///
    /// Owns the base configuration, the time clock, a list of renderers, and an
    /// @ref AnimationManager that centrally manages every attached animation.
    ///
    /// ## Config model (base vs active)
    ///
    /// - @ref SetConfig sets the BASE config - the authored values (sliders,
    ///   presets, hotkeys). @ref GetConfig returns it.
    /// - Each @ref Update advances the attached animations and composites them
    ///   onto a copy of the base to produce the ACTIVE config
    ///   (@ref GetActiveConfig), which is what the renderers draw.
    ///
    /// A field with no animation reads straight through from base to active; a
    /// field driven by a Stopped animation reverts to its base value. See
    /// @ref AnimationManager for the full field-lifetime contract.
    ///
    /// ## Animation policy
    ///
    /// Animations are @ref Attach ed to the effect and advanced by the effect's
    /// own clock, so pausing the clock freezes them. Parameter shapes still
    /// come from the @ref Modulator family - callers compose modulators into an
    /// @ref Animation and hand it to @ref Attach.
    class EdgeLightingEffect
    {
    public:
        EdgeLightingEffect();
        ~EdgeLightingEffect() = default;

        /// @brief Initialise all registered renderers.
        /// @returns false if any renderer fails to initialise.
        bool Initialize();

        /// @brief Advance the clock and every attached animation, recompute the
        ///        active config, and propagate updates to renderers.
        /// @param deltaTime Seconds since the last frame.
        /// @details Animations advance by the clock's delta for this frame, so
        ///          a paused clock (@ref GetClock) freezes them. The active
        ///          config is rebuilt as base + all overlays; renderers are
        ///          notified via @c OnConfigChanged only when it actually
        ///          changes.
        void Update(float deltaTime);

        /// @brief Render all active renderers in registration order.
        /// @param viewportWidth  Current framebuffer width in pixels.
        /// @param viewportHeight Current framebuffer height in pixels.
        void Render(int viewportWidth, int viewportHeight);

        /// @brief Replace the base configuration and refresh the active config.
        /// @param config New authored configuration (the base that animations
        ///        overlay). Renderers are notified if the resulting active
        ///        config changes.
        void SetConfig(const Config &config);

        /// @brief The base (authored) configuration.
        /// @returns Const reference to the base @ref Config - the value set by
        ///          @ref SetConfig, safe to read-modify-write without folding
        ///          in animated values.
        const Config &GetConfig() const;

        /// @brief The active (composited) configuration renderers draw.
        /// @returns Const reference to base + all attached-animation overlays,
        ///          as of the last @ref Update / @ref SetConfig.
        const Config &GetActiveConfig() const;

        /// @brief Attach a standalone animation to be advanced and composited
        ///        each @ref Update. Ignores null and duplicates.
        /// @details Sugar for @c Animations().Attach. The animation keeps its
        ///          play state - call @c anim->Play() to start it.
        void Attach(const AnimationPtr &animation);

        /// @brief Detach a previously attached animation (by identity).
        /// @return true if it was attached and removed.
        bool Detach(const AnimationPtr &animation);

        /// @brief The animation manager (attach list + broadcast control).
        AnimationManager &Animations();
        const AnimationManager &Animations() const;

        /// @brief Register a renderer to be updated and rendered each frame.
        /// @param renderer Shared pointer to a @ref BaseRenderer subclass.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// @brief Access the shared clock for play/pause/time control.
        /// @details Modulators outside the effect read its time to stay in
        ///          lockstep with the renderer.
        Clock &GetClock();
        const Clock &GetClock() const;

    private:
        /// @brief Rebuild @c mActiveConfig = base + overlays; notify renderers
        ///        via @c OnConfigChanged only when it actually changed.
        void refreshActiveConfig();

        Config mBaseConfig;   ///< Authored config; what SetConfig sets, animations overlay.
        Config mActiveConfig; ///< Composited config forwarded to renderers each frame.
        Clock mClock;
        AnimationManager mAnimationManager;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EDGE_LIGHTING_H_
