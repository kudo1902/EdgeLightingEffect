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
        /// shader arrays can hold. The excess is dropped silently otherwise:
        /// the UBOs are fixed-size (@c MAX_ARCS / @c MAX_SEGMENT_BOOSTS, shared
        /// with the GLSL array declarations), and everything past the cap never
        /// reaches the GPU. The demo's UI enforces the caps so it never sees
        /// this, but a library or C-ABI host gets no other signal.
        ///
        /// @p latched is the caller's per-renderer flag: set while the overflow
        /// is being reported, cleared once the count falls back under the cap
        /// so a later overflow is reported again rather than swallowed.
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

    bool NeonRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonRenderer shaders.");
            return false;
        }
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

    void NeonRenderer::packLightBlocks(const Config &config)
    {
        // Both the emission pre-pass and the main pass read these, so they are
        // packed once per frame before either draws.
        //
        // Pack the segment vector as vec4(position, invSigma, boost, hasStops)
        // into the std140 SegmentBlock UBO (DALi-compatible pattern - see
        // neon.frag). Empty vector -> uSegmentCount=0 and both shaders skip the
        // whole feature.
        SegmentBlockData segBlock = {};
        // mEffectiveSegments is NOT refilled here. OnConfigChanged fills it
        // whenever the composited config changes, and this runs once per frame
        // from Render - so on a frame where nothing changed the merged view is
        // already current, and on a frame where something did, OnConfigChanged
        // has already run (Update -> refreshActiveConfig precedes Render).
        // Refilling was the third FillEffectiveSegments of the same frame.
        const std::vector<SegmentBoost> &effSegments = mEffectiveSegments;
        WarnOnOverflow("segments", "NeonRenderer", effSegments.size(),
                       int(MAX_SEGMENT_BOOSTS), mSegmentOverflowLogged);
        int segCount = std::min(static_cast<int>(effSegments.size()),
                                int(MAX_SEGMENT_BOOSTS));
        segBlock.count = segCount;
        for (int i = 0; i < segCount; ++i)
        {
            const auto &s = effSegments[i];
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
        WarnOnOverflow("arcs", "NeonRenderer", config.neon.arcs.size(),
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

    bool NeonRenderer::renderEmissionPass(int viewportWidth, int viewportHeight,
                                          float time, const Config &config)
    {
        // RGBA16F, not RGBA8: row 0 carries Arc::intensity and row 1 sums
        // stacked SegmentBoost::boost values, both of which exceed 1.0 in
        // ordinary use. GLES 3.0 exposes float colour-renderability only
        // through an extension, so fall back to RGBA8 where the driver refuses
        // it - the picture is otherwise identical, but highlights above 1.0
        // clamp. Warn once so the difference is not silent.
        //
        // GL_NEAREST because the consumer reads with texelFetch: adjacent
        // texels are unrelated perimeter samples (and the two rows are
        // different quantities entirely), so filtering across them is
        // meaningless.
        //
        // The target the gather below draws into. NOT necessarily the default
        // framebuffer: an offscreen frame capture (@ref OffscreenCapture) hands
        // this renderer a real FBO, and the gather has no bind of its own, so
        // restoring 0 here would silently redirect the whole neon pass to the
        // window and leave the capture empty. Read BEFORE the resize below, so
        // it stays correct even if a reallocation ever rebinds.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        // The fallback LATCHES. Framebuffer::Resize treats a format change as a
        // reallocation, so re-asking for 16F every frame on a driver that
        // refuses it would destroy and recreate the texture + FBO twice per
        // frame, forever, with the warning suppressed after the first line.
        if (!mEmissionFloatUnavailable &&
            !mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 2,
                                    GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_NEAREST))
        {
            mEmissionFloatUnavailable = true;
            LOG_W("NeonRenderer: RGBA16F emission target unavailable, using RGBA8. "
                  "Arc intensities and stacked segment boosts above 1.0 will clamp.");
        }
        if (mEmissionFloatUnavailable &&
            !mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 2,
                                    GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST))
        {
            // Nothing to draw into, and nothing to fall back on either:
            // Framebuffer::Resize calls destroy() on its failure path, so the
            // previous frame's attachment is already gone. Binding it anyway
            // would hand the gather texture 0 and sample undefined data in core
            // profile. Tell the caller to skip the gather instead.
            return false;
        }

        // Binds the FBO and sets the viewport to NEON_MAX_LOOP_SAMPLES x 2. No
        // clear: the NDC quad covers every texel, so each one is written.
        mEmissionBuffer.Bind();

        mEmissionShader.Use();
        mEmissionShader.SetUniform("uMVP", glm::mat4(1.0f));
        mEmissionShader.SetUniform("uTime", time);
        mEmissionShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        // The full-res renderer always walks every sample; the optimized one
        // passes its runtime slider value instead.
        mEmissionShader.SetUniform("uNumSamples", int(NEON_MAX_LOOP_SAMPLES));
        mGradientLUT.Bind(0);
        mEmissionShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mEmissionShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mEmissionShader.SetUniform("uArcLUT", 2);
        mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mEmissionShader.Unuse();

        // Hand the framebuffer and viewport back exactly as found. Blend mode
        // is untouched here - it is a phase property owned by Render.
        Framebuffer::BindId(targetFbo);
        glViewport(0, 0, viewportWidth, viewportHeight);
        return true;
    }

    void NeonRenderer::renderOpaqueFill(const glm::vec2 &center, const Config &config)
    {
        // A fullscreen NDC quad (identity MVP); the fragment shader shapes the
        // black coverage from an analytic rounded-box SDF read off gl_FragCoord
        // (highp - exact on Mali/Tizen):
        //   BOTH    -> black everywhere (whole viewport opaque).
        //   INSIDE  -> black only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        mBlackRectShader.Use();
        mBlackRectShader.SetUniform("uMVP", glm::mat4(1.0f));
        mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlackRectShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
        mBlackRectShader.SetUniform("uRectCenter", center);
        float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                    static_cast<float>(SIDE_SOFT_EPSILON));
        mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
        mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
        mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
        mFullVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlackRectShader.Unuse();
    }

    void NeonRenderer::renderNeonPass(const glm::mat4 &mvp, float time, const Config &config)
    {
        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
        mShaderProgram.SetUniform("uLineWidth", config.neon.lineWidth);
        mShaderProgram.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mShaderProgram.SetUniform("uIntensity", config.neon.intensity);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mShaderProgram.SetUniform("uGlowRadius", config.neon.glowRadius);
        mShaderProgram.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        mShaderProgram.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mShaderProgram.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mShaderProgram.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mShaderProgram.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);

        mShaderProgram.SetUniform("uWinding", static_cast<int>(config.geometry.winding));

        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);

        // The three LUT atlases are no longer read by the gather (the emission
        // pre-pass consumes them instead), but the pointwise path still samples
        // them for the colour-stop alpha - see the alpha reads in neon.frag.
        mGradientLUT.Bind(0);
        mShaderProgram.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mShaderProgram.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mShaderProgram.SetUniform("uArcLUT", 2);
        // Emission table from pass 0 on unit 3; the gather texelFetches both
        // of its rows per sample.
        mEmissionBuffer.BindTexture(3);
        mShaderProgram.SetUniform("uEmission", 3);
        mShaderProgram.SetUniform("uQuadMargin", mQuadMargin);

        // Tight glow quad in both modes - opaque's far region is covered by the
        // fill pass, so the gather never runs fullscreen.
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mShaderProgram.Unuse();
    }

    void NeonRenderer::renderGradientLUTStrip(const glm::mat4 &mvp, float time, const Config &config)
    {
        // Overwrites the neon output within the strip rect so the baked ring is
        // readable regardless of the glow's tone-mapped brightness, which is
        // why the caller draws it unblended.
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

    void NeonRenderer::renderColorStopMarkers(const glm::mat4 &proj, const glm::vec2 &center,
                                              float halfWidth, float halfHeight, const Config &config)
    {
        // Draws a filled disc in each stop's colour at its perimeter position,
        // so the raw (position, colour) inputs can be checked against the LUT
        // strip and the on-screen glow. Uses standard alpha blending for the
        // ring / anti-aliased edge to composite cleanly - the caller sets that
        // blend mode before calling.

        // Scale marker with the smaller half-extent so it stays inside the
        // rect on very tall/thin geometries; cap at 12 px so it's not huge
        // on large rects.
        float markerRadius = std::min(std::min(halfWidth, halfHeight) * 0.06f, 12.0f);
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

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
        {
            return;
        }

        // Render is a pass schedule and nothing else: derive the transform,
        // then one call per pass. Each pass owns its own shader and, where it
        // retargets, its own framebuffer restore. Blend state is owned HERE -
        // the two passes that deviate say so in their comments.
        //
        // Derived once and handed down in pieces. `center` reaches the screen
        // by two routes - folded into `mvp` for the glow, and added to each
        // stop's perimeter point by the marker pass - so deriving it here
        // rather than per pass is what keeps the markers aligned with the glow.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        const glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                          0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        // Viewport y runs down in Config but up in the projection, so the
        // centre is mirrored about the viewport height.
        const glm::vec2 center(config.geometry.position.x + halfRectW,
                               static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));

        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a). Used
        // for both the opaque black fill and the neon, so the neon composites
        // cleanly over the black. (Blending stays ON the whole time - toggling
        // GL_BLEND mid-draw is a common cross-driver footgun on mobile GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 1: opaque-mode background fill ---------------------------
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(center, config);
        }

        // Debug: stop after the fill. Skips the gather AND the LUT-strip /
        // stop-marker overlays below, so what lands on screen is the opaque
        // silhouette by itself - which is how the fill's square corner at
        // cornerRadius 0 gets compared against the emission's round one.
        // Restores the blend state the tail of this function would have set.
        if (config.neon.opaqueOnly)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            return;
        }

        // --- Pass 0: per-sample emission table ------------------------------
        // Deliberately AFTER the opaqueOnly return: the table feeds only the
        // gather below, so the debug fill-only mode must not pay for a UBO
        // upload plus a draw it never samples. Safe to retarget the framebuffer
        // here - the fill has already landed, and this pass restores the target
        // it was handed.
        packLightBlocks(config);
        // A table write is not a composite: blending would mix this frame's
        // emission into last frame's.
        glDisable(GL_BLEND);
        const bool emissionReady = renderEmissionPass(viewportWidth, viewportHeight, time, config);

        // --- Pass 2: the neon gather ----------------------------------------
        // Re-assert the phase mode: pass 0 leaves blending disabled. Setting it
        // immediately before the phase that needs it (rather than relying on
        // the carry-over from above) is what makes the pass order safe to
        // change without silently breaking compositing.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        // Skipped only when pass 0 could allocate no target at all. The gather
        // reads the table for every sample, so without one it has nothing to
        // shade with - drawing anyway would sample texture 0. The opaque fill
        // above has already landed, so the frame degrades to the silhouette
        // rather than to garbage.
        if (emissionReady)
        {
            renderNeonPass(mvp, time, config);
        }

        // --- Debug overlays --------------------------------------------------
        if (config.neon.showGradientLUT)
        {
            // Unblended: the strip overwrites the glow so it stays readable.
            glDisable(GL_BLEND);
            renderGradientLUTStrip(mvp, time, config);
        }
        if (config.neon.showColorStops && !config.neon.colorStops.empty())
        {
            // Straight alpha for the discs' anti-aliased edges.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderColorStopMarkers(proj, center, halfRectW, halfRectH, config);
        }

        // Restore a known blend state for following renderers (the LUT strip
        // pass disables blending).
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
                              config.neon.blendSpace != mCurrentConfig.neon.blendSpace;
        // Only the segments' colour stops + blend space affect the atlas
        // texture; position/length/boost don't (they're read live from the
        // UBO), so the compare comes from IsAtlasDirty rather than
        // SegmentBoost::operator==, which would also see the live fields an
        // animation rewrites every frame. Compares the merged view so a change
        // in either the transient or preserved pool triggers a re-bake.
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        const bool segLutDirty = IsAtlasDirty(mBakedSegments, mEffectiveSegments);
        // Same idea for arcs - start/length/intensity ride the UBO, only
        // colorStops + blendSpace changes require a re-bake of the atlas.
        const bool arcLutDirty = IsAtlasDirty(mBakedArcs, config.neon.arcs);

        mCurrentConfig = config;
        if (!mShaderProgram.IsValid())
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

    bool NeonRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        // Emission pre-pass. Reuses the neon vertex shader (uMVP -> vPos); the
        // fragment shader ignores vPos and keys off gl_FragCoord instead.
        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonRenderer.Emission");
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
            !mLUTDebugShader.IsValid() || !mStopMarkerShader.IsValid() ||
            !mEmissionShader.IsValid())
        {
            return false;
        }

        mShaderProgram.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mShaderProgram.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        // The pre-pass reads the same two blocks the main pass does, so they
        // share bindings and are packed once per frame before either runs.
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        return true;
    }

    void NeonRenderer::setupGeometry(const Config &config)
    {
        // Size the quad to cover the lit region: rect + glowReach, so geometry
        // bounds the far region instead of a per-fragment discard
        // (tiler-friendly). Factors come from the shared neon-tuning.h.
        //
        // glowRadius ONLY - deliberately not the old
        // max(glowRadius * RADIUS, sampleSpacing * SPACING). sampleSpacing is
        // perimeter / NEON_MAX_LOOP_SAMPLES, so the spacing term won on any
        // reasonably large rect at default glowRadius and made the quad - and
        // with it the distance the bloom got truncated at, and the brightness
        // it still had there - track the rect size: the fade began at 250 px on
        // a 200x150 rect and 1542 px on a 1920x1080 one. The bloom's reach is a
        // function of glowRadius and nothing else, so that is all that sizes the
        // quad now. It also caps the worst case: the old spacing term asked for
        // a ~5800x4900 px quad on a 1920x1080 rect.
        //
        // The wide bloom (1/D tail) stays visible further out as bloomStrength /
        // intensity rise, so grow the quad with them. The shader reproduces this
        // exact expression to place its bloom pedestal, which is what lets the
        // margin stay this tight without the truncation showing - keep the two
        // in step.
        float glowReach = config.neon.glowRadius * float(GLOW_REACH_RADIUS_FACTOR) *
                          (1.0f + config.neon.bloomStrength * config.neon.intensity);

        // ...but the filament is sized by lineWidth, not glowRadius, so the quad
        // must clear it too. Without this floor, glowRadius = 0 ("filament only")
        // produced a zero margin: the quad landed exactly on the rect and clipped
        // every exterior fragment, so the outside half of the filament
        // disappeared while the inside half stayed. Mirrors the shader's `sigma`.
        // Reach in sigmas depends on the falloff exponent, not a constant - a
        // soft filament's tail runs for hundreds of sigmas. Same expression as
        // the shaders' reachSigmas; see neon-tuning.h.
        float filN = 2.0f * std::max(config.neon.filamentFalloff, 1e-3f);
        float filSigmas = std::clamp(
            std::pow(std::log2(float(FILAMENT_GAIN) / float(FILAMENT_CUTOFF)), 1.0f / filN),
            float(FILAMENT_REACH_MIN_SIGMAS), float(FILAMENT_REACH_MAX_SIGMAS));
        float filamentReach = std::max(config.neon.lineWidth * 0.5f, float(FILAMENT_MIN_HALF_WIDTH)) * filSigmas;

        float margin = std::max(glowReach, filamentReach);

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
    }

    void NeonRenderer::rebuildGradientLUT(const Config &config)
    {
        // Bake the entire colour ring on CPU into mLUTTarget; the shader then
        // becomes colour-stop-agnostic. Keeps HSV-vs-RGB blend cost off the GPU
        // hot path.
        // Sorted once here, not per texel - SampleRing walks the ring in
        // order and an unsorted list bakes a silently wrong gradient.
        const std::vector<ColorStop> baseStops = ColorUtils::SortStops(config.neon.colorStops);
        mLUTTarget.resize(GRADIENT_LUT_SIZE * 4);
        for (int i = 0; i < GRADIENT_LUT_SIZE; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(GRADIENT_LUT_SIZE);
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
            uploadGradientLUT(mLUTDisplay);
            mTargetStops = config.neon.colorStops;
            mTargetBlendSpace = config.neon.blendSpace;
            mHasBakedLUT = true;
            mFading = false;
            return;
        }

        // OnConfigChanged fires on ANY change to the composited config - and
        // with an animation attached that is nearly every frame - so it usually
        // arrives with the gradient inputs untouched. Only (re)start a fade when
        // they actually changed.
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
            lutBytes[i] = ToByte(lut[i]);
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
        // CLAMP on both axes, like the segment atlas. U was REPEAT on the
        // theory that an arc's colours "cycle around the perimeter" - but this
        // atlas is only ever sampled when the arc has its OWN stops, and those
        // are laid head-to-tail across the arc's span, not around the ring. An
        // arc that inherits instead reads the base gradient, which keeps its
        // own REPEAT wrap. With the hue-rotation term gone from the arc-local
        // coordinate (see neon.frag) the only reads outside [0, 1] are the few
        // px the straddling arc feather extends past each end, and CLAMP holds
        // the end colour there instead of fetching the opposite end's.
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
        mArcLUT.SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);

        mBakedArcs = config.neon.arcs;
    }
}
