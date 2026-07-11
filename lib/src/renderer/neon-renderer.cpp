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
#include <cstdint>

namespace EdgeLighting
{
    namespace
    {
        /// CPU-side mirror of neon.frag's std140 `SegmentBlock`: the int is
        /// padded to 16 bytes and each vec3 element to a vec4 stride.
        typedef struct SegmentBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 segments[MAX_SEGMENT_BOOSTS];
        } SegmentBlockData;

        static_assert(sizeof(SegmentBlockData) == 16 + 16 * MAX_SEGMENT_BOOSTS,
                      "SegmentBlockData must match the shader's std140 layout");

        constexpr GLuint SEGMENT_BLOCK_BINDING = 0;
    }

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

        // Static NDC-order attribs for the LUT debug strip; the actual verts
        // are (re)uploaded from setupGeometry() so the strip tracks rect size.
        mLUTStripVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        // Unit quad for the per-stop debug markers ([-1,+1] on both axes). Each
        // stop's marker is drawn by scaling+translating this quad via uMVP so
        // it lands at that stop's perimeter position; the marker fragment
        // shader treats vPos in [-1,+1] as disc space.
        // clang-format off
        float unitQuad[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mStopMarkerVertexArray.SetVertexData(unitQuad, sizeof(unitQuad));
        mStopMarkerVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

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
            mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
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
        // Pack the segment vector as vec3(position, invSigma, boost) into the
        // std140 SegmentBlock UBO (DALi-compatible pattern — see neon.frag).
        // Empty vector → uSegmentCount=0 and the shader skips the whole feature.
        SegmentBlockData segBlock = {};
        int segCount = std::min(static_cast<int>(config.neon.segmentBoosts.size()),
                                int(MAX_SEGMENT_BOOSTS));
        segBlock.count = segCount;
        for (int i = 0; i < segCount; ++i)
        {
            const auto &s = config.neon.segmentBoosts[i];
            float invSigma = 1.0f / std::max(s.length * 0.5f, 1e-3f);
            segBlock.segments[i] = glm::vec4(s.position, invSigma, s.boost, 0.0f);
        }
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));
        mSegmentBlock.BindBase(SEGMENT_BLOCK_BINDING);
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

        // --- Debug: gradient LUT strip at the geometry centre -----------------
        // Overwrites the neon output within the strip rect so the baked ring is
        // readable regardless of the glow's tone-mapped brightness.
        if (config.neon.showGradientLUT)
        {
            glDisable(GL_BLEND);
            mLUTDebugShader.Use();
            mLUTDebugShader.SetUniform("uMVP", mvp);
            mLUTDebugShader.SetUniform("uStripHalfSize", mLUTStripHalfSize);
            mLUTDebugShader.SetUniform("uTime", time);
            mLUTDebugShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
            mGradientLUT.Bind(0);
            mLUTDebugShader.SetUniform("uGradientLUT", 0);
            mLUTStripVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mLUTDebugShader.Unuse();
        }

        // --- Debug: per-stop markers on the perimeter -------------------------
        // Draws a filled disc in each stop's colour at its perimeter position,
        // so the raw (position, colour) inputs can be checked against the LUT
        // strip and the on-screen glow. Uses standard alpha blending for the
        // ring / anti-aliased edge to composite cleanly.
        if (config.neon.showColorStops && !config.neon.colorStops.empty())
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Scale marker with the smaller half-extent so it stays inside the
            // rect on very tall/thin geometries; cap at 12 px so it's not huge
            // on large rects. min(halfRect) → smaller of width/2, height/2.
            float markerRadius = std::min(std::min(halfRectW, halfRectH) * 0.06f, 12.0f);
            mStopMarkerShader.Use();
            for (const auto &stop : config.neon.colorStops)
            {
                glm::vec2 localPt = GeometryUtils::GetPointOnRectangle(stop.position, config.geometry);
                glm::mat4 markerModel =
                    glm::translate(glm::mat4(1.0f), glm::vec3(center + localPt, 0.0f)) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(markerRadius, markerRadius, 1.0f));
                mStopMarkerShader.SetUniform("uMVP", proj * markerModel);
                mStopMarkerShader.SetUniform("uMarkerColor", stop.color);
                mStopMarkerVertexArray.DrawArrays(GL_TRIANGLES, 6);
            }
            mStopMarkerShader.Unuse();
        }

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
        // Debug LUT strip — reuses the standard neon vertex shader (uMVP + aPos → vPos)
        // so the strip quad respects the same rect-local transform as the glow quad.
        mLUTDebugShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                        "NeonRenderer.LUTDebug");
        // Debug stop markers — same vertex shader, filled-disc fragment.
        mStopMarkerShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                          ShaderSource::NEON_STOP_MARKER_FRAG_SRC,
                                          "NeonRenderer.StopMarker");
        if (!mShaderProgram.IsValid() || !mBlackRectShader.IsValid() ||
            !mLUTDebugShader.IsValid() || !mStopMarkerShader.IsValid())
        {
            return false;
        }

        mShaderProgram.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        return true;
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

        // Debug LUT strip: 60% of rect width × min(rect_height / 6, 40 px),
        // centred on the geometry origin so it sits inside the rounded box.
        float stripHalfW = halfW * 0.6f;
        float stripHalfH = std::min(halfH / 6.0f, 20.0f);
        mLUTStripHalfSize = glm::vec2(stripHalfW, stripHalfH);
        // clang-format off
        float stripVerts[] = {
            -stripHalfW,  stripHalfH,  -stripHalfW, -stripHalfH,   stripHalfW, -stripHalfH,
            -stripHalfW,  stripHalfH,   stripHalfW, -stripHalfH,   stripHalfW,  stripHalfH,
        };
        // clang-format on
        mLUTStripVertexArray.SetVertexData(stripVerts, sizeof(stripVerts));
        mLUTStripVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
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
