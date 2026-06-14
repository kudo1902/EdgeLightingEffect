#ifndef _EDGE_LIGHTING_BASE_RENDERER_H_
#define _EDGE_LIGHTING_BASE_RENDERER_H_

#include "core/config.h"

namespace EdgeLighting
{

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
        /// @param progress   Animation progress in [0, 1] (resets on loop).
        /// @param headPos    Effective head position along the path [0, 1], already mapped through start/end.
        /// @param time       Accumulated wall-clock time in seconds.
        /// @param config     Current active configuration.
        virtual void Update(float deltaTime, float progress, float headPos, float time, const Config &config) = 0;

        /// Draws the renderer's visual layer.
        /// @param viewportWidth  Current framebuffer width.
        /// @param viewportHeight Current framebuffer height.
        /// @param progress   Animation progress in [0, 1].
        /// @param headPos    Effective head position along the path [0, 1], already mapped through start/end.
        /// @param time       Accumulated wall-clock time in seconds.
        /// @param config     Current active configuration.
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float headPos, float time, const Config &config) = 0;

        /// Called when the configuration changes (e.g. on key press).
        /// @param config  The new configuration to adapt to.
        virtual void OnConfigChanged(const Config &config) = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
