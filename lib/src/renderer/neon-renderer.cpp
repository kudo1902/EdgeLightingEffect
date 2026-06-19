#include "renderer/neon-renderer.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    bool NeonRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        rebuildLoopSamples(mCurrentConfig);
        return true;
    }

    void NeonRenderer::Update(float, float, const Config &)
    {
    }

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
            return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        mShaderProgram.Use();

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth);
        mShaderProgram.SetUniform("uIntensity", config.neon.intensity);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uSweepSpeed", config.neon.sweepSpeed);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mShaderProgram.SetUniform("uBlendSpace", static_cast<int>(config.neon.blendSpace));
        int stopCount = std::min(static_cast<int>(config.neon.colorStops.size()),
                                 NeonConfig::MAX_COLOR_STOPS);
        mShaderProgram.SetUniform("uColorStopCount", stopCount);
        mStopPositions.resize(stopCount);
        mStopColors.resize(stopCount);
        for (int i = 0; i < stopCount; ++i)
        {
            mStopPositions[i] = config.neon.colorStops[i].position;
            mStopColors[i] = config.neon.colorStops[i].color;
        }

        if (stopCount > 0)
        {
            mShaderProgram.SetUniform("uColorStopPositions", mStopPositions.data(), stopCount);
            mShaderProgram.SetUniform("uColorStopColors", mStopColors.data(), stopCount);
        }

        int sampleCount = static_cast<int>(mLoopSamples.size());
        mShaderProgram.SetUniform("uSampleCount", sampleCount);
        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);
        if (sampleCount > 0)
        {
            mShaderProgram.SetUniform("uLoopSamples", mLoopSamples.data(), sampleCount);
        }
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mShaderProgram.IsValid())
        {
            setupGeometry(config);
            rebuildLoopSamples(config);
        }
    }

    bool NeonRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        return mShaderProgram.IsValid();
    }

    void NeonRenderer::setupGeometry(const Config &config)
    {
        float margin = 600.0f; // Large margin to avoid hard clipping the glow
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;
        float l = -(halfW + margin);
        float r = halfW + margin;
        float b = -(halfH + margin);
        float t = halfH + margin;

        // clang-format off
        float verts[] = {
            l, t, l, b, r, b,
            l, t, r, b, r, t,
        };
        // clang-format on

        mVertexArray.SetVertexData(verts, sizeof(verts));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void NeonRenderer::rebuildLoopSamples(const Config &config)
    {
        // Evenly spaced points (by arc length) around the rounded-rect perimeter.
        // Drives the additive halo/spill/colour gather in the fragment shader.
        mLoopSamples.resize(NUM_LOOP_SAMPLES);
        for (int i = 0; i < NUM_LOOP_SAMPLES; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(NUM_LOOP_SAMPLES);
            mLoopSamples[i] = GeometryUtils::GetPointOnRectangle(t, config.geometry);
        }

        float w = config.geometry.width;
        float h = config.geometry.height;
        float r = std::max(0.0f, std::min(config.geometry.cornerRadius, std::min(w, h) * 0.5f));
        constexpr float pi = 3.14159265358979323846f;
        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * pi * r;
        mSampleSpacing = perimeter / static_cast<float>(NUM_LOOP_SAMPLES);
    }
}
