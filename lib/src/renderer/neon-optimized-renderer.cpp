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

        /// CPU-side mirror of neon-optimized.frag's std140 `SegmentBlock`:
        /// int padded to 16 bytes, each vec3 element padded to a vec4 stride.
        typedef struct SegmentBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 segments[MAX_SEGMENT_BOOSTS];
        } SegmentBlockData;

        static_assert(sizeof(SegmentBlockData) == 16 + 16 * MAX_SEGMENT_BOOSTS,
                      "SegmentBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon-optimized.frag's std140 `LoopSamplesBlock`.
        /// Sized by NEON_MAX_LOOP_SAMPLES (neon-tuning.h) - the ceiling; the
        /// shader iterates only uNumSamples of them per frame.
        typedef struct LoopSamplesBlockData
        {
            glm::vec4 samples[NEON_MAX_LOOP_SAMPLES];
        } LoopSamplesBlockData;

        static_assert(sizeof(LoopSamplesBlockData) == 16 * NEON_MAX_LOOP_SAMPLES,
                      "LoopSamplesBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon-optimized.frag's std140 `ArcBlock`.
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
        rebuildSegmentLUT(mCurrentConfig);
        rebuildArcLUT(mCurrentConfig);
        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        rebuildGradientLUT(mCurrentConfig);
        return true;
    }

    void NeonOptimizedRenderer::Update(float deltaTime, float, const Config &)
    {
        // Drive the gradient cross-fade (see rebuildGradientLUT). Uses the raw
        // frame delta, not clock time, so a colour change still fades smoothly
        // even while the animation clock is paused.
        if (!mFading)
        {
            return;
        }

        mFadeElapsed += deltaTime;
        float u = (mFadeDuration > 0.0f) ? (mFadeElapsed / mFadeDuration) : 1.0f;
        u = std::clamp(u, 0.0f, 1.0f);
        float s = u * u * (3.0f - 2.0f * u); // smoothstep ease-in-out

        const int n = mLUTBakedSize * 4;
        mLUTDisplay.resize(n);
        for (int i = 0; i < n; ++i)
        {
            mLUTDisplay[i] = mLUTFrom[i] + (mLUTTarget[i] - mLUTFrom[i]) * s;
        }
        uploadGradientLUT(mLUTDisplay, mLUTBakedSize);

        if (u >= 1.0f)
        {
            mLUTDisplay = mLUTTarget; // land exactly on the target
            mFading = false;
        }
    }

    /// Packs SegmentBlock + ArcBlock and binds them to their binding points.
    /// Shared by both render paths - the contents are normalised perimeter
    /// coordinates and per-arc intensities, so nothing here depends on
    /// resolutionScale or prePassScale.
    void NeonOptimizedRenderer::uploadSegmentAndArcBlocks(const Config &config)
    {
        // Pack the segment vector as vec3(position, invSigma, boost) into the
        // std140 SegmentBlock UBO (DALi-compatible pattern - see the shader).
        // Same packing as NeonRenderer; segment `position` is a normalised
        // perimeter coord in [0, 1), so the resolutionScale does not apply.
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
        // are normalised perimeter coords in [0, 1) so resolutionScale doesn't
        // apply.
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

    /// Emission pre-pass path. Splits the work by SPATIAL FREQUENCY rather
    /// than scaling the whole pass down:
    ///
    ///   Pass A (prePassScale, RGBA16F MRT x2) - the 128-sample gather, which
    ///          is ~96% of the cost and whose four outputs (base hue, segment
    ///          hue, halo coverage, bloom coverage) are each a convolution and
    ///          so band-limited by construction.
    ///   Pass B (opaque only, full res)        - the black-rect silhouette.
    ///   Pass C (full res)                     - the composite: filament,
    ///          analytic halo/bloom profiles, pointwise coverages, cutoffs,
    ///          glowSide, quad fade and grading, straight to the backbuffer.
    ///
    /// Everything runs in FULL-RES pixel units. Pass A gets its reduced
    /// resolution purely from the viewport it draws into, so no uniform is
    /// pre-scaled and none of the single-pass path's uResolutionScale
    /// corrections apply. There is also no blit: Pass C writes the backbuffer
    /// directly, so the cutoffs and the one-sided cut are simply applied where
    /// they always were, at full resolution, before the tone map.
    void NeonOptimizedRenderer::renderEmissionPrePass(int viewportWidth, int viewportHeight,
                                                      float time, const Config &config)
    {
        const float preScale = std::clamp(config.optimizedNeon.prePassScale, 0.05f, 1.0f);
        const int gatherW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * preScale), 1);
        const int gatherH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * preScale), 1);

        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;

        // FULL-RES projection, used by BOTH passes. Drawing it into the
        // smaller gather viewport is what reduces the pre-pass resolution -
        // NDC is resolution-independent, so vPos still interpolates to
        // rect-local FULL-RES pixels in the gather shader.
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                    0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(centerFull, 0.0f));

        const glm::vec2 rectSize(config.geometry.width, config.geometry.height);
        const int numSamples = std::min(config.optimizedNeon.numSamples, int(NEON_MAX_LOOP_SAMPLES));

        uploadSegmentAndArcBlocks(config);
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // --- Pass A: gather at prePassScale into the RGBA16F MRT pair ---
        // RGBA16F because the coverage channels carry per-arc intensity, which
        // the UI allows above 1.0 - RGBA8 would clamp the glow of a bright arc.
        if (!mGatherBuffer.Resize(gatherW, gatherH, GL_RGBA16F, 2))
        {
            return; // Resize logs the reason; skip the frame rather than draw garbage.
        }
        mGatherBuffer.Bind();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // No blending: this pass writes DATA, not premultiplied colour.
        glDisable(GL_BLEND);

        mGatherShader.Use();
        mGatherShader.SetUniform("uMVP", mvp);
        mGatherShader.SetUniform("uRectSize", rectSize);
        mGatherShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mGatherShader.SetUniform("uGlowRadius", config.neon.glowRadius);
        mGatherShader.SetUniform("uTime", time);
        mGatherShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mGatherShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mGatherShader.SetUniform("uNumSamples", numSamples);
        mGradientLUT.Bind(0);
        mGatherShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mGatherShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mGatherShader.SetUniform("uArcLUT", 2);
        mFullResVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mGatherShader.Unuse();

        // --- Back to the backbuffer at full resolution ---
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied "over"

        // --- Pass B (opaque only): black-rect silhouette, unchanged ---
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            glm::mat4 identity(1.0f);
            mBlackRectShader.Use();
            mBlackRectShader.SetUniform("uMVP", identity);
            mBlackRectShader.SetUniform("uRectSize", rectSize);
            mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mBlackRectShader.SetUniform("uRectCenter", centerFull);
            mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
            mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
            mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
            mBlackRectShader.SetUniform("uOpaqueSoftness",
                                        std::max(config.neon.opaqueSoftness, 0.0f));
            mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
            mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mBlackRectShader.Unuse();
        }

        // --- Pass C: full-res composite straight to the backbuffer ---
        mCompositeShader.Use();
        mCompositeShader.SetUniform("uMVP", mvp);
        mCompositeShader.SetUniform("uRectSize", rectSize);
        mCompositeShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mCompositeShader.SetUniform("uLineWidth", config.neon.lineWidth);
        mCompositeShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mCompositeShader.SetUniform("uIntensity", config.neon.intensity);
        mCompositeShader.SetUniform("uTime", time);
        mCompositeShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mCompositeShader.SetUniform("uGlowRadius", config.neon.glowRadius);
        mCompositeShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mCompositeShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mCompositeShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mCompositeShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mCompositeShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mCompositeShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mCompositeShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);
        mCompositeShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mCompositeShader.SetUniform("uQuadMargin", mFullResQuadMargin);
        mCompositeShader.SetUniform("uViewportSize",
                                    glm::vec2(static_cast<float>(viewportWidth),
                                              static_cast<float>(viewportHeight)));

        // showHalfRes reuses its meaning here: nearest-neighbour on the gather
        // targets exposes the pre-pass grid, which is how you check whether
        // prePassScale is too low for the current kernel widths.
        const GLenum filter = config.optimizedNeon.showHalfRes ? GL_NEAREST : GL_LINEAR;
        for (int i = 0; i < 2; ++i)
        {
            mGatherBuffer.BindTexture(static_cast<GLuint>(i), i);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
        }
        mCompositeShader.SetUniform("uGatherHueHalo", 0);
        mCompositeShader.SetUniform("uGatherSegBloom", 1);

        mFullResVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mCompositeShader.Unuse();

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonOptimizedRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.optimizedNeon.enable)
        {
            return;
        }

        if (config.optimizedNeon.emissionPrePass)
        {
            renderEmissionPrePass(viewportWidth, viewportHeight, time, config);
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
        // Lets the shader convert neon-tuning.h's full-res px constants
        // (FILAMENT_MIN_HALF_WIDTH, HEAD/TAIL_FEATHER_PX) into the FBO space
        // every other pixel uniform below is already scaled into.
        mNeonShader.SetUniform("uResolutionScale", scale);
        mNeonShader.SetUniform("uRectSize", rectSizeScaled);
        mNeonShader.SetUniform("uCornerRadius", config.geometry.cornerRadius * scale);
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mNeonShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mNeonShader.SetUniform("uIntensity", config.neon.intensity);
        mNeonShader.SetUniform("uTime", time);
        mNeonShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mNeonShader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mNeonShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        // glowSide is deliberately NOT uploaded here: the one-sided cut is a
        // hard 0/1 edge at d == 0 and is applied at full resolution in
        // neon-blit.frag instead, so Pass 1 leaves the emission continuous
        // across d == 0. See neon-optimized.frag / neon-blit.frag.
        //
        // The cutoffs, by contrast, ARE Pass 1's: they are feathered ramps, and
        // tonemap(x * m) != tonemap(x) * m, so they have to land before the
        // tone map to match neon.frag. No headroom is added to them - Pass 1
        // must place the ramp at the TRUE cutoff, exactly where neon.frag does.
        // (An earlier version widened it by a texel to give the blit something
        // to cut into; that displaced the whole ramp outward by 2 full-res px
        // at scale 0.5 and was the bulk of the remaining divergence from the
        // base renderer. The blit's clamp sits at cutoff + softness, past the
        // ramp's own end, so it removes upscale spill without any headroom.)
        mNeonShader.SetUniform("uInsideCutoff",
                               GetCutoffSize(config.neon.insideCutoff) * scale);
        mNeonShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        mNeonShader.SetUniform("uOutsideCutoff",
                               GetCutoffSize(config.neon.outsideCutoff) * scale);
        mNeonShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);
        uploadSegmentAndArcBlocks(config);

        mNeonShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mNeonShader.SetUniform("uQuadMargin", mQuadMargin);

        // Loop sample positions from the LoopSamplesBlock UBO (see the shader)
        // - raw float32 vec4[N], .xy holds the perimeter point in FBO pixels.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniform("uNumSamples", std::min(config.optimizedNeon.numSamples,
                                                       NEON_MAX_LOOP_SAMPLES));

        mGradientLUT.Bind(0);
        mNeonShader.SetUniform("uGradientLUT", 0);
        // Per-segment gradient atlas on unit 1 (see NeonRenderer for the shape).
        mSegmentLUT.Bind(1);
        mNeonShader.SetUniform("uSegmentLUT", 1);
        // Per-arc gradient atlas on unit 2 - sampled only when the winning
        // arc has stops (ArcBlock's vec4.w).
        mArcLUT.Bind(2);
        mNeonShader.SetUniform("uArcLUT", 2);

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
        // Rounded corners AA cleanly via fwidth(d) - no discard, no stair-step.
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);

        // Premultiplied "over" for both the black fill and the blit, so
        // blending stays ON the whole pass (toggling GL_BLEND mid-draw is a
        // cross-driver footgun on mobile GLES).
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glm::mat4 identity(1.0f);
        // Rect centre in full-res gl_FragCoord space (y-up). Used by the
        // opaque fill and by the blit, which re-imposes the band's cutoffs at
        // full resolution.
        glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            mBlackRectShader.Use();
            mBlackRectShader.SetUniform("uMVP", identity);
            mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
            mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mBlackRectShader.SetUniform("uRectCenter", centerFull);
            float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                        static_cast<float>(SIDE_SOFT_EPSILON));
            mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
            mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
            mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
            mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
            mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
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
        // Full-res band geometry so the blit can re-impose the cutoffs sharply.
        mBlitShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlitShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mBlitShader.SetUniform("uRectCenter", centerFull);
        // The blit's hard clamp only exists to remove upscale spill. At
        // scale >= 1 nothing is resampled, so there is no spill and the clamp
        // can only do harm: its own half-pixel AA would land a fraction off
        // Pass 1's razor-sharp one-sided cut and leave a 1 px seam (measured
        // as 114/255 on 0.014% of pixels). Feed it disabled sentinels there so
        // every term evaluates to a pass-through 1.
        const bool clampBlur = (scale < 1.0f);
        const float noClamp = CUTOFF_DISABLED_SIZE;
        mBlitShader.SetUniform("uInsideCutoff",
                               clampBlur ? GetCutoffSize(config.neon.insideCutoff) : noClamp);
        mBlitShader.SetUniform("uInsideCutoffSoftness",
                               clampBlur ? config.neon.insideCutoff.softness : 0.0f);
        mBlitShader.SetUniform("uOutsideCutoff",
                               clampBlur ? GetCutoffSize(config.neon.outsideCutoff) : noClamp);
        mBlitShader.SetUniform("uOutsideCutoffSoftness",
                               clampBlur ? config.neon.outsideCutoff.softness : 0.0f);
        // glowSide is always applied here - Pass 1 no longer cuts at d == 0 at
        // any scale, so this pass is the only thing that does.
        mBlitShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mBlitShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mBlitShader.Unuse();
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
                                   // lineWidth feeds setupGeometry's filament-reach floor, which is
                                   // what sizes the quad whenever glowRadius is small. Miss it and a
                                   // widened filament keeps the old, tighter margin and gets clipped
                                   // on the OUTSIDE only (the quad bounds the exterior; the interior
                                   // is always covered) - the "outer glow dropped at glowRadius 0"
                                   // report. Only bites at small glowRadius, because above
                                   // lineWidth / 10.4 the glow term wins the max() anyway.
                                   config.neon.lineWidth != mCurrentConfig.neon.lineWidth ||
                                   // filamentFalloff sets how many sigmas the filament
                                   // reaches, so it sizes the quad too (see setupGeometry).
                                   config.neon.filamentFalloff != mCurrentConfig.neon.filamentFalloff ||
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        const bool lutDirty = config.neon.colorStops != mCurrentConfig.neon.colorStops ||
                              config.neon.blendSpace != mCurrentConfig.neon.blendSpace ||
                              config.optimizedNeon.gradientLutSize != mCurrentConfig.optimizedNeon.gradientLutSize;
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
            rebuildLoopSamples(config);
        }

        if (geometryDirty)
        {
            setupGeometry(config);
        }

        if (lutDirty)
        {
            rebuildGradientLUT(config);
        }

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
        mNeonShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_OPTIMIZED_FRAG_SRC,
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
        // Emission pre-pass pair. Both share NEON_VERT_SRC and the same UBO
        // bindings as the single-pass path; only the fragment stage differs.
        mGatherShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                      ShaderSource::NEON_GATHER_FRAG_SRC,
                                      "NeonOptimized.Gather");
        mCompositeShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::NEON_COMPOSITE_FRAG_SRC,
                                         "NeonOptimized.Composite");

        if (!mNeonShader.IsValid() || !mBlackRectShader.IsValid() || !mBlitShader.IsValid() ||
            !mGatherShader.IsValid() || !mCompositeShader.IsValid())
        {
            return false;
        }

        mNeonShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);

        mGatherShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mGatherShader.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mGatherShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);

        // The composite reads no loop samples, but it does read the arc and
        // segment blocks for the pointwise coverages that gate the filament.
        mCompositeShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mCompositeShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        return true;
    }

    void NeonOptimizedRenderer::setupGeometry(const Config &config)
    {
        float scale = config.optimizedNeon.resolutionScale;

        // --- Scaled glow quad (pass 1) ---
        // Size the quad to exactly cover the lit region: rect + earlyOut, so
        // geometry bounds the far region instead of a per-fragment discard
        // (tiler-friendly). Everything here is in scaled (FBO) space -
        // glowRadius*scale is already scaled, matching the uniforms uploaded
        // in Render().
        {
            // Use the SAME early-out factor as the base NeonRenderer so the
            // bloom's wide 1/D tail reaches exactly as far here as it does
            // there - a smaller margin faded the bloom out sooner and made the
            // optimized output look visibly shorter than the base (mismatch).
            // The factor comes from the shared neon-tuning.h.
            //
            // glowRadius ONLY: the old max(..., sampleSpacing * SPACING) term
            // tied the truncation distance to the rect size (and here, to the
            // numSamples slider as well). See the base renderer's setupGeometry
            // for the full rationale.
            //
            // Grow with bloom × intensity, matching the base renderer: the
            // 1/D bloom tail reaches further as those rise. The shader
            // reproduces this exact expression to place its bloom pedestal -
            // keep the two in step.
            float glowReach = config.neon.glowRadius * scale * float(EARLY_OUT_RADIUS_FACTOR) *
                              (1.0f + config.neon.bloomStrength * config.neon.intensity);

            // Filament reach floor - the filament is sized by lineWidth, not
            // glowRadius, so glowRadius = 0 would otherwise give a zero margin
            // and clip the whole exterior. See the base renderer for the full
            // note. FILAMENT_MIN_HALF_WIDTH is a full-res constant, so the
            // whole half-width is taken in full-res px and scaled once.
            // Falloff-aware reach, matching the shaders' reachSigmas. See the
            // base renderer and neon-tuning.h.
            float filN = 2.0f * std::max(config.neon.filamentFalloff, 1e-3f);
            float filSigmas = std::clamp(
                std::pow(std::log2(float(FILAMENT_GAIN) / float(FILAMENT_CUTOFF)), 1.0f / filN),
                float(FILAMENT_REACH_MIN_SIGMAS), float(FILAMENT_REACH_MAX_SIGMAS));
            float filamentReach = std::max(config.neon.lineWidth * 0.5f,
                                           float(FILAMENT_MIN_HALF_WIDTH)) * scale * filSigmas;

            float margin = std::max(glowReach, filamentReach);

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

        // --- Full-res glow quad (emission pre-pass path) ---
        // Same margin formula as above but in FULL-RES px: the gather and the
        // composite both draw this one, the gather through a full-res
        // projection into a smaller viewport. Kept separate from the scaled
        // quad so switching paths at runtime needs no rebuild.
        {
            float glowReach = config.neon.glowRadius * float(EARLY_OUT_RADIUS_FACTOR) *
                              (1.0f + config.neon.bloomStrength * config.neon.intensity);
            float filN = 2.0f * std::max(config.neon.filamentFalloff, 1e-3f);
            float filSigmas = std::clamp(
                std::pow(std::log2(float(FILAMENT_GAIN) / float(FILAMENT_CUTOFF)), 1.0f / filN),
                float(FILAMENT_REACH_MIN_SIGMAS), float(FILAMENT_REACH_MAX_SIGMAS));
            float filamentReach = std::max(config.neon.lineWidth * 0.5f,
                                           float(FILAMENT_MIN_HALF_WIDTH)) * filSigmas;
            float margin = std::max(glowReach, filamentReach);

            if (config.neon.outsideCutoff.enable)
            {
                float outSoft = std::max(config.neon.outsideCutoff.softness,
                                         static_cast<float>(SIDE_SOFT_EPSILON));
                margin = std::min(margin, config.neon.outsideCutoff.size + outSoft + 1.0f);
            }
            mFullResQuadMargin = margin;

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
            mFullResVertexArray.SetVertexData(verts, sizeof(verts));
            mFullResVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
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
        // The emission pre-pass works entirely in FULL-RES px (its quad is
        // drawn through a full-res projection into a smaller viewport), so it
        // wants unscaled sample positions. The single-pass path wants them in
        // FBO space to match its pre-scaled uniforms.
        float scale = config.optimizedNeon.emissionPrePass
                          ? 1.0f
                          : config.optimizedNeon.resolutionScale;
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
    }

    void NeonOptimizedRenderer::rebuildGradientLUT(const Config &config)
    {
        // OnConfigChanged already gates this call behind a lutDirty check
        // (colorStops / blendSpace / gradientLutSize), so a re-entry here
        // always means the inputs actually changed.
        int lutSize = std::max(config.optimizedNeon.gradientLutSize, 4);
        mLUTTarget.resize(lutSize * 4);
        for (int i = 0; i < lutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(lutSize);
            glm::vec3 c = ColorUtils::SampleStops(t, config.neon.colorStops, config.neon.blendSpace);
            mLUTTarget[i * 4 + 0] = c.r;
            mLUTTarget[i * 4 + 1] = c.g;
            mLUTTarget[i * 4 + 2] = c.b;
            mLUTTarget[i * 4 + 3] = 1.0f;
        }

        // First bake (Initialize): seed every buffer and upload immediately -
        // there's nothing to fade from at startup.
        if (!mHasBakedLUT)
        {
            mLUTFrom = mLUTTarget;
            mLUTDisplay = mLUTTarget;
            mLUTBakedSize = lutSize;
            uploadGradientLUT(mLUTDisplay, mLUTBakedSize);
            mHasBakedLUT = true;
            mFading = false;
            return;
        }

        // Snap paths: no fade requested, or the LUT width changed (buffers
        // have different sizes, can't lerp element-wise). Re-seed everything
        // to the target so the next same-size change can fade from here.
        bool sizeChanged = lutSize != mLUTBakedSize;
        if (sizeChanged || config.neon.colorTransitionDuration <= 0.0f)
        {
            mLUTFrom = mLUTTarget;
            mLUTDisplay = mLUTTarget;
            mLUTBakedSize = lutSize;
            uploadGradientLUT(mLUTDisplay, mLUTBakedSize);
            mFading = false;
            return;
        }

        // Fade from whatever is currently on screen (mid-fade or settled)
        // toward the new target. Update() does the first blended upload this
        // same frame (SetConfig -> OnConfigChanged runs before Update).
        mLUTFrom = mLUTDisplay;
        mFadeElapsed = 0.0f;
        mFadeDuration = config.neon.colorTransitionDuration;
        mFading = true;
    }

    void NeonOptimizedRenderer::uploadGradientLUT(const std::vector<float> &lut, int lutSize)
    {
        std::vector<unsigned char> lutBytes(static_cast<size_t>(lutSize) * 4);
        for (int i = 0; i < lutSize * 4; ++i)
        {
            lutBytes[i] = static_cast<unsigned char>(
                std::clamp(lut[i] * 255.0f, 0.0f, 255.0f));
        }

        mGradientLUT.SetData(lutBytes.data(), lutSize, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mGradientLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
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

} // namespace EdgeLighting
