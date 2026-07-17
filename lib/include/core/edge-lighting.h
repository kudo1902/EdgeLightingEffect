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
        /// @returns false if any renderer fails to initialise.
        bool Initialize();

        /// @brief Advance animation time and propagate updates to renderers.
        /// @param deltaTime Seconds since the last frame.
        void Update(float deltaTime);

        /// @brief Render all active renderers in registration order.
        /// @param viewportWidth  Current framebuffer width in pixels.
        /// @param viewportHeight Current framebuffer height in pixels.
        void Render(int viewportWidth, int viewportHeight);

        /// @brief Replace the active configuration and notify all renderers.
        /// @param config New configuration to apply.
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
