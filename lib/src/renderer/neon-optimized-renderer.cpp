#include "renderer/neon-optimized-renderer.h"
#include "util/color-utils.h"
#include "util/constants.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------

    bool NeonOptimizedRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonOptimizedRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        rebuildLoopSamples(mCurrentConfig);
        rebuildGradientLUT(mCurrentConfig);
        return true;
    }

    void NeonOptimizedRenderer::Update(float, float, const Config &)
    {
    }

    void NeonOptimizedRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.optimizedNeon.enable)
        {
            return;
        }

        float scale = config.optimizedNeon.resolutionScale;
        int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // --- Pass 1: render neon to scaled FBO ---
        mHalfResBuffer.Resize(bufW, bufH);
        mHalfResBuffer.Bind();

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        mNeonShader.Use();

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(bufW), 0.0f, static_cast<float>(bufH), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        // Scale center to FBO coordinates
        center.x *= scale;
        center.y *= scale;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        // Scale geometry to FBO space
        glm::vec2 rectSizeScaled(config.geometry.width * scale, config.geometry.height * scale);

        mNeonShader.SetUniform("uMVP", mvp);
        mNeonShader.SetUniform("uRectSize", rectSizeScaled);
        mNeonShader.SetUniform("uCornerRadius", config.geometry.cornerRadius * scale);
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth);
        mNeonShader.SetUniform("uIntensity", config.neon.intensity);
        mNeonShader.SetUniform("uTime", time);
        mNeonShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mNeonShader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mNeonShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mNeonShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mNeonShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        mNeonShader.SetUniform("uSegmentPosition", config.neon.segmentPosition);
        mNeonShader.SetUniform("uSegmentLength", config.neon.segmentLength);
        mNeonShader.SetUniform("uSegmentBoost", config.neon.segmentBoost);
        mNeonShader.SetUniform("uArcStart", config.neon.arcStart);
        mNeonShader.SetUniform("uArcLength", config.neon.arcLength);

        mNeonShader.SetUniform("uNumSamples", config.optimizedNeon.numSamples);
        mNeonShader.SetUniform("uSampleSpacing", mSampleSpacing);
        int sampleCount = static_cast<int>(mLoopSamples.size());
        if (sampleCount > 0)
        {
            mNeonShader.SetUniform("uLoopSamples", mLoopSamples.data(), sampleCount);
        }

        mGradientLUT.Bind(0);
        mNeonShader.SetUniform("uGradientLUT", 0);

        mNeonVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mNeonShader.Unuse();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 2: blit half-res FBO to full-res backbuffer ---
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);

        glDisable(GL_BLEND);

        mBlitShader.Use();

        // Identity MVP — blit quad is in NDC [-1, 1]
        glm::mat4 identity(1.0f);
        mBlitShader.SetUniform("uMVP", identity);

        // Debug toggle: nearest neighbour shows the raw half-res pixels.
        GLuint texId = mHalfResBuffer.GetTextureId();
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        config.optimizedNeon.showHalfRes ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        config.optimizedNeon.showHalfRes ? GL_NEAREST : GL_LINEAR);

        mHalfResBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mBlitShader.Unuse();
    }

    void NeonOptimizedRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mNeonShader.IsValid())
        {
            setupGeometry(config);
            rebuildLoopSamples(config);
            rebuildGradientLUT(config);
        }
    }

    bool NeonOptimizedRenderer::setupShaders()
    {
        mNeonShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_OPTIMIZED_FRAG_SRC,
                                    "NeonOptimized");

        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonBlit");
        return mNeonShader.IsValid() && mBlitShader.IsValid();
    }

    void NeonOptimizedRenderer::setupGeometry(const Config &config)
    {
        float scale = config.optimizedNeon.resolutionScale;

        // --- Scaled glow quad (pass 1) ---
        {
            float margin = 600.0f * scale;
            float halfW = config.geometry.width * 0.5f * scale;
            float halfH = config.geometry.height * 0.5f * scale;
            float l = -(halfW + margin);
            float r = halfW + margin;
            float b = -(halfH + margin);
            float t = halfH + margin;

            float verts[] = {
                l, t, l, b, r, b,
                l, t, r, b, r, t,
            };

            mNeonVertexArray.SetVertexData(verts, sizeof(verts));
            mNeonVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        }

        // --- Fullscreen NDC quad (pass 2, identity MVP) ---
        {
            float verts[] = {
                -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f, -1.0f,
                -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  1.0f,
            };

            mBlitVertexArray.SetVertexData(verts, sizeof(verts));
            mBlitVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        }
    }

    void NeonOptimizedRenderer::rebuildLoopSamples(const Config &config)
    {
        float scale = config.optimizedNeon.resolutionScale;
        int n = std::min(config.optimizedNeon.numSamples, MAX_LOOP_SAMPLES);

        mLoopSamples.resize(MAX_LOOP_SAMPLES);
        for (int i = 0; i < MAX_LOOP_SAMPLES; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(n);
            mLoopSamples[i] = GeometryUtils::GetPointOnRectangle(t, config.geometry) * scale;
        }

        float w = config.geometry.width;
        float h = config.geometry.height;
        float r = std::max(0.0f, std::min(config.geometry.cornerRadius, std::min(w, h) * 0.5f));

        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * PI * r;
        mSampleSpacing = (perimeter * scale) / static_cast<float>(n);
    }

    void NeonOptimizedRenderer::rebuildGradientLUT(const Config &config)
    {
        int lutSize = std::max(config.optimizedNeon.gradientLutSize, 4);
        mLUTScratch.resize(lutSize * 4);
        for (int i = 0; i < lutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(lutSize);
            glm::vec3 c = ColorUtils::SampleStops(t, config.neon.colorStops, config.neon.blendSpace);
            mLUTScratch[i * 4 + 0] = c.r;
            mLUTScratch[i * 4 + 1] = c.g;
            mLUTScratch[i * 4 + 2] = c.b;
            mLUTScratch[i * 4 + 3] = 1.0f;
        }

        std::vector<unsigned char> lutBytes(lutSize * 4);
        for (int i = 0; i < lutSize * 4; ++i)
        {
            lutBytes[i] = static_cast<unsigned char>(
                std::clamp(mLUTScratch[i] * 255.0f, 0.0f, 255.0f));
        }

        mGradientLUT.SetData(lutBytes.data(), lutSize, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mGradientLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
    }

} // namespace EdgeLighting
