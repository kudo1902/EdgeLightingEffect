#include "renderer/neon-renderer.h"
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

    bool NeonRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        rebuildLoopSamples(mCurrentConfig);
        rebuildGradientLUT(mCurrentConfig);
        return true;
    }

    void NeonRenderer::Update(float, float, const Config &)
    {
    }

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
        {
            return;
        }

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

        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);
        int sampleCount = static_cast<int>(mLoopSamples.size());
        if (sampleCount > 0)
        {
            mShaderProgram.SetUniform("uLoopSamples", mLoopSamples.data(), sampleCount);
        }

        // Bind the precomputed gradient LUT to texture unit 0. The shader
        // pulls per-sample colour from this in a single texture() call.
        mGradientLUT.Bind(0);
        mShaderProgram.SetUniform("uGradientLUT", 0);

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
            rebuildGradientLUT(config);
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

        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * PI * r;
        mSampleSpacing = perimeter / static_cast<float>(NUM_LOOP_SAMPLES);
    }

    void NeonRenderer::rebuildGradientLUT(const Config &config)
    {
        // Bake the entire colour ring once on CPU; the shader then becomes
        // colour-stop-agnostic. Keeps HSV-vs-RGB blend cost off the GPU hot path.
        mLUTScratch.resize(GRADIENT_LUT_SIZE * 4);
        for (int i = 0; i < GRADIENT_LUT_SIZE; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(GRADIENT_LUT_SIZE);
            glm::vec3 c = ColorUtils::SampleStops(t, config.neon.colorStops, config.neon.blendSpace);
            mLUTScratch[i * 4 + 0] = c.r;
            mLUTScratch[i * 4 + 1] = c.g;
            mLUTScratch[i * 4 + 2] = c.b;
            mLUTScratch[i * 4 + 3] = 1.0f;
        }
        mGradientLUT.SetData(mLUTScratch.data(), GRADIENT_LUT_SIZE, GL_RGBA32F, GL_RGBA);
        mGradientLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT);
    }
}
