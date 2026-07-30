#include "renderer/lens-flare-optimized-renderer.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    bool LensFlareOptimizedRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link LensFlareOptimizedRenderer shaders.");
            return false;
        }
        setupGeometry();
        return true;
    }

    void LensFlareOptimizedRenderer::Update(float, float, const Config &)
    {
    }

    void LensFlareOptimizedRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.optimizedLensFlare.enable)
        {
            return;
        }

        float scale = config.optimizedLensFlare.resolutionScale;
        int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // --- Pass 1: render the flare into the scaled FBO ---
        mScaledBuffer.Resize(bufW, bufH);
        mScaledBuffer.Bind();

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Premultiplied "over" into the transparent FBO: the shader writes
        // premultiplied colour + coverage alpha, ready to composite over the
        // backbuffer in Pass 2. Same blend the base LensFlareRenderer uses.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mFlareShader.Use();

        // The shader normalises everything by uResolution, so it is scale
        // invariant: feeding it the FBO size (not the viewport size) reproduces
        // the identical flare at lower resolution. The sun rides the perimeter
        // in full-res gl_FragCoord; scale it into the FBO by the same factor.
        glm::vec2 resolution(static_cast<float>(bufW), static_cast<float>(bufH));
        glm::vec2 sunPosFrag = GeometryUtils::GetSunFragPosition(config.lensFlare, config.geometry,
                                                                 viewportWidth, viewportHeight);
        sunPosFrag *= scale;

        mFlareShader.SetUniform("uResolution", resolution);
        mFlareShader.SetUniform("uSunPos", sunPosFrag);
        mFlareShader.SetUniform("uSunColor", config.lensFlare.color);
        mFlareShader.SetUniform("uIntensity", config.lensFlare.intensity);
        mFlareShader.SetUniform("uSpread", config.lensFlare.spread);
        mFlareShader.SetUniform("uGhostSpacing", config.lensFlare.ghostSpacing);
        mFlareShader.SetUniform("uGhostSize", config.lensFlare.ghostSize);
        mFlareShader.SetUniform("uGhostOffset", config.lensFlare.ghostOffset);
        mFlareShader.SetUniform("uGhostColor", config.lensFlare.ghostColor);
        mFlareShader.SetUniform("uGhostTint", config.lensFlare.ghostTint);
        mFlareShader.SetUniform("uFlareCenter", config.lensFlare.flareCenter);
        mFlareShader.SetUniform("uSize", config.lensFlare.size);

        constexpr float TWO_PI = 6.28318530717958647692f;
        mFlareShader.SetUniform("uRotation", time * config.lensFlare.rotationRate * TWO_PI);
        // Same [0,1] density -> integer slot quantisation as the base renderer
        // (see LensFlareRenderer::Render for the recipe): the shader needs an
        // integer so abs(sin(a * N/2)) closes cleanly at 2 PI.
        constexpr int MAX_RAY_SLOTS = 80;
        float clampedDensity = std::clamp(config.lensFlare.rayDensity, 0.0f, 1.0f);
        int slots = std::max(1, static_cast<int>(std::round(clampedDensity * MAX_RAY_SLOTS)));
        mFlareShader.SetUniform("uRayDensity", static_cast<float>(slots));

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mFlareShader.Unuse();

        // --- Pass 2: bilinear composite of the scaled FBO onto the backbuffer.
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read composited over whatever's already on
        // the backbuffer. (Reuses the neon blit shader - identical job.)
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mBlitShader.Use();
        glm::mat4 identity(1.0f);
        mBlitShader.SetUniform("uMVP", identity);

        // Debug toggle: nearest neighbour shows the raw scaled FBO pixels.
        GLuint texId = mScaledBuffer.GetTextureId();
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        config.optimizedLensFlare.showScaled ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        config.optimizedLensFlare.showScaled ? GL_NEAREST : GL_LINEAR);

        mScaledBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mBlitShader.Unuse();

        // Restore the default alpha blend state for any following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void LensFlareOptimizedRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
    }

    bool LensFlareOptimizedRenderer::setupShaders()
    {
        // Pass 1 reuses the full-res lens-flare shader unchanged (it is
        // resolution-agnostic). Pass 2 reuses the neon blit shader - a plain
        // premultiplied texture composite of the scaled FBO.
        mFlareShader = ShaderProgram(ShaderSource::LENS_FLARE_VERT_SRC,
                                     ShaderSource::LENS_FLARE_FRAG_SRC,
                                     "LensFlareOptimized");
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "LensFlareOptimized.Blit");
        return mFlareShader.IsValid() && mBlitShader.IsValid();
    }

    void LensFlareOptimizedRenderer::setupGeometry()
    {
        // Static fullscreen NDC quad, shared by both passes: Pass 1's flare
        // shader shapes everything from gl_FragCoord + uniforms, and Pass 2's
        // blit reads it back as an NDC-derived UV under identity MVP. Both bind
        // position at attribute location 0, so one buffer serves both.
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
