#include "renderer/neon-optimized-renderer.h"
#include "renderer/neon-tuning.h"
#include "util/color-utils.h"
#include "util/constants.h"
#include "util/geometry-utils.h"
#include "util/segment-utils.h"
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
        /// Pixel distance the shader should treat as the cutoff boundary.
        /// Disabled cutoffs collapse to a huge sentinel so the shader's
        /// smoothstep / discard math naturally no-ops on realistic geometry;
        /// only the CPU knows this number, shaders see it as a plain uniform.
        constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;
        inline float GetCutoffSize(const Cutoff &c)
        {
            return c.enable ? c.size : CUTOFF_DISABLED_SIZE;
        }

        /// CPU-side mirror of neon.frag's std140 `SegmentBlock`:
        /// int padded to 16 bytes, each vec3 element padded to a vec4 stride.
        typedef struct SegmentBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 segments[MAX_SEGMENT_BOOSTS];
        } SegmentBlockData;

        static_assert(sizeof(SegmentBlockData) == 16 + 16 * MAX_SEGMENT_BOOSTS,
                      "SegmentBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon.frag's std140 `LoopSamplesBlock`.
        /// Sized by NEON_MAX_LOOP_SAMPLES (neon-tuning.h) - the ceiling; the
        /// shader iterates only uNumSamples of them per frame.
        typedef struct LoopSamplesBlockData
        {
            glm::vec4 samples[NEON_MAX_LOOP_SAMPLES];
        } LoopSamplesBlockData;

        static_assert(sizeof(LoopSamplesBlockData) == 16 * NEON_MAX_LOOP_SAMPLES,
                      "LoopSamplesBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon.frag's std140 `ArcBlock`.
        typedef struct ArcBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 arcs[MAX_ARCS];
        } ArcBlockData;

        static_assert(sizeof(ArcBlockData) == 16 + 16 * MAX_ARCS,
                      "ArcBlockData must match the shader's std140 layout");

        constexpr GLuint SEGMENT_BLOCK_BINDING = 0;
        constexpr GLuint LOOP_SAMPLES_BLOCK_BINDING = 1;
        constexpr GLuint ARC_BLOCK_BINDING = 2;
    }

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
        rebuildSegmentLUT(mCurrentConfig);
        rebuildArcLUT(mCurrentConfig);
        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        mGradientLUT.Rebuild(mCurrentConfig.neon.colorStops,
                             mCurrentConfig.neon.blendSpace,
                             mCurrentConfig.neon.colorTransitionDuration,
                             mCurrentConfig.optimizedNeon.gradientLutSize);
        return true;
    }

    void NeonOptimizedRenderer::Update(float deltaTime, float, const Config &)
    {
        // Drive the gradient cross-fade. Uses the raw frame delta, not clock
        // time, so a colour change still fades smoothly even while the
        // animation clock is paused.
        mGradientLUT.Update(deltaTime);
    }

    void NeonOptimizedRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.optimizedNeon.enable)
        {
            return;
        }

        int numSamples = std::max(1, std::min(config.optimizedNeon.numSamples,
                                              int(NEON_MAX_LOOP_SAMPLES)));

        packLightBlocks(config);
        renderEmissionPass(time, config, numSamples, viewportWidth, viewportHeight);
        renderHalfResNeonPass(config, viewportWidth, viewportHeight, numSamples);

        // Premultiplied "over" for both the black fill and the blit, so
        // blending stays ON across the whole composite (toggling GL_BLEND
        // mid-draw is a cross-driver footgun on mobile GLES).
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(config, viewportHeight);
        }

        renderBlitPass(config);

        // Restore default blend state for following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonOptimizedRenderer::OnConfigChanged(const Config &config)
    {
        // Same gating scheme as NeonRenderer, with extra deps from the
        // optimized sub-config: numSamples/resolutionScale affect the sample
        // walk + FBO scale; gradientLutSize picks the LUT texture width.
        const bool samplesDirty = config.geometry != mCurrentConfig.geometry ||
                                  config.optimizedNeon.resolutionScale != mCurrentConfig.optimizedNeon.resolutionScale ||
                                  config.optimizedNeon.numSamples != mCurrentConfig.optimizedNeon.numSamples;
        const bool geometryDirty = samplesDirty ||
                                   config.neon.glowRadius != mCurrentConfig.neon.glowRadius ||
                                   config.neon.bloomStrength != mCurrentConfig.neon.bloomStrength ||
                                   config.neon.intensity != mCurrentConfig.neon.intensity ||
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        // See NeonRenderer for the same guard - only per-segment stops/blend
        // affect the atlas; live position/length/boost don't.
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const bool segLutDirty = mEffectiveSegments != mBakedSegments;
        const bool arcLutDirty = config.neon.arcs != mBakedArcs;

        mCurrentConfig = config;
        if (!mNeonShader.IsValid())
        {
            return;
        }

        if (samplesDirty)
        {
            rebuildLoopSamples(config); // updates mSampleSpacing, read by setupGeometry
        }

        if (geometryDirty)
        {
            setupGeometry(config);
        }

        // Self-guarding: GradientLUT::Rebuild compares the stops and width it
        // last baked and returns without touching GL when nothing changed.
        mGradientLUT.Rebuild(config.neon.colorStops, config.neon.blendSpace,
                             config.neon.colorTransitionDuration,
                             config.optimizedNeon.gradientLutSize);

        if (segLutDirty)
        {
            rebuildSegmentLUT(config);
        }

        if (arcLutDirty)
        {
            rebuildArcLUT(config);
        }
    }

    bool NeonOptimizedRenderer::setupShaders()
    {
        // Same fragment body as NeonRenderer, compiled with the dynamic
        // NEON_LOOP_BOUND variant so numSamples stays a runtime knob.
        mNeonShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_FRAG_DYNAMIC_SRC,
                                    "NeonOptimized");

        // Opaque-mode fullscreen black fill (shared with NeonRenderer): the
        // analytic SDF in the fragment shader shapes the silhouette with
        // softness-aware feathering, so rounded corners AA cleanly - no more
        // stair-stepping like the old per-fragment discard in neon-blit.frag.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonOptimized.BlackRect");

        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonBlit");

        // Perimeter emission pre-pass, shared verbatim with NeonRenderer.
        // Reuses the standard neon vertex shader with an identity uMVP over a
        // fullscreen NDC quad; the fragment shader keys off gl_FragCoord.x.
        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonOptimized.Emission");
        if (!mNeonShader.IsValid() || !mBlackRectShader.IsValid() ||
            !mBlitShader.IsValid() || !mEmissionShader.IsValid())
        {
            return false;
        }

        mNeonShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);

        // Prefer a float target (see NeonRenderer::setupShaders for why), fall
        // back to RGBA8 where the driver won't render to one.
        mEmissionIsFloat = mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 1,
                                                  GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
        if (!mEmissionIsFloat)
        {
            LOG_W("NeonOptimizedRenderer: RGBA16F emission target unavailable, falling back to "
                  "RGBA8 (segment boosts above 1.0 will clamp).");
            if (!mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 1,
                                        GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
            {
                LOG_E("NeonOptimizedRenderer: could not create the emission target.");
                return false;
            }
        }
        return true;
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
            // there - a smaller margin faded the bloom out sooner and made the
            // optimized output look visibly shorter than the base (mismatch).
            // The factors come from the shared neon-tuning.h (also fed to the
            // base renderer's setupGeometry).
            float earlyOut = std::max(config.neon.glowRadius * scale * float(EARLY_OUT_RADIUS_FACTOR),
                                      mSampleSpacing * float(EARLY_OUT_SPACING_FACTOR));

            // Grow with bloom × intensity, matching the base renderer: the
            // 1/D bloom tail reaches further as those rise. The uQuadMargin
            // soft-fade (below, also mirrored from the base) guards the edge.
            float margin = earlyOut * (1.0f + config.neon.bloomStrength * config.neon.intensity);

            // Hard cap: when the outside cutoff is enabled the shader discards
            // emission past size + softness, so there's no point rasterising
            // further. Everything is in scaled/FBO space here; cutoff sizes
            // are unscaled pixels so multiply by `scale`. +1 (scaled) safety
            // so the shader's own softmask fades to zero before the quad edge.
            if (config.neon.outsideCutoff.enable)
            {
                float outSoft = std::max(config.neon.outsideCutoff.softness,
                                         static_cast<float>(SIDE_SOFT_EPSILON));
                float cutoffCap = (config.neon.outsideCutoff.size + outSoft) * scale + 1.0f;
                margin = std::min(margin, cutoffCap);
            }
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
        int n = std::max(1, std::min(config.optimizedNeon.numSamples, NEON_MAX_LOOP_SAMPLES));

        // Only n unique perimeter points are in use per frame (shader loop
        // bound is uNumSamples). The remaining UBO slots stay at (0,0,0,0)
        // - never read because the loop stops before them.
        // .xy = perimeter point (scaled px). (.zw stays 0 - the shader
        // recovers a fragment's continuous perimeter position geometrically
        // from vPos, so the per-sample phase pairs are no longer needed.)
        LoopSamplesBlockData block = {};
        for (int i = 0; i < n; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(n);
            glm::vec2 p = GeometryUtils::GetPointOnRectangle(t, config.geometry) * scale;
            block.samples[i] = glm::vec4(p, 0.0f, 0.0f);
        }
        mLoopSamplesBlock.SetData(&block, sizeof(block));

        float w = config.geometry.width;
        float h = config.geometry.height;
        float r = std::max(0.0f, std::min(config.geometry.cornerRadius, std::min(w, h) * 0.5f));

        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * PI * r;
        mPerimeter = perimeter * scale;
        mSampleSpacing = mPerimeter / static_cast<float>(n);
    }

    void NeonOptimizedRenderer::rebuildSegmentLUT(const Config &config)
    {
        // Same atlas shape as NeonRenderer (see there for the full rationale):
        // SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS RGBA8; row `i` = segment i's
        // baked stops or zero if it has none. Fixed width here (128) rather
        // than the tunable base gradientLutSize - segments are short so extra
        // resolution wouldn't be visible.
        constexpr int W = 128;
        constexpr int H = MAX_SEGMENT_BOOSTS;
        std::vector<unsigned char> atlas(W * H * 4, 0);

        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const std::vector<SegmentBoost> &effSegments = mEffectiveSegments;
        const int segCount = std::min(static_cast<int>(effSegments.size()),
                                      int(MAX_SEGMENT_BOOSTS));
        for (int s = 0; s < segCount; ++s)
        {
            const auto &seg = effSegments[s];
            if (seg.colorStops.empty())
            {
                continue;
            }
            unsigned char *row = atlas.data() + (s * W * 4);
            for (int x = 0; x < W; ++x)
            {
                float t = static_cast<float>(x) / static_cast<float>(W - 1);
                glm::vec3 c = ColorUtils::SampleStops(t, seg.colorStops, seg.blendSpace);
                row[x * 4 + 0] = static_cast<unsigned char>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 1] = static_cast<unsigned char>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 2] = static_cast<unsigned char>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 3] = 255;
            }
        }

        mSegmentLUT.SetData(atlas.data(), W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mSegmentLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

        mBakedSegments = mEffectiveSegments;
    }

    void NeonOptimizedRenderer::rebuildArcLUT(const Config &config)
    {
        // Same atlas shape as NeonRenderer::rebuildArcLUT (see there for the
        // full rationale). REPEAT on U (arcs share the perimeter hue cycle
        // with the base gradient), CLAMP on V (rows outside [0, arcCount)
        // are unused).
        constexpr int W = 128;
        constexpr int H = MAX_ARCS;
        std::vector<unsigned char> atlas(W * H * 4, 0);

        const int arcCount = std::min(static_cast<int>(config.neon.arcs.size()),
                                      int(MAX_ARCS));
        for (int a = 0; a < arcCount; ++a)
        {
            const auto &arc = config.neon.arcs[a];
            if (arc.colorStops.empty())
            {
                continue;
            }
            unsigned char *row = atlas.data() + (a * W * 4);
            for (int x = 0; x < W; ++x)
            {
                float t = static_cast<float>(x) / static_cast<float>(W - 1);
                glm::vec3 c = ColorUtils::SampleStops(t, arc.colorStops, arc.blendSpace);
                row[x * 4 + 0] = static_cast<unsigned char>(std::clamp(c.r * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 1] = static_cast<unsigned char>(std::clamp(c.g * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 2] = static_cast<unsigned char>(std::clamp(c.b * 255.0f, 0.0f, 255.0f));
                row[x * 4 + 3] = 255;
            }
        }

        mArcLUT.SetData(atlas.data(), W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mArcLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);

        mBakedArcs = config.neon.arcs;
    }

    void NeonOptimizedRenderer::packLightBlocks(const Config &config)
    {
        // Pack the segment vector as vec4(position, invSigma, boost, hasStops)
        // into the std140 SegmentBlock UBO (DALi-compatible pattern - see the
        // shader). Segment `position` is a normalised perimeter coord in
        // [0, 1), so the resolutionScale does not apply.
        SegmentBlockData segBlock = {};
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const std::vector<SegmentBoost> &effSegments = mEffectiveSegments;
        int segCount = std::min(static_cast<int>(effSegments.size()),
                                int(MAX_SEGMENT_BOOSTS));
        segBlock.count = segCount;
        for (int i = 0; i < segCount; ++i)
        {
            const auto &s = effSegments[i];
            float invSigma = 1.0f / std::max(s.length * 0.5f, 1e-3f);
            // .w = hasOwnStops flag (see NeonRenderer for details).
            float hasStops = s.colorStops.empty() ? 0.0f : 1.0f;
            segBlock.segments[i] = glm::vec4(s.position, invSigma, s.boost, hasStops);
        }
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));
        mSegmentBlock.BindBase(SEGMENT_BLOCK_BINDING);

        // Pack the arcs vector into ArcBlock: vec4(start, length, intensity,
        // hasStops) per entry. Same packing as NeonRenderer; arc start/length
        // are normalised perimeter coords so resolutionScale doesn't apply.
        ArcBlockData arcBlock = {};
        int arcCount = std::min(static_cast<int>(config.neon.arcs.size()),
                                int(MAX_ARCS));
        arcBlock.count = arcCount;
        for (int i = 0; i < arcCount; ++i)
        {
            const auto &a = config.neon.arcs[i];
            float hasStops = a.colorStops.empty() ? 0.0f : 1.0f;
            arcBlock.arcs[i] = glm::vec4(a.start, a.length, a.intensity, hasStops);
        }
        mArcBlock.SetData(&arcBlock, sizeof(arcBlock));
        mArcBlock.BindBase(ARC_BLOCK_BINDING);
    }

    void NeonOptimizedRenderer::renderEmissionPass(float time, const Config &config, int numSamples,
                                                   int viewportWidth, int viewportHeight)
    {
        // One fragment per perimeter sample; see neon-emission.frag. Blending
        // must be off - this is a table write, not a composite.
        glDisable(GL_BLEND);
        mEmissionBuffer.Bind(); // also sets the viewport to NEON_MAX_LOOP_SAMPLES x 1

        mEmissionShader.Use();
        mEmissionShader.SetUniform("uMVP", glm::mat4(1.0f));
        mEmissionShader.SetUniform("uTime", time);
        mEmissionShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mEmissionShader.SetUniform("uIntensity", config.neon.intensity);
        // Scaled perimeter, matching the scaled uRectSize the Pass-1 shader
        // uses for its own feather conversion.
        mEmissionShader.SetUniform("uPerimeter", mPerimeter);
        mEmissionShader.SetUniform("uNumSamples", numSamples);

        mGradientLUT.Bind(0);
        mEmissionShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mEmissionShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mEmissionShader.SetUniform("uArcLUT", 2);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mEmissionShader.Unuse();

        // Hand the screen back to the caller in the state every other pass
        // expects: default framebuffer, full viewport, blending on.
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);
        glEnable(GL_BLEND);
    }

    void NeonOptimizedRenderer::renderHalfResNeonPass(const Config &config, int viewportWidth,
                                                      int viewportHeight, int numSamples)
    {
        float scale = config.optimizedNeon.resolutionScale;
        int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        mHalfResBuffer.Resize(bufW, bufH);
        mHalfResBuffer.Bind();

        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Premultiplied "over" into the transparent FBO: a single non-overlapping
        // quad over (0,0,0,0) leaves the FBO holding the shader's premultiplied
        // colour + coverage alpha, ready to be composited over the backbuffer.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // Everything below is in scaled (FBO) space - the rect, the line width,
        // the cutoffs and mSampleSpacing are all pre-multiplied by `scale`, so
        // the shader never has to know the resolution it is running at.
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(bufW), 0.0f, static_cast<float>(bufH), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        center *= scale;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;
        glm::vec2 rectSizeScaled(config.geometry.width * scale, config.geometry.height * scale);

        mNeonShader.Use();
        mNeonShader.SetUniform("uMVP", mvp);
        mNeonShader.SetUniform("uRectSize", rectSizeScaled);
        mNeonShader.SetUniform("uCornerRadius", config.geometry.cornerRadius * scale);
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mNeonShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mNeonShader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mNeonShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mNeonShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mNeonShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        mNeonShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff) * scale);
        mNeonShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        mNeonShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff) * scale);
        mNeonShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);
        mNeonShader.SetUniform("uSampleSpacing", mSampleSpacing);
        mNeonShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mNeonShader.SetUniform("uQuadMargin", mQuadMargin);
        mNeonShader.SetUniform("uNumSamples", numSamples);

        // Loop sample positions from the LoopSamplesBlock UBO (see the shader)
        // - raw float32 vec4[N], .xy holds the perimeter point in FBO pixels.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // The three gradient LUTs are consumed by the pre-pass, not here - this
        // shader only needs its output: one texel per sample holding the
        // composed colour and coverage.
        mEmissionBuffer.BindTexture(0);
        mNeonShader.SetUniform("uEmission", 0);

        mNeonVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mNeonShader.Unuse();

        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);
    }

    void NeonOptimizedRenderer::renderOpaqueFill(const Config &config, int viewportHeight)
    {
        // A single NDC quad + identity MVP; the black-rect fragment shader
        // shapes the silhouette from the analytic rounded-box SDF read off
        // gl_FragCoord, with softness-aware feathering:
        //   BOTH    -> whole viewport opaque black.
        //   INSIDE  -> black only where d <= softEdge; off-side stays clear.
        //   OUTSIDE -> mirror of INSIDE.
        // Rounded corners AA cleanly via fwidth(d) - no discard, no stair-step.
        //
        // Rect centre in FULL-res gl_FragCoord space (y-up) - this pass draws
        // on the backbuffer, so nothing here is scaled.
        glm::vec2 centerFull(config.geometry.position.x + config.geometry.width * 0.5f,
                             static_cast<float>(viewportHeight) - config.geometry.position.y -
                                 config.geometry.height * 0.5f);
        float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                    static_cast<float>(SIDE_SOFT_EPSILON));

        mBlackRectShader.Use();
        mBlackRectShader.SetUniform("uMVP", glm::mat4(1.0f));
        mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mBlackRectShader.SetUniform("uRectCenter", centerFull);
        mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
        mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
        mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlackRectShader.Unuse();
    }

    void NeonOptimizedRenderer::renderBlitPass(const Config &config)
    {
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read that composites over whatever's on
        // the backbuffer (black fill if opaque, original bg otherwise).
        mBlitShader.Use();
        mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));

        mHalfResBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlitShader.Unuse();
    }
} // namespace EdgeLighting
