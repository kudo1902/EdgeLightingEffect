#include "renderer/snowy-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    bool SnowyRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link SnowyRenderer shaders.");
            return false;
        }
        setupGeometry();
        return true;
    }

    void SnowyRenderer::Update(float, float, const Config &)
    {
    }

    void SnowyRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.snowy.enable || viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        // Rect centre in framebuffer pixel space (origin at bottom-left, matching
        // gl_FragCoord). Config::geometry uses a top-left origin, hence the flip.
        glm::vec2 rectCenter(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // Premultiplied-alpha "over" - snow occludes what's behind it, the
        // band feathers into the framebuffer at its discard boundary.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", glm::mat4(1.0f));
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uRectCenter", rectCenter);
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uAmount", config.snowy.amount);
        mShaderProgram.SetUniform("uFallSpeed", config.snowy.fallSpeed);
        mShaderProgram.SetUniform("uLanes", std::max(config.snowy.lanes, 1));
        mShaderProgram.SetUniform("uDensity", std::clamp(config.snowy.density, 1, 3));
        mShaderProgram.SetUniform("uBandWidth", config.snowy.bandWidth);
        mShaderProgram.SetUniform("uBandOffset", config.snowy.bandOffset);
        mShaderProgram.SetUniform("uTint", config.snowy.tint);
        // Side-mask tracks the neon's live glow-side, matching droplets.
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SnowyRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
    }

    bool SnowyRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::SNOWY_FRAG_SRC,
                                       "SnowyRenderer");
        return mShaderProgram.IsValid();
    }

    void SnowyRenderer::setupGeometry()
    {
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
