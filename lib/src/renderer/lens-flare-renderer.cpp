#include "renderer/lens-flare-renderer.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    bool LensFlareRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link LensFlareRenderer shaders.");
            return false;
        }
        setupGeometry();
        return true;
    }

    void LensFlareRenderer::Update(float, float, const Config &)
    {
    }

    void LensFlareRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.lensFlare.enable)
        {
            return;
        }

        // Premultiplied "over": alpha = brightest channel, so the flare
        // occludes cleanly under its cores and blends additively in the
        // low-brightness ghost wings.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();

        // The sun rides the rect perimeter (same parameter space as
        // segments/arcs); GetSunFragPosition does the perimeter sample, the
        // signed outward-normal offset, and the flip into gl_FragCoord space.
        glm::vec2 resolution(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        glm::vec2 sunPosFrag = GeometryUtils::GetSunFragPosition(config.lensFlare, config.geometry,
                                                                 viewportWidth, viewportHeight);

        mShaderProgram.SetUniform("uResolution", resolution);
        mShaderProgram.SetUniform("uSunPos", sunPosFrag);
        mShaderProgram.SetUniform("uSunColor", config.lensFlare.color);
        mShaderProgram.SetUniform("uIntensity", config.lensFlare.intensity);
        mShaderProgram.SetUniform("uSpread", config.lensFlare.spread);
        mShaderProgram.SetUniform("uGhostSpacing", config.lensFlare.ghostSpacing);
        mShaderProgram.SetUniform("uGhostSize", config.lensFlare.ghostSize);
        mShaderProgram.SetUniform("uGhostOffset", config.lensFlare.ghostOffset);
        mShaderProgram.SetUniform("uGhostColor", config.lensFlare.ghostColor);
        mShaderProgram.SetUniform("uGhostTint", config.lensFlare.ghostTint);
        mShaderProgram.SetUniform("uFlareCenter", config.lensFlare.flareCenter);
        mShaderProgram.SetUniform("uSize", config.lensFlare.size);

        constexpr float TWO_PI = 6.28318530717958647692f;
        mShaderProgram.SetUniform("uRotation", time * config.lensFlare.rotationRate * TWO_PI);
        // Quantise the [0, 1] density into an integer slot count for the
        // shader; the shader needs an integer so `abs(sin(a * N/2))` closes
        // cleanly at 2 PI. MAX_RAY_SLOTS caps the top of the range so
        // extreme densities stay artist-friendly (rays too thin to see).
        constexpr int MAX_RAY_SLOTS = 80;
        float clampedDensity = std::clamp(config.lensFlare.rayDensity, 0.0f, 1.0f);
        int slots = std::max(1, static_cast<int>(std::round(clampedDensity * MAX_RAY_SLOTS)));
        mShaderProgram.SetUniform("uRayDensity", static_cast<float>(slots));

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();

        // Restore the default alpha blend state for any following renderers -
        // the same hand-back every other renderer does. This one is registered
        // last in the demo, so the premultiplied mode it sets above used to
        // leak out of the effect entirely and land on whatever the host drew
        // next.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void LensFlareRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
    }

    bool LensFlareRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::LENS_FLARE_VERT_SRC,
                                       ShaderSource::LENS_FLARE_FRAG_SRC,
                                       "LensFlareRenderer");
        return mShaderProgram.IsValid();
    }

    void LensFlareRenderer::setupGeometry()
    {
        // Static fullscreen NDC quad; shader shapes everything from
        // gl_FragCoord + uniforms, so the geometry never needs a rebuild.
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mVertexArray.SetVertexData(ndc, sizeof(ndc));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

} // namespace EdgeLighting
