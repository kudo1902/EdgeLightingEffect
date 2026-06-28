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

        // Compositing mode:
        //  - opaque == false (default): premultiplied-alpha "over" —
        //    final = src.rgb + dst * (1 - src.a). The shader emits premultiplied
        //    colour with coverage in alpha, so the hot core occludes background
        //    objects while the halo/bloom stay additive and the dark surround
        //    leaves the background untouched. The expensive gather runs only on
        //    the tight glow quad.
        //  - opaque == true: blending off. A cheap fullscreen black fill runs
        //    first (Pass 1), then the SAME tight glow quad draws the neon on top
        //    (Pass 2). The far region keeps the fill's black, so the result is a
        //    fullscreen opaque image — but the costly 128-sample gather still
        //    only runs on the tight quad, not the whole screen.
        if (config.neon.opaque)
        {
            glDisable(GL_BLEND);

            // Pass 1: fullscreen black fill. Build a viewport-covering quad in
            // rect-local space — the four viewport corners are (world - center);
            // uMVP maps local -> NDC so the quad lands on the screen corners.
            float l = -center.x;
            float b = -center.y;
            float r = static_cast<float>(viewportWidth) - center.x;
            float t = static_cast<float>(viewportHeight) - center.y;
            // clang-format off
            float verts[] = {
                l, t, l, b, r, b,
                l, t, r, b, r, t,
            };
            // clang-format on
            mFullVertexArray.SetVertexData(verts, sizeof(verts));
            mFullVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

            mFillShader.Use();
            mFillShader.SetUniform("uMVP", mvp);
            mFillShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
            mFillShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mFillShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
            mFillShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
            mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mFillShader.Unuse();
        }
        else
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        // Pass 2 (opaque) / only pass (transparent): the neon gather on the
        // tight glow quad, in both modes.
        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth);
        mShaderProgram.SetUniform("uIntensity", config.neon.intensity);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);
        int sampleCount = static_cast<int>(mLoopSamples.size());
        if (sampleCount > 0)
        {
            mShaderProgram.SetUniform("uLoopSamples", mLoopSamples.data(), sampleCount);

            // Per-sample colour + weight (baseLevel + segments + spots + colour),
            // rebuilt each frame so animated segments/colours update directly.
            ColorUtils::BuildSampleData(config.neon, time, sampleCount, mSampleData);
            mShaderProgram.SetUniform("uSampleData", mSampleData.data(), sampleCount);
        }

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
        mFillShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_FILL_FRAG_SRC,
                                    "NeonRenderer.Fill");
        return mShaderProgram.IsValid() && mFillShader.IsValid();
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

        float w = config.geometry.width;
        float h = config.geometry.height;
        float r = std::max(0.0f, std::min(config.geometry.cornerRadius, std::min(w, h) * 0.5f));

        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * PI * r;
        mSampleSpacing = perimeter / static_cast<float>(NUM_LOOP_SAMPLES);
    }
}
