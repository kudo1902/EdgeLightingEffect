#ifndef _EDGE_LIGHTING_EDGE_LIGHTING_H_
#define _EDGE_LIGHTING_EDGE_LIGHTING_H_

#include "core/config.h"
#include "animation/clock.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>

namespace EdgeLighting
{
    /// @brief Top-level orchestrator for the edge-lighting effect.
    ///
    /// Owns the configuration, the time clock, and a list of renderers.
    /// Callers compose modulators externally and write the resulting values
    /// into a @ref Config before calling @ref SetConfig.
    ///
    /// ## Animation policy
    ///
    /// This class only ticks the clock and forwards its time to renderers.
    /// Parameter modulation (oscillators / easing / sequences) lives outside
    /// in the @ref Modulator family — callers compose them however they like
    /// and write into a @ref Config before calling @ref SetConfig.
    class EdgeLightingEffect
    {
    public:
        EdgeLightingEffect();
        ~EdgeLightingEffect() = default;

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

        /// @brief Current active configuration.
        /// @returns Const reference to the internal @ref Config.
        const Config &GetConfig() const;

        /// @brief Register a renderer to be updated and rendered each frame.
        /// @param renderer Shared pointer to a @ref BaseRenderer subclass.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// @brief Access the shared clock for play/pause/time control.
        /// @details Modulators outside the effect read its time to stay in
        ///          lockstep with the renderer.
        Clock &GetClock();
        const Clock &GetClock() const;

    private:
        Config mConfig;
        Clock mClock;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EDGE_LIGHTING_H_
