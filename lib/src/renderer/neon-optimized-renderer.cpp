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

        /// Packs @c uArcs[].w for arc @p i: bit 0 = the arc has its own colour
        /// stops, bit 1 = another arc covers the perimeter immediately BEFORE
        /// its start, bit 2 = another arc covers it immediately AFTER its end.
        ///
        /// The abutment bits choose each endpoint's feather direction in the
        /// shaders' @c arcCoverContinuous, and getting that per-endpoint is what
        /// lets two arcs tile the ring without a seam notch while a lone arc
        /// still lights nothing outside its own span. Both properties matter:
        /// see the long note in neon.frag, and in particular why a symmetric
        /// feather cannot satisfy both at @c cornerRadius 0.
        ///
        /// It is a pure function of the arc set, so it is resolved here, once
        /// per frame, rather than by an O(arcs^2) scan in every fragment.
        ///
        /// @p arcs is the list as packed - only the first @p count entries are
        /// visible to the shader, so only they can abut.
        inline float PackArcFlags(const std::vector<Arc> &arcs, int i, int count)
        {
            // Perimeter-fraction slop. Endpoints that are meant to coincide are
            // usually authored as exact values or driven by an animation, so
            // this only has to absorb float round-trip error.
            constexpr float EPS = 1e-5f;
            const Arc &a = arcs[i];
            float flags = a.colorStops.empty() ? 0.0f : 1.0f;

            float end = a.start + a.length;
            bool tailAbuts = false;
            bool headAbuts = false;
            for (int b = 0; b < count; ++b)
            {
                if (b == i)
                {
                    continue;
                }
                const Arc &o = arcs[b];
                // A dark arc is skipped by the shader's coverage loop, so it
                // cannot take over a neighbour's endpoint either.
                if (o.length <= 0.0f || o.intensity <= 0.0f)
                {
                    continue;
                }
                // Where a.start falls within o, measured forward from o.start.
                float rTail = a.start - o.start;
                rTail -= std::floor(rTail);
                // Covers strictly BEFORE a.start: rTail must be past o's start
                // (rTail > 0 excludes two arcs that merely share a start point)
                // and no further than its end.
                if (rTail > EPS && rTail <= o.length + EPS)
                {
                    tailAbuts = true;
                }
                // Where a's end falls within o. Covers strictly AFTER it when
                // the end lands inside o but not exactly on o's own end - an
                // arc finishing where this one finishes extends nothing.
                float rHead = end - o.start;
                rHead -= std::floor(rHead);
                if (rHead < o.length - EPS)
                {
                    headAbuts = true;
                }
            }
            if (tailAbuts)
            {
                flags += 2.0f;
            }
            if (headAbuts)
            {
                flags += 4.0f;
            }
            return flags;
        }

        /// Quantise a [0, 1] colour channel to 8 bits, rounding to nearest.
        ///
        /// The `+ 0.5f` is the whole point. Without it the cast truncates, so
        /// every baked texel lands up to 1 LSB low and about 0.5 LSB low on
        /// average, across all three LUTs - a systematic darkening of the
        /// authored colours. GL's own float-to-unorm conversion rounds, so
        /// truncating here was the CPU bake disagreeing with the hardware it
        /// feeds. The clamp comes after the bias so 1.0 still maps to 255.
        inline unsigned char ToByte(float v)
        {
            return static_cast<unsigned char>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        /// Whether the atlas baked from @p baked would differ from one baked
        /// from @p current - i.e. whether the atlas is dirty and must be
        /// re-baked. Named for the `*Dirty` flags at the call site, which is
        /// the pattern every renderer here gates its rebuilds on.
        ///
        /// Row `i` of the atlas is a pure function of `[i].colorStops` and
        /// `[i].blendSpace` (see rebuildArcLUT / rebuildSegmentLUT) - those are
        /// the only fields a re-bake depends on. An arc's `start`, `length` and
        /// `intensity`, and a segment's `position`, `length` and `boost`, ride
        /// the UBOs and are re-uploaded every frame regardless.
        ///
        /// That distinction is the whole point. Comparing whole structs - which
        /// is what `arcs != mBakedArcs` did - re-baked and re-uploaded the atlas
        /// on every ArcWipe / OutlineTracer / SegmentTravel frame, because those
        /// animations write exactly the live fields and the structs'
        /// `operator==` includes them.
        ///
        /// The comparison is POSITIONAL, not set-wise: the atlas is indexed by
        /// arc / segment index, so swapping two entries swaps their rows even
        /// though the collection of stops is unchanged.
        inline bool IsAtlasDirty(const std::vector<Arc> &baked, const std::vector<Arc> &current)
        {
            if (baked.size() != current.size())
            {
                return true;
            }
            for (size_t i = 0; i < baked.size(); ++i)
            {
                if (baked[i].blendSpace != current[i].blendSpace ||
                    baked[i].colorStops != current[i].colorStops)
                {
                    return true;
                }
            }
            return false;
        }

        /// Segment overload of @ref IsAtlasDirty - same rule, same reasoning.
        inline bool IsAtlasDirty(const std::vector<SegmentBoost> &baked,
                                 const std::vector<SegmentBoost> &current)
        {
            if (baked.size() != current.size())
            {
                return true;
            }
            for (size_t i = 0; i < baked.size(); ++i)
            {
                if (baked[i].blendSpace != current[i].blendSpace ||
                    baked[i].colorStops != current[i].colorStops)
                {
                    return true;
                }
            }
            return false;
        }

        /// Warn once when a host hands over more arcs / segments than the
        /// shader arrays can hold - see NeonRenderer's copy for the rationale.
        /// @p latched is the caller's per-renderer flag, cleared once the count
        /// falls back under the cap.
        inline void WarnOnOverflow(const char *what, const char *renderer,
                                   size_t requested, int cap, bool &latched)
        {
            if (static_cast<int>(requested) <= cap)
            {
                latched = false;
                return;
            }
            if (latched)
            {
                return;
            }
            latched = true;
            LOG_W("%s: %zu %s configured but only %d fit - the rest are ignored.",
                  renderer, requested, what, cap);
        }
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

    void NeonOptimizedRenderer::packLightBlocks(const Config &config)
    {
        // Read by both the emission pre-pass and the half-res neon pass, so
        // packed once before either draws. See NeonRenderer::packLightBlocks.
        SegmentBlockData segBlock = {};
        // mEffectiveSegments is NOT refilled here. OnConfigChanged fills it
        // whenever the composited config changes, and this runs once per frame
        // from Render - so on a frame where nothing changed the merged view is
        // already current, and on a frame where something did, OnConfigChanged
        // has already run (Update -> refreshActiveConfig precedes Render).
        // Refilling was the third FillEffectiveSegments of the same frame.
        const std::vector<SegmentBoost> &effSegments = mEffectiveSegments;
        WarnOnOverflow("segments", "NeonOptimizedRenderer", effSegments.size(),
                       int(MAX_SEGMENT_BOOSTS), mSegmentOverflowLogged);
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
        WarnOnOverflow("arcs", "NeonOptimizedRenderer", config.neon.arcs.size(),
                       int(MAX_ARCS), mArcOverflowLogged);
        int arcCount = std::min(static_cast<int>(config.neon.arcs.size()),
                                int(MAX_ARCS));
        arcBlock.count = arcCount;
        for (int i = 0; i < arcCount; ++i)
        {
            const auto &a = config.neon.arcs[i];
            // .w is a bitmask, not just hasStops - see PackArcFlags.
            float flags = PackArcFlags(config.neon.arcs, i, arcCount);
            arcBlock.arcs[i] = glm::vec4(a.start, a.length, a.intensity, flags);
        }
        mArcBlock.SetData(&arcBlock, sizeof(arcBlock));
        mArcBlock.BindBase(ARC_BLOCK_BINDING);
    }

    bool NeonOptimizedRenderer::renderEmissionPass(int viewportWidth, int viewportHeight,
                                                   float time, const Config &config)
    {
        // Identical contract to NeonRenderer::renderEmissionPass - see there
        // for why RGBA16F / GL_NEAREST. The table is in perimeter-parameter
        // space, so resolutionScale does NOT apply to it; only the sample
        // count matters, and it must match the main pass's loop bound.
        //
        // Same restore rule as NeonRenderer::renderEmissionPass: put back the
        // target that was handed in, not framebuffer 0, and read it BEFORE the
        // resize below. Both of this renderer's later passes rebind explicitly,
        // so 0 happens to be harmless here - but leaving it would make the two
        // copies disagree on the invariant.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        // The fallback LATCHES - see NeonRenderer::renderEmissionPass for why.
        if (!mEmissionFloatUnavailable &&
            !mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 2,
                                    GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST))
        {
            mEmissionFloatUnavailable = true;
            LOG_W("NeonOptimizedRenderer: RGBA16F emission target unavailable, using RGBA8. "
                  "Arc intensities and stacked segment boosts above 1.0 will clamp.");
        }
        if (mEmissionFloatUnavailable &&
            !mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 2,
                                    GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
        {
            // No target, and no stale one either - Framebuffer::Resize destroys
            // the attachment when it fails. See NeonRenderer::renderEmissionPass.
            return false;
        }

        mEmissionBuffer.Bind();

        mEmissionShader.Use();
        mEmissionShader.SetUniform("uMVP", glm::mat4(1.0f));
        mEmissionShader.SetUniform("uTime", time);
        mEmissionShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mEmissionShader.SetUniform("uNumSamples", std::max(1, std::min(config.optimizedNeon.numSamples,
                                                                       NEON_MAX_LOOP_SAMPLES)));
        mGradientLUT.Bind(0);
        mEmissionShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mEmissionShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mEmissionShader.SetUniform("uArcLUT", 2);
        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mEmissionShader.Unuse();

        // Hand the framebuffer and viewport back exactly as found. Blend mode
        // is untouched here - it is a phase property owned by Render.
        Framebuffer::BindId(targetFbo);
        glViewport(0, 0, viewportWidth, viewportHeight);
        return true;
    }

    bool NeonOptimizedRenderer::renderHalfResNeonPass(int viewportHeight, int bufW, int bufH,
                                                      float time, const Config &config)
    {
        // Everything below is in FBO space: the transform, the rect size and
        // every pixel-valued uniform are pre-multiplied by `scale`, and the
        // shader converts neon-tuning.h's full-res constants with the same
        // factor via uResolutionScale.
        const float scale = config.optimizedNeon.resolutionScale;
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;

        // --- Pass 1: render neon to scaled FBO ---
        // Resize destroys the attachment on its failure path, so a failure
        // leaves mHalfResBuffer holding id 0 - and Bind() would then bind the
        // CALLER'S framebuffer, whereupon the glClear below erases everything
        // already drawn this frame (glClear is not clipped by the viewport).
        // Under an OffscreenCapture that target is the capture. Bail instead;
        // Render skips Pass 2b with us. Same contract as renderEmissionPass.
        // The filter is requested through Resize, which is the ONLY writer of
        // the tracked value, so it cannot drift. Setting it on the texture
        // afterwards instead - which is what the debug toggle used to do - left
        // mFilter saying LINEAR while the texture was NEAREST, so the next
        // frame's Resize saw a mismatch and destroyed and recreated the FBO.
        // Measured at one reallocation per frame for as long as showHalfRes was
        // on, against one for the whole run. A filter change still costs one
        // reallocation here, but only on the frame the toggle actually moves.
        const GLint filter = config.optimizedNeon.showHalfRes ? GL_NEAREST : GL_LINEAR;
        if (!mHalfResBuffer.Resize(bufW, bufH, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, filter))
        {
            return false;
        }
        mHalfResBuffer.Bind();

        // The clear colour is global GL state, so put it back: a host that
        // sets its own once at startup would otherwise find it silently
        // replaced with transparent black by whichever frame ran this pass.
        // The query costs the same as the GetBoundId one above - once per
        // frame, and for the same reason: this renderer does not own the
        // context it is drawing into.
        GLfloat prevClear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);

        mNeonShader.Use();

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
        mNeonShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry) * scale);
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mNeonShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mNeonShader.SetUniform("uIntensity", config.neon.intensity);
        mNeonShader.SetUniform("uTime", time);
        mNeonShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mNeonShader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mNeonShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mNeonShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mNeonShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        mNeonShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff) * scale);
        mNeonShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        mNeonShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff) * scale);
        mNeonShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);
        // Pack the segment vector as vec3(position, invSigma, boost) into the
        // std140 SegmentBlock UBO (DALi-compatible pattern - see the shader).
        // Same packing as NeonRenderer; segment `position` is a normalised
        // perimeter coord in [0, 1), so the resolutionScale does not apply.

        mNeonShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        mNeonShader.SetUniform("uQuadMargin", mQuadMargin);

        // Loop sample positions from the LoopSamplesBlock UBO (see the shader)
        // - raw float32 vec4[N], .xy holds the perimeter point in FBO pixels.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniform("uNumSamples", std::max(1, std::min(config.optimizedNeon.numSamples,
                                                                   NEON_MAX_LOOP_SAMPLES)));
        // Emission table from pass 0 on unit 3.
        mEmissionBuffer.BindTexture(3);
        mNeonShader.SetUniform("uEmission", 3);

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
        return true;
    }

    void NeonOptimizedRenderer::renderOpaqueFill(int viewportHeight, const Config &config)
    {
        // A single NDC quad + identity MVP; the black-rect fragment shader
        // shapes the silhouette from the analytic rounded-box SDF read off
        // gl_FragCoord, with softness-aware feathering:
        //   BOTH    -> whole viewport opaque black.
        //   INSIDE  -> black only where d <= softEdge; off-side stays clear.
        //   OUTSIDE -> mirror of INSIDE.
        // Rounded corners AA cleanly via fwidth(d) - no discard, no stair-step.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        const glm::mat4 identity(1.0f);

        // Rect centre in full-res gl_FragCoord space (y-up).
        glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        mBlackRectShader.Use();
        mBlackRectShader.SetUniform("uMVP", identity);
        mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlackRectShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
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

    void NeonOptimizedRenderer::renderBlitPass(const Config &config)
    {
        const glm::mat4 identity(1.0f);

        // --- Pass 2b: bilinear composite of the half-res neon FBO ---
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read that composites over whatever's on
        // the backbuffer (black fill if opaque, original bg otherwise).
        mBlitShader.Use();
        mBlitShader.SetUniform("uMVP", identity);

        // Just bind it. The showHalfRes filter is requested through Resize in
        // pass 1, so this pass sets no texture parameters at all - and the bare
        // glBindTexture that used to sit here, on whatever unit the previous
        // pass left active, is gone with it.
        mHalfResBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mBlitShader.Unuse();
    }

    void NeonOptimizedRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.optimizedNeon.enable)
        {
            return;
        }

        // Render is a pass schedule and nothing else - see NeonRenderer::Render
        // for the same shape. Each render*Pass owns its shader; this function
        // owns the blend state and the framebuffer hand-back.
        const float scale = config.optimizedNeon.resolutionScale;
        const int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        const int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // Debug: render the opaque fill and nothing else. Skips the whole
        // half-res gather (Pass 1) and the composite that would bring it back
        // (Pass 2b), so the backbuffer ends up holding the fill silhouette
        // alone - the same view NeonRenderer gives, for A/B against it.
        const bool opaqueOnly = config.neon.opaqueOnly;

        // The target this renderer was handed. Usually the window's default
        // framebuffer, but an offscreen frame capture (@ref OffscreenCapture)
        // binds a real FBO, so the backbuffer passes have to come back to
        // whatever was bound rather than assuming 0. Read BEFORE Pass 0, which
        // binds an FBO of its own - querying after would capture that instead.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        // Cleared if pass 0 cannot allocate an emission target: pass 1 would
        // then have no table to gather from, and pass 2b would composite
        // whatever the half-res FBO happened to hold from an earlier frame.
        // Both are skipped together, so the frame degrades to the opaque fill
        // (or to nothing) rather than to a stale or undefined image.
        bool drawNeon = !opaqueOnly;
        if (drawNeon)
        {
            // --- Pass 0: per-sample emission table ---------------------------
            // Inside the guard because the table feeds only Pass 1 - the debug
            // fill-only mode must not pay for a UBO upload plus a draw it never
            // samples. Must precede Pass 1, which binds the half-res FBO.
            packLightBlocks(config);
            // A table write is not a composite: blending would mix this frame's
            // emission into last frame's.
            glDisable(GL_BLEND);
            drawNeon = renderEmissionPass(viewportWidth, viewportHeight, time, config);
        }
        if (drawNeon)
        {
            // --- Pass 1: neon at resolutionScale into the half-res FBO -------
            // Premultiplied "over" into the transparent FBO: a single
            // non-overlapping quad over (0,0,0,0) leaves the FBO holding the
            // shader's premultiplied colour + coverage alpha, ready to be
            // composited over the backbuffer. Set here, not in the pass: blend
            // mode is a phase property, and pass 0 above left blending off.
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            drawNeon = renderHalfResNeonPass(viewportHeight, bufW, bufH, time, config);
        }

        // Back to the caller's target for the full-res passes.
        Framebuffer::BindId(targetFbo);
        glViewport(0, 0, viewportWidth, viewportHeight);

        // Premultiplied "over" for both the black fill and the blit, so
        // blending stays ON the whole pass (toggling GL_BLEND mid-draw is a
        // cross-driver footgun on mobile GLES).
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 2a: opaque-mode fill on the backbuffer ---------------------
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(viewportHeight, config);
        }

        // --- Pass 2b: composite the half-res neon over it --------------------
        if (drawNeon)
        {
            renderBlitPass(config);
        }

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
        // See NeonRenderer for the same guard - only per-segment and per-arc
        // stops/blend space affect the atlases; the live position/length/boost
        // and start/length/intensity fields ride the UBOs, which is why these
        // compare through IsAtlasDirty rather than the structs' operator==.
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const bool segLutDirty = IsAtlasDirty(mBakedSegments, mEffectiveSegments);
        const bool arcLutDirty = IsAtlasDirty(mBakedArcs, config.neon.arcs);

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

        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonOptimized.Emission");
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonBlit");
        if (!mNeonShader.IsValid() || !mBlackRectShader.IsValid() || !mBlitShader.IsValid() ||
            !mEmissionShader.IsValid())
        {
            return false;
        }

        mNeonShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        // The pre-pass reads the same two blocks (not LoopSamplesBlock - it
        // works in perimeter-parameter space and needs no pixel positions).
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        return true;
    }

    void NeonOptimizedRenderer::setupGeometry(const Config &config)
    {
        float scale = config.optimizedNeon.resolutionScale;

        // --- Scaled glow quad (pass 1) ---
        // Size the quad to exactly cover the lit region: rect + glowReach, so
        // geometry bounds the far region instead of a per-fragment discard
        // (tiler-friendly). Everything here is in scaled (FBO) space -
        // glowRadius*scale is already scaled, matching the uniforms uploaded
        // in Render().
        {
            // Use the SAME glow-reach factor as the base NeonRenderer so the
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
            float glowReach = config.neon.glowRadius * scale * float(GLOW_REACH_RADIUS_FACTOR) *
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
            float filamentReach = std::max(config.neon.lineWidth * 0.5f, float(FILAMENT_MIN_HALF_WIDTH)) * scale * filSigmas;

            float margin = std::max(glowReach, filamentReach);

            // Hard cap: when the outside cutoff is enabled the shader discards
            // emission past size + softness, so there's no point rasterising
            // further. The +1 px safety leaves the shader's own softmask at
            // zero BEFORE the quad edge, so no rectangular seam leaks through.
            //
            // The WHOLE expression is built in full-res px and scaled once, so
            // that safety margin is 1 FULL-RES px here, exactly as it is in the
            // base renderer. Adding the +1 after the scale made it 1 FBO px
            // instead - 2 full-res px at resolutionScale 0.5 - so the quad edge
            // sat further out than the base renderer's, and with it the ramp
            // the shader fits between the cutoff boundary and uQuadMargin. Same
            // units on both sides is also what makes the shader's fadeStart
            // floor engage at the same cutoff size in the two renderers.
            if (config.neon.outsideCutoff.enable)
            {
                float outSoft = std::max(config.neon.outsideCutoff.softness,
                                         static_cast<float>(SIDE_SOFT_EPSILON));
                float cutoffCap = (config.neon.outsideCutoff.size + outSoft + 1.0f) * scale;
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
    }

    void NeonOptimizedRenderer::rebuildGradientLUT(const Config &config)
    {
        // OnConfigChanged already gates this call behind a lutDirty check
        // (colorStops / blendSpace / gradientLutSize), so a re-entry here
        // always means the inputs actually changed.
        int lutSize = std::max(config.optimizedNeon.gradientLutSize, 4);
        // Sorted once here, not per texel - SampleRing walks the ring in
        // order and an unsorted list bakes a silently wrong gradient.
        const std::vector<ColorStop> baseStops = ColorUtils::SortStops(config.neon.colorStops);
        mLUTTarget.resize(lutSize * 4);
        for (int i = 0; i < lutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(lutSize);
            glm::vec4 c = ColorUtils::SampleRing(t, baseStops, config.neon.blendSpace);
            mLUTTarget[i * 4 + 0] = c.r;
            mLUTTarget[i * 4 + 1] = c.g;
            mLUTTarget[i * 4 + 2] = c.b;
            mLUTTarget[i * 4 + 3] = c.a;
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
            lutBytes[i] = ToByte(lut[i]);
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
            const std::vector<ColorStop> segStops = ColorUtils::SortStops(seg.colorStops);
            unsigned char *row = atlas.data() + (s * W * 4);
            for (int x = 0; x < W; ++x)
            {
                float t = static_cast<float>(x) / static_cast<float>(W - 1);
                // Clamped, not cyclic: this row is a head-to-tail span sampled
                // CLAMP_TO_EDGE, so stops that do not reach 0 and 1 must hold
                // their end colours rather than wrapping. See SampleSpan.
                glm::vec4 c = ColorUtils::SampleSpan(t, segStops, seg.blendSpace);
                row[x * 4 + 0] = ToByte(c.r);
                row[x * 4 + 1] = ToByte(c.g);
                row[x * 4 + 2] = ToByte(c.b);
                row[x * 4 + 3] = ToByte(c.a);
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
            const std::vector<ColorStop> arcStops = ColorUtils::SortStops(arc.colorStops);
            unsigned char *row = atlas.data() + (a * W * 4);
            for (int x = 0; x < W; ++x)
            {
                float t = static_cast<float>(x) / static_cast<float>(W - 1);
                // Clamped, not cyclic - same reason as the segment atlas above.
                glm::vec4 c = ColorUtils::SampleSpan(t, arcStops, arc.blendSpace);
                row[x * 4 + 0] = ToByte(c.r);
                row[x * 4 + 1] = ToByte(c.g);
                row[x * 4 + 2] = ToByte(c.b);
                row[x * 4 + 3] = ToByte(c.a);
            }
        }

        mArcLUT.SetData(atlas.data(), W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        // CLAMP on both axes - the row is a head-to-tail span, not a ring.
        // See NeonRenderer::rebuildArcLUT.
        mArcLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

        mBakedArcs = config.neon.arcs;
    }

} // namespace EdgeLighting
