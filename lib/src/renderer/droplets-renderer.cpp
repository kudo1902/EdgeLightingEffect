#include "renderer/droplets-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

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
        // Seed the capture as a 1x1 opaque-black texture so the sampler is
        // always "complete" - overlay mode never touches it but still binds
        // it, and some drivers sample an incomplete texture as black without
        // signalling an error while others emit spam.
        const unsigned char black[4] = {0, 0, 0, 255};
        mBackgroundCapture.SetData(black, 1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mBackgroundCapture.SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
        mCaptureWidth = 1;
        mCaptureHeight = 1;
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

        // Snapshot everything rendered so far this frame (background + neon
        // layers) - the pane shader either frosts (WET_GLASS) or refracts
        // (WET_GLASS, LENS) this capture. Mips are only needed for the
        // WET_GLASS frost blur; LENS samples LOD 0 only. HIGHLIGHTS mode
        // needs no capture at all - drops become translucent highlights that
        // never read the framebuffer.
        if (config.droplets.mode != DropletsMode::HIGHLIGHTS)
        {
            const bool wantMips = config.droplets.mode == DropletsMode::WET_GLASS &&
                                  config.droplets.blur > 0.0f;
            captureBackground(viewportWidth, viewportHeight, wantMips);
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        // Rect centre in framebuffer pixel space (origin at bottom-left, matching
        // gl_FragCoord). Config::geometry uses a top-left origin, hence the flip.
        glm::vec2 rectCenter(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // Premultiplied-alpha "over" - side-masked pane feathers into the
        // existing framebuffer at the discard boundary.
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
        mShaderProgram.SetUniform("uScale", config.droplets.scale);
        mShaderProgram.SetUniform("uDistortion", config.droplets.distortion);
        mShaderProgram.SetUniform("uBlur", config.droplets.blur);
        mShaderProgram.SetUniform("uTint", config.droplets.tint);
        // The pane's side-mask tracks the neon's live glow-side directly -
        // there is no droplet-side duplicate. Change the neon's side (or
        // softness) and the wet region re-masks automatically.
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mShaderProgram.SetUniform("uMode", static_cast<int>(config.droplets.mode));

        mBackgroundCapture.Bind(0);
        mShaderProgram.SetUniform("uBackground", 0);

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
        // Fullscreen NDC quad - the pane now covers the whole viewport and
        // the shader masks it by @c uGlowSide. Identity MVP in Render().
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on

        mVertexArray.SetVertexData(ndc, sizeof(ndc));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void DropletsRenderer::captureBackground(int viewportWidth, int viewportHeight, bool wantMips)
    {
        // The whole viewport is captured (not just the rect) because both the
        // refraction offset and the frost LOD sample outside the pane bounds;
        // a partial copy would leave garbage texels at the pane edge.
        if (viewportWidth != mCaptureWidth || viewportHeight != mCaptureHeight)
        {
            mBackgroundCapture.SetData(nullptr, viewportWidth, viewportHeight,
                                       GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            mCaptureWidth = viewportWidth;
            mCaptureHeight = viewportHeight;
        }

        mBackgroundCapture.Bind(0);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, viewportWidth, viewportHeight);

        if (wantMips)
        {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        // textureLod ignores LODs above 0 under plain GL_LINEAR, so the
        // no-blur path can skip mip generation entirely.
        mBackgroundCapture.SetParams(wantMips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR,
                                     GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
    }
}
