#include "renderer/lens-flare-renderer.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"

namespace EdgeLighting
{
    namespace
    {
        /// Same sentinel + resolver as neon-renderer.cpp: a disabled Cutoff
        /// collapses to a huge pixel size so the shader's smoothstep / discard
        /// naturally no-ops. Duplicated here to keep the renderer self-contained.
        constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;
        inline float GetCutoffSize(const Cutoff &c)
        {
            return c.enable ? c.size : CUTOFF_DISABLED_SIZE;
        }
    }

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

    void LensFlareRenderer::Render(int viewportWidth, int viewportHeight, float, const Config &config)
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

        // The sun rides the rect perimeter: sample the shared helper (same
        // parameter space as segments/arcs), convert local -> app pixel space
        // (rect top-left origin, y-down), then flip Y for gl_FragCoord.
        glm::vec2 local = GeometryUtils::GetPointOnRectangle(config.lensFlare.perimeterPosition,
                                                             config.geometry);
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;
        glm::vec2 sunApp(config.geometry.position.x + local.x + halfW,
                         config.geometry.position.y + (halfH - local.y));

        glm::vec2 resolution(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        glm::vec2 sunPosFrag(sunApp.x, resolution.y - sunApp.y);

        mShaderProgram.SetUniform("uResolution", resolution);
        mShaderProgram.SetUniform("uSunPos", sunPosFrag);
        mShaderProgram.SetUniform("uSunColor", config.lensFlare.color);
        mShaderProgram.SetUniform("uIntensity", config.lensFlare.intensity);
        mShaderProgram.SetUniform("uSpread", config.lensFlare.spread);
        mShaderProgram.SetUniform("uSize", config.lensFlare.size);

        // Rect + cutoffs (reused from NeonConfig so the two layers stay in
        // lockstep). Rect centre in gl_FragCoord space (y-up), same convention
        // as sunPosFrag above.
        glm::vec2 rectCenter(config.geometry.position.x + halfW,
                             resolution.y - config.geometry.position.y - halfH);
        mShaderProgram.SetUniform("uRectCenter", rectCenter);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mShaderProgram.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mShaderProgram.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mShaderProgram.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();
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
