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
        ~EdgeLightingEffect();

        /// @brief Initialise all registered renderers.
        /// @returns false if any renderer fails to initialise.
        bool Initialize();

        /// @brief Advance the clock and every attached animation, recompute the
        ///        active config, and propagate updates to renderers.
        /// @param deltaTime Seconds since the last frame.
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
        AnimationManager &Animations();
        const AnimationManager &Animations() const;

        /// @brief Register a renderer to be updated and rendered each frame.
        /// @param renderer Shared pointer to a @ref BaseRenderer subclass.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// @brief Access the shared clock for play/pause/time control.
        Clock &GetClock();
        const Clock &GetClock() const;

    private:
        void refreshActiveConfig();

        Config mBaseConfig;   ///< Authored config - what SetConfig sets.
        Config mActiveConfig; ///< Base + animation overlays - forwarded to renderers.
        Clock mClock;
        std::unique_ptr<AnimationManager> mAnimationManager;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EDGE_LIGHTING_H_
