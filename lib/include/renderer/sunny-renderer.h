#ifndef _EDGE_LIGHTING_SUNNY_RENDERER_H_
#define _EDGE_LIGHTING_SUNNY_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    /// Sunlight glints + rays renderer.
    ///
    /// Draws a fullscreen quad whose fragment shader (sunny.frag) paints two
    /// self-lit sun phenomena along the rounded-rect perimeter:
    ///
    ///   * Glints - star-shaped twinkles hugging the edge (grid-hashed cells
    ///     with per-cell fade envelopes, the same technique as the droplets
    ///     renderer's static condensation layer).
    ///   * Rays - soft light shafts fanning outward from the edge, built from
    ///     integer-frequency sines of the polar angle so the pattern is
    ///     seamless around the perimeter.
    ///
    /// Both are weighted by how directly the local edge faces
    /// @c SunnyConfig::sunDirection (from the SDF gradient), so the sun-facing
    /// runs light up and the far side stays in shadow, blending softly round
    /// the corners.
    ///
    /// The pass is purely additive - premultiplied output with zero alpha -
    /// so it adds light on top of the neon layers and occludes nothing.
    ///
    /// Parameters come from @c Config::sunny; geometry from @c Config::geometry.
    class SunnyRenderer : public BaseRenderer
    {
    public:
        SunnyRenderer() = default;
        virtual ~SunnyRenderer() = default;

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
        VertexArray mVertexArray{"SunnyRenderer"}; ///< Fullscreen NDC quad; the shader masks it to the edge shell.
    };
}

#endif // _EDGE_LIGHTING_SUNNY_RENDERER_H_
