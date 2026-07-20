#include "renderer/droplets-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    bool DropletsRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link DropletsRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        return true;
    }

    void DropletsRenderer::Update(float, float, const Config &)
    {
    }

    void DropletsRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.droplets.enable || viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        // Rect centre in framebuffer pixel space (origin at bottom-left, matching
        // gl_FragCoord). Config::geometry uses a top-left origin, hence the flip.
        glm::vec2 rectCenter(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // Premultiplied-alpha "over" - the band feathers into the existing
        // framebuffer at its discard boundary.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        // Fullscreen quad uses an identity MVP; the shader reads gl_FragCoord
        // directly so it does not need a per-viewport projection.
        mShaderProgram.SetUniform("uMVP", glm::mat4(1.0f));
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uRectCenter", rectCenter);
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uViewport", glm::vec2(static_cast<float>(viewportWidth),
                                                         static_cast<float>(viewportHeight)));
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uAmount", config.droplets.amount);
        mShaderProgram.SetUniform("uSpeed", config.droplets.speed);
        mShaderProgram.SetUniform("uLanes", std::max(config.droplets.lanes, 1));
        mShaderProgram.SetUniform("uBandWidth", config.droplets.bandWidth);
        mShaderProgram.SetUniform("uBandOffset", config.droplets.bandOffset);
        mShaderProgram.SetUniform("uTint", config.droplets.tint);
        // The band's side-mask tracks the neon's live glow-side directly -
        // there is no droplet-side duplicate. Change the neon's side (or
        // softness) and the wet region re-masks automatically.
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();

        // Restore the blend state convention the other renderers leave behind.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void DropletsRenderer::OnConfigChanged(const Config &config)
    {
        // Fullscreen NDC quad is size-independent, but keep the hook so any
        // future dependency on geometry rebuilds cleanly.
        (void)mCurrentConfig;
        mCurrentConfig = config;
    }

    bool DropletsRenderer::setupShaders()
    {
        // Reuses the standard neon vertex shader (uMVP + aPos -> vPos) so the
        // pane quad lives in the same rect-local space as the glow quad.
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::DROPLETS_FRAG_SRC,
                                       "DropletsRenderer");
        return mShaderProgram.IsValid();
    }

    void DropletsRenderer::setupGeometry(const Config &)
    {
        // Fullscreen NDC quad - the pane covers the whole viewport and the
        // shader masks it to the band. Identity MVP in Render().
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
