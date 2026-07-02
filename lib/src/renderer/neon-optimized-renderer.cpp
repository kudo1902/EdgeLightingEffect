#include "renderer/neon-optimized-renderer.h"
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

    bool NeonOptimizedRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonOptimizedRenderer shaders.");
            return false;
        }
        // rebuildLoopSamples must run before setupGeometry: the Pass-1 quad
        // size depends on mSampleSpacing (computed in rebuildLoopSamples).
        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
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

        // Premultiplied "over" into the transparent FBO: a single non-overlapping
        // quad over (0,0,0,0) leaves the FBO holding the shader's premultiplied
        // colour + coverage alpha, ready to be composited over the backbuffer.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

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
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mNeonShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
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
        mNeonShader.SetUniform("uQuadMargin", mQuadMargin);

        // Loop sample positions from a data texture (unit 1) the shader
        // texelFetches, instead of a uniform vec2[] array.
        mLoopSamplesTex.Bind(1);
        mNeonShader.SetUniform("uLoopSamplesTex", 1);
        mNeonShader.SetUniform("uSampleMaxCoord", mSampleMaxCoord);

        mGradientLUT.Bind(0);
        mNeonShader.SetUniform("uGradientLUT", 0);

        mNeonVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mNeonShader.Unuse();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 2a (opaque only): fullscreen black fill on the backbuffer ---
        // A single NDC quad + identity MVP; the black-rect fragment shader
        // shapes the silhouette from the analytic rounded-box SDF read off
        // gl_FragCoord, with softness-aware feathering:
        //   BOTH    -> whole viewport opaque black.
        //   INSIDE  -> black only where d <= softEdge; off-side stays clear.
        //   OUTSIDE -> mirror of INSIDE.
        // Rounded corners AA cleanly via fwidth(d) — no discard, no stair-step.
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);

        // Premultiplied "over" for both the black fill and the blit, so
        // blending stays ON the whole pass (toggling GL_BLEND mid-draw is a
        // cross-driver footgun on mobile GLES).
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glm::mat4 identity(1.0f);
        if (config.neon.opaque)
        {
            // Rect centre in full-res gl_FragCoord space (y-up).
            glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                                 static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
            float softEdge = std::max(config.neon.glowSideSoftness,
                                      static_cast<float>(SIDE_SOFT_EPSILON));

            mBlackRectShader.Use();
            mBlackRectShader.SetUniform("uMVP", identity);
            mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
            mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mBlackRectShader.SetUniform("uRectCenter", centerFull);
            mBlackRectShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
            mBlackRectShader.SetUniform("uSoftEdge", softEdge);
            mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mBlackRectShader.Unuse();
        }

        // --- Pass 2b: bilinear composite of the half-res neon FBO ---
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read that composites over whatever's on
        // the backbuffer (black fill if opaque, original bg otherwise).
        mBlitShader.Use();
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
        // Restore default blend state for following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonOptimizedRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mNeonShader.IsValid())
        {
            rebuildLoopSamples(config); // updates mSampleSpacing, used by setupGeometry
            setupGeometry(config);
            rebuildGradientLUT(config);
        }
    }

    bool NeonOptimizedRenderer::setupShaders()
    {
        mNeonShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_OPTIMIZED_FRAG_SRC,
                                    "NeonOptimized");

        // Opaque-mode fullscreen black fill (shared with NeonRenderer): the
        // analytic SDF in the fragment shader shapes the silhouette with
        // softness-aware feathering, so rounded corners AA cleanly — no more
        // stair-stepping like the old per-fragment discard in neon-blit.frag.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonOptimized.BlackRect");

        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonBlit");
        return mNeonShader.IsValid() && mBlackRectShader.IsValid() && mBlitShader.IsValid();
    }

    void NeonOptimizedRenderer::setupGeometry(const Config &config)
    {
        float scale = config.optimizedNeon.resolutionScale;

        // --- Scaled glow quad (pass 1) ---
        // Size the quad to exactly cover the lit region: rect + earlyOut, where
        // earlyOut matches the shader's old discard threshold. Beyond this the
        // halo/bloom are < ~1% and were previously discarded; now geometry
        // bounds them instead (no per-fragment discard → tiler-friendly).
        // Everything here is in scaled (FBO) space: glowRadius*scale and
        // mSampleSpacing are already scaled, matching the uniforms uploaded
        // in Render().
        {
            // Use the SAME early-out factors as the base NeonRenderer so the
            // bloom's wide 1/D tail reaches exactly as far here as it does
            // there — a smaller margin faded the bloom out sooner and made the
            // optimized output look visibly shorter than the base (mismatch).
            // The factors come from the shared neon-tuning.h (also fed to the
            // base renderer's setupGeometry).
            float earlyOut = std::max(config.neon.glowRadius * scale * float(EARLY_OUT_RADIUS_FACTOR),
                                      mSampleSpacing * float(EARLY_OUT_SPACING_FACTOR));

            // Grow with bloom × intensity, matching the base renderer: the
            // 1/D bloom tail reaches further as those rise. The uQuadMargin
            // soft-fade (below, also mirrored from the base) guards the edge.
            float margin = earlyOut * (1.0f + config.neon.bloomStrength * config.neon.intensity);
            mQuadMargin = margin;
            float halfW = config.geometry.width * 0.5f * scale;
            float halfH = config.geometry.height * 0.5f * scale;
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
            mNeonVertexArray.SetVertexData(verts, sizeof(verts));
            mNeonVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        }

        // --- Fullscreen NDC quad (pass 2, identity MVP) ---
        {
            // clang-format off
            float verts[] = {
                -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f, -1.0f,
                -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  1.0f,
            };
            // clang-format on

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

        // Upload the (scaled) positions as an N×1 RGBA8 data texture (16-bit-packed
        // xy; byte texture only on the target). The shader texelFetches + decodes
        // this instead of a uniform vec2[].
        GeometryUtils::PackLoopSamplesRGBA8(mLoopSamples, MAX_LOOP_SAMPLES, mLoopSamplesBytes, mSampleMaxCoord);
        mLoopSamplesTex.SetData(mLoopSamplesBytes.data(), MAX_LOOP_SAMPLES, /*height=*/1,
                                GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mLoopSamplesTex.SetParams(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

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
