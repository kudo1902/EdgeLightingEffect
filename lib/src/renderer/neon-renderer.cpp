#include "renderer/neon-renderer.h"
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

        /// CPU-side mirror of neon.frag's std140 `LoopSamplesBlock`. std140
        /// pads each vec2 to a 16-byte stride, so we store as vec4 and the
        /// shader reads .xy. Sized by NEON_MAX_LOOP_SAMPLES (neon-tuning.h),
        /// which also sizes the shader's uLoopSamples array.
        typedef struct LoopSamplesBlockData
        {
            glm::vec4 samples[NEON_MAX_LOOP_SAMPLES];
        } LoopSamplesBlockData;

        static_assert(sizeof(LoopSamplesBlockData) == 16 * NEON_MAX_LOOP_SAMPLES,
                      "LoopSamplesBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon.frag's std140 `ArcBlock`. Same layout
        /// pattern as SegmentBlockData: int padded to 16 bytes, then a vec4
        /// per array element (start, length, intensity, hasStops).
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

        /// Width of each segment's row in the segment gradient atlas. Half
        /// the base LUT is enough - a segment's visible span is short so
        /// higher resolution wouldn't be visible; segments also don't wrap
        /// (CLAMP on X), so the extra texels would only pad head/tail.
        constexpr int SEGMENT_LUT_WIDTH = 128;
        /// Width of each arc's row in the arc gradient atlas. Same rationale
        /// as SEGMENT_LUT_WIDTH: an arc's LUT is sampled over the perimeter
        /// hue coordinate (uTime * rate) which cycles slowly, so 128 texels
        /// look identical to 256.
        constexpr int ARC_LUT_WIDTH = 128;
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
        mGradientLUT.Rebuild(mCurrentConfig.neon.colorStops,
                             mCurrentConfig.neon.blendSpace,
                             mCurrentConfig.neon.colorTransitionDuration);
        rebuildSegmentLUT(mCurrentConfig);
        rebuildArcLUT(mCurrentConfig);

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

    void NeonRenderer::Update(float deltaTime, float, const Config &)
    {
        // Drive the gradient cross-fade. Uses the raw frame delta, not clock
        // time, so a colour change still fades smoothly even while the
        // animation clock is paused.
        mGradientLUT.Update(deltaTime);
    }

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
        {
            return;
        }

        // Rect-local -> viewport transform, shared by every pass below. The
        // y-flip lands the config's top-left origin in GL's bottom-left space.
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a). Used
        // for both the opaque black fill and the neon, so the neon composites
        // cleanly over the black. (Blending stays ON the whole time apart from
        // the emission pre-pass, which is a table write, and the LUT strip -
        // toggling GL_BLEND mid-draw is a common cross-driver footgun on mobile
        // GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        packLightBlocks(config);
        renderEmissionPass(time, config, viewportWidth, viewportHeight);

        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(config, center);
        }

        renderNeonPass(config, mvp);

        // Restore a known blend state for following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        // Snapshot dirtiness before we overwrite mCurrentConfig. Each rebuild
        // is gated on the exact set of fields it reads (see the corresponding
        // methods below) - dragging a slider like `bloomStrength` used to
        // re-upload the whole LUT and loop-samples UBO every frame; now only
        // the geometry quad refreshes.
        const bool samplesDirty = config.geometry != mCurrentConfig.geometry;
        const bool geometryDirty = samplesDirty ||
                                   config.neon.glowRadius != mCurrentConfig.neon.glowRadius ||
                                   config.neon.bloomStrength != mCurrentConfig.neon.bloomStrength ||
                                   config.neon.intensity != mCurrentConfig.neon.intensity ||
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        // Only the segments' colour stops + blend space affect the atlas
        // texture; position/length/boost don't (they're read live from the
        // UBO). Cheap deep-compare via mBakedSegments (each SegmentBoost's
        // operator== includes its stops). Compares the merged view so a change
        // in either the transient or preserved pool triggers a re-bake.
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const bool segLutDirty = mEffectiveSegments != mBakedSegments;
        // Same idea for arcs - start/length/intensity ride the UBO, only
        // colorStops + blendSpace changes require a re-bake of the atlas.
        const bool arcLutDirty = config.neon.arcs != mBakedArcs;

        mCurrentConfig = config;
        if (!mShaderProgram.IsValid())
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

        // Self-guarding: GradientLUT::Rebuild compares the stops it last baked
        // and returns without touching GL when nothing that shapes the ring
        // changed (SetConfig fires OnConfigChanged every frame).
        mGradientLUT.Rebuild(config.neon.colorStops, config.neon.blendSpace,
                             config.neon.colorTransitionDuration);

        if (segLutDirty)
        {
            rebuildSegmentLUT(config);
        }

        if (arcLutDirty)
        {
            rebuildArcLUT(config);
        }
    }

    bool NeonRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        // Perimeter emission pre-pass. Reuses the standard neon vertex shader
        // with an identity uMVP over the fullscreen NDC quad; the fragment
        // shader keys off gl_FragCoord.x, not vPos.
        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonRenderer.Emission");
        // Cheap fullscreen black fill, used only by opaque mode. Reuses the
        // standard neon vertex shader (uMVP) so the fill quad respects the
        // viewport.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonRenderer.BlackRect");
        if (!mShaderProgram.IsValid() || !mEmissionShader.IsValid() ||
            !mBlackRectShader.IsValid())
        {
            return false;
        }

        mShaderProgram.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        // The pre-pass reads the same arc/segment blocks off the same binding
        // points; it has no use for the loop-sample positions.
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);

        // Prefer a float target: a SegmentBoost's `boost` is an absolute peak
        // brightness and several segments can stack, so the emission texel's
        // rgb routinely exceeds 1.0. GLES 3.0 only exposes float
        // colour-renderability through an extension, so fall back rather than
        // fail - RGBA8 clamps those highlights but keeps everything else exact.
        mEmissionIsFloat = mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 1,
                                                  GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST);
        if (!mEmissionIsFloat)
        {
            LOG_W("NeonRenderer: RGBA16F emission target unavailable, falling back to RGBA8 "
                  "(segment boosts above 1.0 will clamp).");
            if (!mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 1,
                                        GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
            {
                LOG_E("NeonRenderer: could not create the emission target.");
                return false;
            }
        }
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
        // intensity rise, so grow the quad with them - otherwise a strong bloom
        // gets chopped at a hard rectangular edge, worst on small geometry. The
        // shader still soft-fades the emission to zero at mQuadMargin, so even
        // if this under-estimates there's no hard cutoff.
        margin *= 1.0f + config.neon.bloomStrength * config.neon.intensity;

        // Hard cap: when the outside cutoff is enabled the shader discards
        // emission past size + softness, so there's no point rasterising
        // further. Disabled outside cutoff leaves the natural glowRadius /
        // bloom-driven margin untouched. Add a 1 px safety so the shader's
        // own softmask fades to zero *before* the quad edge and no
        // rectangular seam leaks through.
        if (config.neon.outsideCutoff.enable)
        {
            float outSoft = std::max(config.neon.outsideCutoff.softness,
                                     static_cast<float>(SIDE_SOFT_EPSILON));
            float cutoffCap = config.neon.outsideCutoff.size + outSoft + 1.0f;
            margin = std::min(margin, cutoffCap);
        }
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
        // Uploaded directly to the std140 UBO: vec4[N] where .xy holds the
        // position - raw float32 through the constant cache, no decode step
        // in the shader. (.zw stays 0 - the shader recovers a fragment's
        // continuous perimeter position geometrically from vPos, so the
        // per-sample phase pairs are no longer needed.)
        LoopSamplesBlockData block = {};
        for (int i = 0; i < NEON_MAX_LOOP_SAMPLES; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(NEON_MAX_LOOP_SAMPLES);
            glm::vec2 p = GeometryUtils::GetPointOnRectangle(t, config.geometry);
            block.samples[i] = glm::vec4(p, 0.0f, 0.0f);
        }
        mLoopSamplesBlock.SetData(&block, sizeof(block));

        float w = config.geometry.width;
        float h = config.geometry.height;
        float r = std::max(0.0f, std::min(config.geometry.cornerRadius, std::min(w, h) * 0.5f));

        float perimeter = 2.0f * (w - 2.0f * r) + 2.0f * (h - 2.0f * r) + 2.0f * PI * r;
        mPerimeter = perimeter;
        mSampleSpacing = perimeter / static_cast<float>(NEON_MAX_LOOP_SAMPLES);
    }

    void NeonRenderer::rebuildSegmentLUT(const Config &config)
    {
        // Atlas of size SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS; row `i` holds
        // segment i's baked stops (or zeros if it has none / doesn't exist).
        // The shader treats zeros as "no own colour" via the vec4.w hasStops
        // flag in SegmentBlock, so leaving unused rows zero is safe.
        constexpr int W = SEGMENT_LUT_WIDTH;
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
                continue; // row stays zero; shader falls back to base gradient
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

        // CLAMP on both axes: a segment's gradient runs head-to-tail (no wrap
        // at its own ends), and rows outside [0, segCount) are unused.
        mSegmentLUT.SetData(atlas.data(), W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mSegmentLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

        mBakedSegments = mEffectiveSegments;
    }

    void NeonRenderer::rebuildArcLUT(const Config &config)
    {
        // Atlas of size ARC_LUT_WIDTH x MAX_ARCS; row `i` holds arc i's baked
        // stops (or zeros if it has none / doesn't exist). Same convention as
        // rebuildSegmentLUT - the shader treats zeros as "no own colour" via
        // the vec4.w hasStops flag in ArcBlock, so leaving unused rows zero
        // is safe.
        //
        // REPEAT on U (colours cycle around the perimeter, matching the base
        // gradient's REPEAT wrap so an arc that inherits stops from the same
        // source looks continuous); CLAMP on V (rows outside [0, arcCount)
        // are unused).
        constexpr int W = ARC_LUT_WIDTH;
        constexpr int H = MAX_ARCS;
        std::vector<unsigned char> atlas(W * H * 4, 0);

        const int arcCount = std::min(static_cast<int>(config.neon.arcs.size()),
                                      int(MAX_ARCS));
        for (int a = 0; a < arcCount; ++a)
        {
            const auto &arc = config.neon.arcs[a];
            if (arc.colorStops.empty())
            {
                continue; // row stays zero; shader falls back to base gradient
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

    void NeonRenderer::packLightBlocks(const Config &config)
    {
        // Pack the segment vector as vec4(position, invSigma, boost, hasStops)
        // into the std140 SegmentBlock UBO (DALi-compatible pattern - see
        // neon.frag). Empty vector -> uSegmentCount=0 and the shaders skip the
        // whole feature.
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
            // .w = hasOwnStops flag; the pre-pass reads its colour from row `i`
            // of the segment LUT atlas when set, else falls back to the base
            // gradient at that sample.
            float hasStops = s.colorStops.empty() ? 0.0f : 1.0f;
            segBlock.segments[i] = glm::vec4(s.position, invSigma, s.boost, hasStops);
        }
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));
        mSegmentBlock.BindBase(SEGMENT_BLOCK_BINDING);

        // Pack the arcs vector into ArcBlock: vec4(start, length, intensity,
        // hasStops) per entry. .w picks between the winner arc's own atlas row
        // and the base gradient in the pre-pass's winner-take-all branch.
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

    void NeonRenderer::renderEmissionPass(float time, const Config &config,
                                          int viewportWidth, int viewportHeight)
    {
        // One fragment per perimeter sample. Everything here is a pure function
        // of (sample position, time, config) - see neon-emission.frag for why
        // that lets it come out of the main shader's gather loop.
        //
        // Blending must be off: this is a table write, not a composite.
        glDisable(GL_BLEND);
        mEmissionBuffer.Bind(); // also sets the viewport to NEON_MAX_LOOP_SAMPLES x 1

        mEmissionShader.Use();
        mEmissionShader.SetUniform("uMVP", glm::mat4(1.0f));
        mEmissionShader.SetUniform("uTime", time);
        mEmissionShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mEmissionShader.SetUniform("uIntensity", config.neon.intensity);
        mEmissionShader.SetUniform("uPerimeter", mPerimeter);
        // This renderer always walks the full fixed-size sample set, so sample
        // i sits at perimeter position i / NEON_MAX_LOOP_SAMPLES.
        mEmissionShader.SetUniform("uNumSamples", int(NEON_MAX_LOOP_SAMPLES));

        mGradientLUT.Bind(0);
        mEmissionShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mEmissionShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mEmissionShader.SetUniform("uArcLUT", 2);

        mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mEmissionShader.Unuse();

        // Hand the screen back to the caller in the state every other pass
        // expects: default framebuffer, full viewport, blending on.
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);
        glEnable(GL_BLEND);
    }

    void NeonRenderer::renderOpaqueFill(const Config &config, const glm::vec2 &center)
    {
        // A fullscreen NDC quad (identity MVP); the fragment shader shapes the
        // black coverage from an analytic rounded-box SDF read off gl_FragCoord
        // (highp - exact on Mali/Tizen):
        //   BOTH    -> black everywhere (whole viewport opaque).
        //   INSIDE  -> black only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                    static_cast<float>(SIDE_SOFT_EPSILON));

        mBlackRectShader.Use();
        mBlackRectShader.SetUniform("uMVP", glm::mat4(1.0f));
        mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mBlackRectShader.SetUniform("uRectCenter", center);
        mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
        mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
        mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
        mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlackRectShader.Unuse();
    }

    void NeonRenderer::renderNeonPass(const Config &config, const glm::mat4 &mvp)
    {
        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth);
        mShaderProgram.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mShaderProgram.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mShaderProgram.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mShaderProgram.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mShaderProgram.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);
        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);
        mShaderProgram.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mShaderProgram.SetUniform("uQuadMargin", mQuadMargin);

        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // The three gradient LUTs are consumed by the pre-pass, not here - this
        // shader only needs its output: one texel per sample holding the
        // composed colour and coverage.
        mEmissionBuffer.BindTexture(0);
        mShaderProgram.SetUniform("uEmission", 0);

        // Tight glow quad in both modes - opaque's far region is covered by the
        // black fill, so the gather never runs fullscreen.
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mShaderProgram.Unuse();
    }

}
