#include "renderer/neon-multi-pass-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    bool NeonMultiPassRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonMultiPassRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        return true;
    }

    void NeonMultiPassRenderer::Update(float, float, const Config &)
    {
    }

    void NeonMultiPassRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.multipassNeon.enable)
        {
            return;
        }

        // All three FBOs share the same size as the viewport. The blur kernel
        // is sized in pixels via uBlurRadius so an FBO supersample factor is a
        // separate concern — keep them at 1x for now.
        if (!mBarBuffer.Resize(viewportWidth, viewportHeight) ||
            !mBlurHBuffer.Resize(viewportWidth, viewportHeight) ||
            !mBlurVBuffer.Resize(viewportWidth, viewportHeight))
        {
            return;
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                    0.0f, static_cast<float>(viewportHeight),
                                    -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        glm::vec2 texelSize(1.0f / static_cast<float>(viewportWidth),
                            1.0f / static_cast<float>(viewportHeight));

        // ===================================================================
        // Pass 1 — bake the bar (premultiplied colour) into mBarBuffer.
        // ===================================================================
        mBarBuffer.Bind();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_BLEND);

        mBarShader.Use();
        mBarShader.SetUniform("uMVP", mvp);
        mBarShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBarShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mBarShader.SetUniform("uTime", time);
        mBarShader.SetUniform("uHueRotationRate", config.multipassNeon.hueRotationRate);
        mBarShader.SetUniform("uLineWidth", config.multipassNeon.lineWidth);
        mBarShader.SetUniform("uBlendSpace", static_cast<int>(config.multipassNeon.blendSpace));
        mBarShader.SetUniform("uFullGradient", config.multipassNeon.showFullGradient);

        int stopCount = std::min(static_cast<int>(config.multipassNeon.colorStops.size()),
                                 MultiPassNeonConfig::MAX_COLOR_STOPS);
        mBarShader.SetUniform("uColorStopCount", stopCount);
        mStopPositions.resize(stopCount);
        mStopColors.resize(stopCount);
        for (int i = 0; i < stopCount; ++i)
        {
            mStopPositions[i] = config.multipassNeon.colorStops[i].position;
            mStopColors[i] = config.multipassNeon.colorStops[i].color;
        }
        if (stopCount > 0)
        {
            mBarShader.SetUniform("uColorStopPositions", mStopPositions.data(), stopCount);
            mBarShader.SetUniform("uColorStopColors", mStopColors.data(), stopCount);
        }

        // In full-gradient debug mode the bar shader writes everywhere — use the
        // wide quad so we cover the whole rect.
        if (config.multipassNeon.showFullGradient)
        {
            mGlowVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }
        else
        {
            mBarVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }

        // ===================================================================
        // Pass 2 — horizontal Gaussian blur. Reads mBarBuffer, writes mBlurH.
        // ===================================================================
        mBlurHBuffer.Bind();
        glClear(GL_COLOR_BUFFER_BIT);

        mBlurShader.Use();
        mBlurShader.SetUniform("uMVP", mvp);
        mBlurShader.SetUniform("uTexelSize", texelSize);
        mBlurShader.SetUniform("uBlurRadius", config.multipassNeon.glowRadius);
        mBlurShader.SetUniform("uDirection", glm::vec2(1.0f, 0.0f));
        mBarBuffer.BindTexture(0);
        mBlurShader.SetUniform("uSource", 0);
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, 6);

        // ===================================================================
        // Pass 3 — vertical Gaussian blur. Reads mBlurH, writes mBlurV.
        // ===================================================================
        mBlurVBuffer.Bind();
        glClear(GL_COLOR_BUFFER_BIT);

        mBlurShader.SetUniform("uDirection", glm::vec2(0.0f, 1.0f));
        mBlurHBuffer.BindTexture(0);
        mBlurShader.SetUniform("uSource", 0);
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, 6);

        // ===================================================================
        // Pass 4 — composite to screen.
        // ===================================================================
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);
        // Premultiplied-alpha "over" so the composite blends over background
        // objects (core occludes, halo/bloom add, surround transparent) rather
        // than only adding light. The composite shader emits premultiplied RGBA.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mCompositeShader.Use();
        mCompositeShader.SetUniform("uMVP", mvp);
        mCompositeShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mCompositeShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mCompositeShader.SetUniform("uLineWidth", config.multipassNeon.lineWidth);
        mCompositeShader.SetUniform("uIntensity", config.multipassNeon.intensity);
        mCompositeShader.SetUniform("uTime", time);
        mCompositeShader.SetUniform("uGlowRadius", config.multipassNeon.glowRadius);
        mCompositeShader.SetUniform("uBloomStrength", config.multipassNeon.bloomStrength);
        mCompositeShader.SetUniform("uGlowSide", static_cast<int>(config.multipassNeon.glowSide));
        mCompositeShader.SetUniform("uGlowSideSoftness", config.multipassNeon.glowSideSoftness);
        mCompositeShader.SetUniform("uViewportSize",
                                    glm::vec2(viewportWidth, viewportHeight));

        // Debug toggles: showPerimeterGradient/showFullGradient → display the
        // baked bar texture directly; otherwise normal composite.
        bool showGradient = config.multipassNeon.showPerimeterGradient ||
                            config.multipassNeon.showFullGradient;
        mCompositeShader.SetUniform("uShowGradient", showGradient);
        mCompositeShader.SetUniform("uShowBlurred", false);

        mBarBuffer.BindTexture(0);
        mBlurVBuffer.BindTexture(1);
        mCompositeShader.SetUniform("uBar", 0);
        mCompositeShader.SetUniform("uHalo", 1);

        mGlowVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mCompositeShader.Unuse();
    }

    void NeonMultiPassRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mBarShader.IsValid())
        {
            setupGeometry(config);
        }
    }

    bool NeonMultiPassRenderer::setupShaders()
    {
        mBarShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                   ShaderSource::NEON_GRADIENT_FRAG_SRC,
                                   "NeonMultiPass.Bar");
        mBlurShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLUR_FRAG_SRC,
                                    "NeonMultiPass.Blur");
        mCompositeShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::NEON_GLOW_FRAG_SRC,
                                         "NeonMultiPass.Composite");
        return mBarShader.IsValid() && mBlurShader.IsValid() && mCompositeShader.IsValid();
    }

    void NeonMultiPassRenderer::setupGeometry(const Config &config)
    {
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;

        // --- Bar quad: tight around the perimeter ---
        // Pass 1 only writes within lineHalf + BAR_FADE_MARGIN of the edge,
        // so a quad sized to (rect + ~max line half width + a few pixels of
        // safety) covers everything and saves ~90% of Pass 1 fragment work.
        constexpr float BAR_MARGIN = 16.0f;
        {
            float l = -(halfW + BAR_MARGIN);
            float r = (halfW + BAR_MARGIN);
            float b = -(halfH + BAR_MARGIN);
            float t = (halfH + BAR_MARGIN);

            // clang-format off
            float verts[] = {
                l, t, l, b, r, b,
                l, t, r, b, r, t,
            };
            // clang-format on

            mBarVertexArray.SetVertexData(verts, sizeof(verts));
            mBarVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        }

        // --- Glow quad: wide enough to cover the blur reach for Pass 2-4. ---
        constexpr float GLOW_MARGIN = 600.0f;
        {
            float l = -(halfW + GLOW_MARGIN);
            float r = (halfW + GLOW_MARGIN);
            float b = -(halfH + GLOW_MARGIN);
            float t = (halfH + GLOW_MARGIN);

            // clang-format off
            float verts[] = {
                l, t, l, b, r, b,
                l, t, r, b, r, t,
            };
            // clang-format on

            mGlowVertexArray.SetVertexData(verts, sizeof(verts));
            mGlowVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        }
    }
}
