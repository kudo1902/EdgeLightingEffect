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
        /// which also sizes the shader's uLoopSamples array - that is the
        /// ceiling; a frame walks only NeonConfig::numSamples of them.
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

        /// Floor for NeonConfig::gradientLutSize. The ring is a texture, so
        /// anything below a handful of texels is meaningless; the default is
        /// 256, which is more than enough for any gradient the human eye can
        /// resolve.
        constexpr int MIN_GRADIENT_LUT_SIZE = 4;
        /// Width of each segment's row in the segment gradient atlas. Fixed
        /// rather than following gradientLutSize: a segment's visible span is
        /// short so higher resolution wouldn't be visible; segments also don't
        /// wrap (CLAMP on X), so the extra texels would only pad head/tail.
        constexpr int SEGMENT_LUT_WIDTH = 128;
        /// Width of each arc's row in the arc gradient atlas. Same rationale
        /// as SEGMENT_LUT_WIDTH: an arc's LUT is sampled over the perimeter
        /// hue coordinate (uTime * rate) which cycles slowly, so 128 texels
        /// look identical to 256.
        constexpr int ARC_LUT_WIDTH = 128;
    }

    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------

    float NeonRenderer::resolutionScale(const Config &config)
    {
        // Above 1 would mean supersampling, which nothing here implements;
        // exactly 1 is the direct-to-backbuffer path (no FBO, no blit).
        return std::clamp(config.neon.resolutionScale, 1.0f / 32.0f, 1.0f);
    }

    int NeonRenderer::sampleCount(const Config &config)
    {
        return std::clamp(config.neon.numSamples, 1, int(NEON_MAX_LOOP_SAMPLES));
    }

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

        // Static fullscreen NDC quad shared by the emission pre-pass, the
        // opaque-mode fill and the blit (all three use an identity MVP and
        // derive their shape from gl_FragCoord / vUV, not aPos).
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

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
        {
            return;
        }

        const float scale = resolutionScale(config);
        const bool scaled = scale < 1.0f;
        const int numSamples = sampleCount(config);

        // Rect-local -> viewport transform at FULL resolution, shared by the
        // backbuffer passes below (the opaque fill and the debug overlays).
        // The y-flip lands the config's top-left origin in GL's bottom-left
        // space. The main pass builds its own transform because it may be
        // drawing into a scaled target.
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a). Used
        // for the opaque fill, the neon and the blit alike, so the neon
        // composites cleanly over the fill. (Blending stays ON the whole time
        // apart from the emission pre-pass, which is a table write, and the LUT
        // strip - toggling GL_BLEND mid-draw is a common cross-driver footgun
        // on mobile GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        packLightBlocks(config);
        renderEmissionPass(time, config, numSamples, viewportWidth, viewportHeight);

        if (scaled)
        {
            // The gather runs into the scaled FBO first; the fill then goes
            // down on the backbuffer at full res (so its rounded corners
            // anti-alias against the real pixel grid) and the blit composites
            // the neon over it.
            renderNeonPass(config, viewportWidth, viewportHeight, numSamples, scale);
            if (config.neon.opaqueMode != OpaqueMode::NONE)
            {
                renderOpaqueFill(config, center);
            }
            renderBlitPass(config);
        }
        else
        {
            if (config.neon.opaqueMode != OpaqueMode::NONE)
            {
                renderOpaqueFill(config, center);
            }
            renderNeonPass(config, viewportWidth, viewportHeight, numSamples, scale);
        }

        if (config.neon.showGradientLUT)
        {
            renderGradientLUTStrip(config, time, mvp);
        }

        if (config.neon.showColorStops && !config.neon.colorStops.empty())
        {
            renderColorStopMarkers(config, proj, center);
        }

        // Restore a known blend state for following renderers (the LUT strip
        // overlay disables blending).
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
        const bool samplesDirty = config.geometry != mCurrentConfig.geometry ||
                                  config.neon.resolutionScale != mCurrentConfig.neon.resolutionScale ||
                                  config.neon.numSamples != mCurrentConfig.neon.numSamples;
        const bool geometryDirty = samplesDirty ||
                                   config.neon.glowRadius != mCurrentConfig.neon.glowRadius ||
                                   config.neon.bloomStrength != mCurrentConfig.neon.bloomStrength ||
                                   config.neon.intensity != mCurrentConfig.neon.intensity ||
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        const bool lutDirty = config.neon.colorStops != mCurrentConfig.neon.colorStops ||
                              config.neon.blendSpace != mCurrentConfig.neon.blendSpace ||
                              config.neon.gradientLutSize != mCurrentConfig.neon.gradientLutSize;
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
        // Main pass. The gather's trip count rides the uNumSamples uniform,
        // so NeonConfig::numSamples moves at runtime with no recompile and no
        // second program - see the header of neon.frag for why the unroll in
        // there makes the uniform bound cost nothing.
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        // Perimeter emission pre-pass. Reuses the standard neon vertex shader
        // with an identity uMVP over the fullscreen NDC quad; the fragment
        // shader keys off gl_FragCoord.x, not vPos.
        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonRenderer.Emission");
        // Cheap fullscreen fill, used only by opaque mode. The analytic SDF in
        // the fragment shader shapes the silhouette with softness-aware
        // feathering, so rounded corners anti-alias cleanly. Reuses the
        // standard neon vertex shader (uMVP) so the fill quad respects the
        // viewport.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonRenderer.BlackRect");
        // Scaled-path upscale. Only used when resolutionScale < 1, but built
        // unconditionally: the scale is a live config value, so the program
        // has to be ready before the frame that first lowers it.
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonRenderer.Blit");
        // Debug LUT strip - reuses the standard neon vertex shader (uMVP + aPos → vPos)
        // so the strip quad respects the same rect-local transform as the glow quad.
        mLUTDebugShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                        "NeonRenderer.LUTDebug");
        // Debug stop markers - same vertex shader, filled-disc fragment.
        mStopMarkerShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                          ShaderSource::NEON_STOP_MARKER_FRAG_SRC,
                                          "NeonRenderer.StopMarker");
        if (!mShaderProgram.IsValid() ||
            !mEmissionShader.IsValid() || !mBlackRectShader.IsValid() ||
            !mBlitShader.IsValid() ||
            !mLUTDebugShader.IsValid() || !mStopMarkerShader.IsValid())
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
        // Everything below is in render-target space: at resolutionScale < 1
        // the quad, the margin and mSampleSpacing are all pre-multiplied by
        // the scale, matching the uniforms renderNeonPass uploads. At scale 1
        // the multiplications are identities.
        const float scale = resolutionScale(config);

        // Size the quad to cover the lit region: rect + earlyOut. Beyond this the
        // halo/bloom are < ~1% at default strength, so geometry bounds the far
        // region instead of a per-fragment discard (tiler-friendly).
        // Factors come from the shared neon-tuning.h (also fed to the shaders).
        float margin = std::max(config.neon.glowRadius * scale * float(EARLY_OUT_RADIUS_FACTOR),
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
        // bloom-driven margin untouched. Cutoff sizes are unscaled pixels, so
        // multiply by `scale`; add a 1 px safety so the shader's own softmask
        // fades to zero *before* the quad edge and no rectangular seam leaks
        // through.
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

        mVertexArray.SetVertexData(verts, sizeof(verts));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        // Debug LUT strip: 60% of rect width × min(rect_height / 6, 40 px),
        // centred on the geometry origin so it sits inside the rounded box.
        // Drawn on the backbuffer, so this one stays in full-res pixels.
        float stripHalfW = config.geometry.width * 0.5f * 0.6f;
        float stripHalfH = std::min(config.geometry.height * 0.5f / 6.0f, 20.0f);
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
        const float scale = resolutionScale(config);
        const int n = sampleCount(config);

        // Evenly spaced points (by arc length) around the rounded-rect perimeter.
        // Drives the additive halo/spill/colour gather in the fragment shader.
        // Uploaded directly to the std140 UBO: vec4[N] where .xy holds the
        // position in render-target pixels - raw float32 through the constant
        // cache, no decode step in the shader. (.zw stays 0 - the shader
        // recovers a fragment's continuous perimeter position geometrically
        // from vPos, so the per-sample phase pairs are no longer needed.)
        //
        // Only the first n slots are in use; the rest stay at (0,0,0,0) and are
        // never read, because the shader's loop bound stops before them.
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

    void NeonRenderer::rebuildGradientLUT(const Config &config)
    {
        // Bake the entire colour ring on CPU into mLUTTarget; the shader then
        // becomes colour-stop-agnostic. Keeps HSV-vs-RGB blend cost off the GPU
        // hot path.
        const int lutSize = std::max(config.neon.gradientLutSize, MIN_GRADIENT_LUT_SIZE);
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
            mTargetStops = config.neon.colorStops;
            mTargetBlendSpace = config.neon.blendSpace;
            mHasBakedLUT = true;
            mFading = false;
            return;
        }

        // OnConfigChanged fires every frame with an unchanged config, so only
        // (re)start a fade when the gradient inputs actually changed.
        const bool sizeChanged = lutSize != mLUTBakedSize;
        bool inputsChanged = sizeChanged ||
                             config.neon.blendSpace != mTargetBlendSpace ||
                             config.neon.colorStops != mTargetStops;
        if (!inputsChanged)
        {
            return;
        }
        mTargetStops = config.neon.colorStops;
        mTargetBlendSpace = config.neon.blendSpace;

        // Snap paths: no cross-fade requested, or the LUT width changed (the
        // buffers have different sizes, so there is nothing to lerp
        // element-wise). Re-seed everything to the target so the next
        // same-size change can fade from here.
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

    void NeonRenderer::uploadGradientLUT(const std::vector<float> &lut, int lutSize)
    {
        // Edge devices often lack float-texture support; pack into ubyte RGBA8.
        std::vector<unsigned char> lutBytes(static_cast<size_t>(lutSize) * 4);
        for (int i = 0; i < lutSize * 4; ++i)
        {
            lutBytes[i] = static_cast<unsigned char>(
                std::clamp(lut[i] * 255.0f, 0.0f, 255.0f));
        }

        // 1-row 2D texture (sampled with v = 0.5 in the shader). REPEAT on
        // the U axis lets the gradient sweep wrap naturally; the V axis is a
        // single row, so CLAMP is fine.
        mGradientLUT.SetData(lutBytes.data(), lutSize, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
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
        // whole feature. Segment `position` is a normalised perimeter coord in
        // [0, 1), so the resolution scale does not apply here.
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
        // and the base gradient in the pre-pass's winner-take-all branch. Arc
        // start/length are normalised perimeter coords, so again unscaled.
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

    void NeonRenderer::renderEmissionPass(float time, const Config &config, int numSamples,
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
        // Render-target-space perimeter, matching the (possibly scaled)
        // uRectSize the main pass uses for its own feather conversion.
        mEmissionShader.SetUniform("uPerimeter", mPerimeter);
        // Sample i sits at perimeter position i / numSamples, matching the
        // walk in rebuildLoopSamples.
        mEmissionShader.SetUniform("uNumSamples", numSamples);

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
        // fill coverage from an analytic rounded-box SDF read off gl_FragCoord
        // (highp - exact on Mali/Tizen):
        //   BOTH    -> filled everywhere (whole viewport opaque).
        //   INSIDE  -> filled only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        // Rounded corners AA cleanly via fwidth(d) - no discard, no stair-step.
        //
        // Always drawn on the backbuffer in FULL-res gl_FragCoord space, so
        // nothing here is scaled even when the neon pass is.
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

    void NeonRenderer::renderNeonPass(const Config &config, int viewportWidth,
                                      int viewportHeight, int numSamples, float scale)
    {
        const bool scaled = scale < 1.0f;

        // Render-target size: the backbuffer, or the scaled FBO.
        int bufW = viewportWidth;
        int bufH = viewportHeight;
        if (scaled)
        {
            bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
            bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

            mScaledBuffer.Resize(bufW, bufH);
            mScaledBuffer.Bind();

            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Premultiplied "over" into the transparent FBO: a single
            // non-overlapping quad over (0,0,0,0) leaves the FBO holding the
            // shader's premultiplied colour + coverage alpha, ready to be
            // composited over the backbuffer by the blit.
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        // Everything below is in render-target space - the rect, the line
        // width, the cutoffs and mSampleSpacing are all pre-multiplied by
        // `scale`, so the shader never has to know the resolution it is
        // running at. At scale 1 every multiplication is an identity and this
        // reduces to the plain full-res transform.
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(bufW), 0.0f, static_cast<float>(bufH), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        center *= scale;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width * scale, config.geometry.height * scale));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius * scale);
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mShaderProgram.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        mShaderProgram.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff) * scale);
        mShaderProgram.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        mShaderProgram.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff) * scale);
        mShaderProgram.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);
        mShaderProgram.SetUniform("uSampleSpacing", mSampleSpacing);
        mShaderProgram.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mShaderProgram.SetUniform("uQuadMargin", mQuadMargin);
        mShaderProgram.SetUniform("uNumSamples", numSamples);

        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point in
        // render-target pixels.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // The three gradient LUTs are consumed by the pre-pass, not here - this
        // shader only needs its output: one texel per sample holding the
        // composed colour and coverage.
        mEmissionBuffer.BindTexture(0);
        mShaderProgram.SetUniform("uEmission", 0);

        // Tight glow quad in both modes - opaque's far region is covered by the
        // fill, so the gather never runs fullscreen.
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mShaderProgram.Unuse();

        if (scaled)
        {
            Framebuffer::BindDefault();
            glViewport(0, 0, viewportWidth, viewportHeight);
        }
    }

    void NeonRenderer::renderBlitPass(const Config &config)
    {
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read that composites over whatever's on
        // the backbuffer (the opaque fill if enabled, original bg otherwise).
        mBlitShader.Use();
        mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));

        // Debug toggle: nearest neighbour shows the raw low-res pixels.
        glBindTexture(GL_TEXTURE_2D, mScaledBuffer.GetTextureId());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        config.neon.showHalfRes ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        config.neon.showHalfRes ? GL_NEAREST : GL_LINEAR);

        mScaledBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlitShader.Unuse();
    }

    void NeonRenderer::renderGradientLUTStrip(const Config &config, float time, const glm::mat4 &mvp)
    {
        // Overwrites the neon output within the strip rect so the baked ring is
        // readable regardless of the glow's tone-mapped brightness.
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

    void NeonRenderer::renderColorStopMarkers(const Config &config, const glm::mat4 &proj,
                                              const glm::vec2 &center)
    {
        // Draws a filled disc in each stop's colour at its perimeter position,
        // so the raw (position, colour) inputs can be checked against the LUT
        // strip and the on-screen glow. Uses standard alpha blending for the
        // ring / anti-aliased edge to composite cleanly.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Scale marker with the smaller half-extent so it stays inside the
        // rect on very tall/thin geometries; cap at 12 px so it's not huge
        // on large rects.
        float markerRadius = std::min(std::min(config.geometry.width, config.geometry.height) * 0.5f * 0.06f, 12.0f);
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

}
