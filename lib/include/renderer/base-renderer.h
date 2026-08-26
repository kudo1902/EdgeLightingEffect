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
        /// @param time       Accumulated wall-clock time in seconds (paused when the animation is paused).
        /// @param config     Current active configuration.
        virtual void Update(float deltaTime, float time, const Config &config) = 0;

        /// Draws the renderer's visual layer.
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
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) = 0;

        /// Called when the configuration changes (e.g. on key press).
        /// @param config  The new configuration to adapt to.
        virtual void OnConfigChanged(const Config &config) = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
