#ifndef _EDGE_LIGHTING_EFFECT_H_
#define _EDGE_LIGHTING_EFFECT_H_

#include "core/config.h"
#include "animation/animation.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>
#include <cstdint>

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

        /// Load an image from disk, trace the outermost contour of its alpha channel,
        /// and set it as the MASK path source.
        /// @param path  Filesystem path to a PNG or other image readable by stb_image.
        /// @return true if the mask was loaded and traced successfully.
        bool SetMaskFromFile(const char *path);

        /// Trace the outermost contour of a raw RGBA pixel buffer and set it as the MASK path source.
        /// @param pixels  4-channel RGBA pixel data.
        /// @param w, h    Image dimensions in pixels.
        /// @return true if foreground was found and traced successfully.
        bool SetMaskFromPixels(const uint8_t *pixels, int w, int h);

    private:
        float computeHeadPos() const;

        Config mConfig;
        float mTime = 0.0f;

        Animation mAnimation;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EFFECT_H_
