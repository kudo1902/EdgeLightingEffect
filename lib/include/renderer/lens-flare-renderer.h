#ifndef _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_
#define _EDGE_LIGHTING_LENS_FLARE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Draws a sun + hex-aperture lens flare (rays, chromatic ghosts) as a
    /// single fullscreen premultiplied-alpha pass. The sun rides the rect
    /// perimeter (see @c LensFlareConfig::perimeterPosition), so it moves with
    /// the geometry and animates from the same parameter space as neon
    /// segments / arcs.
    ///
    /// @see LensFlareConfig for configuration options.
    class LensFlareRenderer : public BaseRenderer
    {
    public:
        LensFlareRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry();

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        VertexArray mVertexArray{"LensFlare"};
        /// Ghost spin angle in radians, accumulated per-frame from the clock
        /// delta so changing the rate or toggling follow never applies a rate
        /// to past time (which would jump the trail), and so it freezes with
        /// the clock on pause just like the rays. Held at 0 while the ghosts
        /// are not following, so enabling rotation always starts at the offset.
        float mGhostSpin = 0.0f;
        /// Previous clock time seen in Update, for the clock-delta above.
        float mPrevClockTime = 0.0f;
    };
}

#endif
