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
        /// Resolves a @c Cutoff to its effective pixel size for the shader.
        /// Disabled cutoffs collapse to a value large enough that the shader's
        /// discard / softmask branches never fire on realistic geometry - one
        /// value shared with the black-rect fill so both agree on where the
        /// "no cap" boundary sits.
        constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;

        inline float ResolveCutoffSize(const Cutoff &c)
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

        /// Width of the precomputed colour-ring LUT texture (RGBA8, REPEAT
        /// wrap). 256 is more than enough for any gradient the human eye can
        /// resolve.
        constexpr int GRADIENT_LUT_SIZE = 256;
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
        rebuildGradientLUT(mCurrentConfig);
        rebuildSegmentLUT(mCurrentConfig);
        rebuildArcLUT(mCurrentConfig);

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

    void NeonRenderer::Update(float deltaTime, float, const Config &)
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

        const int n = GRADIENT_LUT_SIZE * 4;
        mLUTDisplay.resize(n);
        for (int i = 0; i < n; ++i)
        {
            mLUTDisplay[i] = mLUTFrom[i] + (mLUTTarget[i] - mLUTFrom[i]) * s;
        }
        uploadGradientLUT(mLUTDisplay);

        if (u >= 1.0f)
        {
            mLUTDisplay = mLUTTarget; // land exactly on the target
            mFading = false;
        }
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
        // cleanly over the black. (Blending stays ON the whole time - toggling
        // GL_BLEND mid-draw is a common cross-driver footgun on mobile GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Opaque-mode black background pass -----------------------------
        // A fullscreen NDC quad (identity MVP); the fragment shader shapes the
        // black coverage from an analytic rounded-box SDF read off gl_FragCoord
        // (highp - exact on Mali/Tizen):
        //   BOTH    -> black everywhere (whole viewport opaque).
        //   INSIDE  -> black only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            mBlackRectShader.Use();
            mBlackRectShader.SetUniform("uMVP", glm::mat4(1.0f));
            mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
            mBlackRectShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
            mBlackRectShader.SetUniform("uRectCenter", center);
            float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                        static_cast<float>(SIDE_SOFT_EPSILON));
            mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
            mBlackRectShader.SetUniform("uInsideCutoff", ResolveCutoffSize(config.neon.insideCutoff));
            mBlackRectShader.SetUniform("uOutsideCutoff", ResolveCutoffSize(config.neon.outsideCutoff));
            mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
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
        mShaderProgram.SetUniform("uInsideCutoff", ResolveCutoffSize(config.neon.insideCutoff));
        mShaderProgram.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mShaderProgram.SetUniform("uOutsideCutoff", ResolveCutoffSize(config.neon.outsideCutoff));
        mShaderProgram.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);
        // Pack the segment vector as vec3(position, invSigma, boost) into the
        // std140 SegmentBlock UBO (DALi-compatible pattern - see neon.frag).
        // Empty vector → uSegmentCount=0 and the shader skips the whole feature.
        SegmentBlockData segBlock = {};
        int segCount = std::min(static_cast<int>(config.neon.segmentBoosts.size()),
                                int(MAX_SEGMENT_BOOSTS));
        segBlock.count = segCount;
        for (int i = 0; i < segCount; ++i)
        {
            const auto &s = config.neon.segmentBoosts[i];
            float invSigma = 1.0f / std::max(s.length * 0.5f, 1e-3f);
            // .w = hasOwnStops flag; the shader reads its colour from row `i`
            // of the segment LUT atlas when set, else falls back to the base
            // gradient at that sample.
            float hasStops = s.colorStops.empty() ? 0.0f : 1.0f;
            segBlock.segments[i] = glm::vec4(s.position, invSigma, s.boost, hasStops);
        }
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));
        mSegmentBlock.BindBase(SEGMENT_BLOCK_BINDING);

        // Pack the arcs vector into ArcBlock: vec4(start, length, intensity,
        // hasStops) per entry. .w picks between the winner arc's own atlas row
        // and the base gradient in the shader's winner-take-all branch.
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

        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);

        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // Bind the precomputed gradient LUT to texture unit 0. The shader
        // pulls per-sample colour from this in a single texture() call.
        mGradientLUT.Bind(0);
        mShaderProgram.SetUniform("uGradientLUT", 0);
        // Per-segment gradient atlas on unit 1; sampled by the segment inner
        // loop only when a segment's hasStops flag is set.
        mSegmentLUT.Bind(1);
        mShaderProgram.SetUniform("uSegmentLUT", 1);
        // Per-arc gradient atlas on unit 2; sampled by the arc winner branch
        // only when the winning arc has stops (see ArcBlock's vec4.w).
        mArcLUT.Bind(2);
        mShaderProgram.SetUniform("uArcLUT", 2);
        mShaderProgram.SetUniform("uQuadMargin", mQuadMargin);

        // Tight glow quad in both modes - opaque's far region is covered by the
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
        const bool lutDirty = config.neon.colorStops != mCurrentConfig.neon.colorStops ||
                              config.neon.blendSpace != mCurrentConfig.neon.blendSpace;
        // Only the segments' colour stops + blend space affect the atlas
        // texture; position/length/boost don't (they're read live from the
        // UBO). Cheap deep-compare via mBakedSegments (each SegmentBoost's
        // operator== includes its stops).
        const bool segLutDirty = config.neon.segmentBoosts != mBakedSegments;
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
        // Debug LUT strip - reuses the standard neon vertex shader (uMVP + aPos → vPos)
        // so the strip quad respects the same rect-local transform as the glow quad.
        mLUTDebugShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                        "NeonRenderer.LUTDebug");
        // Debug stop markers - same vertex shader, filled-disc fragment.
        mStopMarkerShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                          ShaderSource::NEON_STOP_MARKER_FRAG_SRC,
                                          "NeonRenderer.StopMarker");
        if (!mShaderProgram.IsValid() || !mBlackRectShader.IsValid() ||
            !mLUTDebugShader.IsValid() || !mStopMarkerShader.IsValid())
        {
            return false;
        }

        mShaderProgram.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
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
        // Uploaded directly to the std140 UBO: vec4[N] where .xy holds the
        // position - raw float32 through the constant cache, no decode step
        // in the shader.
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
        mSampleSpacing = perimeter / static_cast<float>(NEON_MAX_LOOP_SAMPLES);
    }

    void NeonRenderer::rebuildGradientLUT(const Config &config)
    {
        // Bake the entire colour ring on CPU into mLUTTarget; the shader then
        // becomes colour-stop-agnostic. Keeps HSV-vs-RGB blend cost off the GPU
        // hot path.
        mLUTTarget.resize(GRADIENT_LUT_SIZE * 4);
        for (int i = 0; i < GRADIENT_LUT_SIZE; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(GRADIENT_LUT_SIZE);
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
            uploadGradientLUT(mLUTDisplay);
            mTargetStops = config.neon.colorStops;
            mTargetBlendSpace = config.neon.blendSpace;
            mHasBakedLUT = true;
            mFading = false;
            return;
        }

        // OnConfigChanged fires every frame with an unchanged config, so only
        // (re)start a fade when the gradient inputs actually changed.
        bool inputsChanged = config.neon.blendSpace != mTargetBlendSpace ||
                             config.neon.colorStops != mTargetStops;
        if (!inputsChanged)
        {
            return;
        }
        mTargetStops = config.neon.colorStops;
        mTargetBlendSpace = config.neon.blendSpace;

        // Instant path: no cross-fade requested - snap the display to target.
        if (config.neon.colorTransitionDuration <= 0.0f)
        {
            mLUTDisplay = mLUTTarget;
            uploadGradientLUT(mLUTDisplay);
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

    void NeonRenderer::uploadGradientLUT(const std::vector<float> &lut)
    {
        // Edge devices often lack float-texture support; pack into ubyte RGBA8.
        std::vector<unsigned char> lutBytes(GRADIENT_LUT_SIZE * 4);
        for (int i = 0; i < GRADIENT_LUT_SIZE * 4; ++i)
        {
            lutBytes[i] = static_cast<unsigned char>(
                std::clamp(lut[i] * 255.0f, 0.0f, 255.0f));
        }

        // 1-row 2D texture (sampled with v = 0.5 in the shader). REPEAT on
        // the U axis lets the gradient sweep wrap naturally; the V axis is a
        // single row, so CLAMP is fine.
        mGradientLUT.SetData(lutBytes.data(), GRADIENT_LUT_SIZE, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        mGradientLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
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

        const int segCount = std::min(static_cast<int>(config.neon.segmentBoosts.size()),
                                      int(MAX_SEGMENT_BOOSTS));
        for (int s = 0; s < segCount; ++s)
        {
            const auto &seg = config.neon.segmentBoosts[s];
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

        mBakedSegments = config.neon.segmentBoosts;
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
}
