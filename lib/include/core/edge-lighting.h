#ifndef _EDGE_LIGHTING_EFFECT_H_
#define _EDGE_LIGHTING_EFFECT_H_

#include "core/config.h"
#include "animation/animation.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>

namespace EdgeLighting
{
    /// Top-level orchestrator that owns the configuration, animation state,
    /// and a list of renderers. Drives the per-frame update/render loop.
    class EdgeLightingEffect
    {
    public:
        EdgeLightingEffect();
        ~EdgeLightingEffect() = default;

        /// Initialises all registered renderers.
        /// @return false if any renderer fails to initialise.
        bool Initialize();

        /// Advances animation time and propagate updates to renderers.
        /// @param deltaTime  Seconds since the last frame.
        void Update(float deltaTime);

        /// Renders all active renderers in registration order.
        /// @param viewportWidth   Current framebuffer width.
        /// @param viewportHeight  Current framebuffer height.
        void Render(int viewportWidth, int viewportHeight);

        /// Replaces the active configuration and notifies all renderers.
        void SetConfig(const Config &config);

        /// Returns a const reference to the current configuration.
        const Config &GetConfig() const;

        /// Registers a renderer to be updated and rendered each frame.
        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);

        /// Provides access to the shared animation controller.
        Animation &GetAnimation();

    private:
        Config mConfig;
        float mTime = 0.0f;

        Animation mAnimation;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EFFECT_H_
