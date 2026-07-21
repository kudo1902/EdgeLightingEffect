#include "renderer/sunny-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    bool SunnyRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link SunnyRenderer shaders.");
            return false;
        }
        setupGeometry();
        return true;
    }

    void SunnyRenderer::Update(float, float, const Config &)
    {
    }

    void SunnyRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.sunny.enable || viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        // Rect centre in framebuffer pixel space (origin at bottom-left, matching
        // gl_FragCoord). Config::geometry uses a top-left origin, hence the flip.
        glm::vec2 rectCenter(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // The shader outputs premultiplied colour with zero alpha, so this
        // "over" blend degenerates to pure addition - sunlight adds to the
        // layers below and occludes nothing.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        // Fullscreen quad uses an identity MVP; the shader reads gl_FragCoord
        // directly so it does not need a per-viewport projection.
        mShaderProgram.SetUniform("uMVP", glm::mat4(1.0f));
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uRectCenter", rectCenter);
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uAmount", config.sunny.amount);
        mShaderProgram.SetUniform("uSpeed", config.sunny.speed);
        mShaderProgram.SetUniform("uLanes", std::max(config.sunny.lanes, 1));
        mShaderProgram.SetUniform("uRayStrength", config.sunny.rayStrength);
        mShaderProgram.SetUniform("uBandWidth", config.sunny.bandWidth);
        mShaderProgram.SetUniform("uBandOffset", config.sunny.bandOffset);
        mShaderProgram.SetUniform("uTint", config.sunny.tint);
        // The band's side-mask tracks the neon's live glow-side directly,
        // matching the droplets renderer - change the neon's side (or
        // softness) and the sunlit region re-masks automatically.
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();

        // Restore the blend state convention the other renderers leave behind.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SunnyRenderer::OnConfigChanged(const Config &config)
    {
        // Fullscreen NDC quad is size-independent, but keep the hook so any
        // future dependency on geometry rebuilds cleanly.
        mCurrentConfig = config;
    }

    bool SunnyRenderer::setupShaders()
    {
        // Reuses the standard neon vertex shader (uMVP + aPos -> vPos) so the
        // quad lives in the same space as the glow and droplets quads.
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::SUNNY_FRAG_SRC,
                                       "SunnyRenderer");
        return mShaderProgram.IsValid();
    }

    void SunnyRenderer::setupGeometry()
    {
        // Fullscreen NDC quad - the shader masks it to a shell around the
        // rect edge. Identity MVP in Render().
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on

        mVertexArray.SetVertexData(ndc, sizeof(ndc));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }
}
