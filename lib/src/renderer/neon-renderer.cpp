#include "renderer/neon-renderer.h"
#include "renderer/neon-tuning.h"
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
        // rebuildLoopSamples must run before setupGeometry: the quad size
        // depends on mSampleSpacing (computed in rebuildLoopSamples).
        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        rebuildGradientLUT(mCurrentConfig);

        // Static fullscreen NDC quad for the opaque-mode black fill (identity
        // MVP; the fill shader derives its shape from gl_FragCoord, not aPos).
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mFullVertexArray.SetVertexData(ndc, sizeof(ndc));
        mFullVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
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

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a). Used
        // for both the opaque black fill and the neon, so the neon composites
        // cleanly over the black. (Blending stays ON the whole time — toggling
        // GL_BLEND mid-draw is a common cross-driver footgun on mobile GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Opaque-mode black background pass -----------------------------
        // A fullscreen NDC quad (identity MVP); the fragment shader shapes the
        // black coverage from an analytic rounded-box SDF read off gl_FragCoord
        // (highp — exact on Mali/Tizen):
        //   BOTH    -> black everywhere (whole viewport opaque).
        //   INSIDE  -> black only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        if (config.neon.opaque)
        {
            float softEdge = std::max(config.neon.glowSideSoftness,
                                      static_cast<float>(SIDE_SOFT_EPSILON));
            mBlackRectShader.Use();
            mBlackRectShader.SetUniform("uMVP", glm::mat4(1.0f));
            mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
            mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mBlackRectShader.SetUniform("uRectCenter", center);
            mBlackRectShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
            mBlackRectShader.SetUniform("uSoftEdge", softEdge);
            mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mBlackRectShader.Unuse();
        }

        // Pass 2 (opaque) / only pass (transparent): the neon gather on the
        // tight glow quad, in both modes.
        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth);
        mShaderProgram.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mShaderProgram.SetUniform("uIntensity", config.neon.intensity);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mShaderProgram.SetUniform("uSegmentPosition", config.neon.segmentPosition);
        mShaderProgram.SetUniform("uSegmentLength", config.neon.segmentLength);
        mShaderProgram.SetUniform("uSegmentBoost", config.neon.segmentBoost);
        mShaderProgram.SetUniform("uArcStart", config.neon.arcStart);
        mShaderProgram.SetUniform("uArcLength", config.neon.arcLength);

        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);

        // Loop sample positions come from a data texture (unit 1) that the shader
        // texelFetches, instead of a uniform vec2[] array (see neon.frag).
        mLoopSamplesTex.Bind(1);
        mShaderProgram.SetUniform("uLoopSamplesTex", 1);
        mShaderProgram.SetUniform("uSampleMaxCoord", mSampleMaxCoord);

        // Bind the precomputed gradient LUT to texture unit 0. The shader
        // pulls per-sample colour from this in a single texture() call.
        mGradientLUT.Bind(0);
        mShaderProgram.SetUniform("uGradientLUT", 0);
        mShaderProgram.SetUniform("uQuadMargin", mQuadMargin);

        // Tight glow quad in both modes — opaque's far region is covered by the
        // Pass 1 black fill above, so the gather never runs fullscreen.
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();

        // Restore a known blend state for following renderers (the opaque path
        // disables blending).
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mShaderProgram.IsValid())
        {
            rebuildLoopSamples(config); // updates mSampleSpacing, used by setupGeometry
            setupGeometry(config);
            rebuildGradientLUT(config);
        }
    }

    bool NeonRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        // Cheap fullscreen black fill, used only by opaque mode. Reuses the
        // standard neon vertex shader (uMVP) so the fill quad respects the
        // viewport.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonRenderer.BlackRect");
        return mShaderProgram.IsValid() && mBlackRectShader.IsValid();
    }

    void NeonRenderer::setupGeometry(const Config &config)
    {
        // Size the quad to cover the lit region: rect + earlyOut. Beyond this the
        // halo/bloom are < ~1% at default strength, so geometry bounds the far
        // region instead of a per-fragment discard (tiler-friendly).
        // Factors come from the shared neon-tuning.h (also fed to the shaders).
        float margin = std::max(config.neon.glowRadius * float(EARLY_OUT_RADIUS_FACTOR),
                                mSampleSpacing * float(EARLY_OUT_SPACING_FACTOR));

        // The wide bloom (1/D tail) stays visible further out as bloomStrength /
        // intensity rise, so grow the quad with them — otherwise a strong bloom
        // gets chopped at a hard rectangular edge, worst on small geometry. The
        // shader still soft-fades the emission to zero at mQuadMargin, so even
        // if this under-estimates there's no hard cutoff.
        margin *= 1.0f + config.neon.bloomStrength * config.neon.intensity;
        mQuadMargin = margin;

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

        // Upload the positions as an N×1 RGBA8 data texture (16-bit-packed xy;
        // only byte textures are guaranteed on the target). The shader texelFetches
        // and decodes this instead of reading a uniform vec2[] array.
        GeometryUtils::PackLoopSamplesRGBA8(mLoopSamples, NUM_LOOP_SAMPLES, mLoopSamplesBytes, mSampleMaxCoord);
        mLoopSamplesTex.SetData(mLoopSamplesBytes.data(), NUM_LOOP_SAMPLES, /*height=*/1,
                                GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mLoopSamplesTex.SetParams(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

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

        // Edge devices often lack float-texture support; pack into ubyte RGBA8.
        std::vector<unsigned char> lutBytes(GRADIENT_LUT_SIZE * 4);
        for (int i = 0; i < GRADIENT_LUT_SIZE * 4; ++i)
        {
            lutBytes[i] = static_cast<unsigned char>(
                std::clamp(mLUTScratch[i] * 255.0f, 0.0f, 255.0f));
        }

        // 1-row 2D texture (sampled with v = 0.5 in the shader). REPEAT on
        // the U axis lets the gradient sweep wrap naturally; the V axis is a
        // single row, so CLAMP is fine.
        mGradientLUT.SetData(lutBytes.data(), GRADIENT_LUT_SIZE, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mGradientLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
    }
}
