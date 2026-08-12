#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/framebuffer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// The neon renderer.
    ///
    /// One class covers what used to be two (a full-res renderer and a
    /// near-identical half-res "optimized" copy). The resolution is now a
    /// config knob, @c NeonConfig::resolutionScale, and everything else -
    /// UBO packing, the three baked LUTs, the gradient cross-fade, the
    /// emission pre-pass, the shader body - is shared by construction rather
    /// than by keeping two files in sync.
    ///
    /// Per frame:
    ///
    ///  1. **Emission pre-pass** (neon-emission.frag) - resolves, for each of
    ///     the active perimeter samples, which arc wins there (winner-take-all
    ///     on @c mask*intensity), what colour it contributes, what the
    ///     travelling segments add, and the resulting coverage. Writes one
    ///     texel per sample into @c mEmissionBuffer. This is where the three
    ///     baked LUTs are consumed:
    ///       - @c uGradientLUT - the base colour ring.
    ///       - @c uSegmentLUT  - per-segment gradient atlas (one row per segment).
    ///       - @c uArcLUT      - per-arc gradient atlas (one row per arc).
    ///     All of it is a pure function of (sample position, time, config), so
    ///     it costs one fragment per sample per frame rather than being
    ///     recomputed by every screen fragment.
    ///
    ///  2. **Main pass** (neon.frag) - draws a tight quad over the rect +
    ///     earlyOut margin and composes filament + halo + bloom. Per-fragment
    ///     work is an analytic rounded-box SDF plus a gather loop whose body
    ///     is one UBO read (the sample position) and one @c texelFetch into
    ///     the emission texture. Target is the backbuffer at
    ///     @c resolutionScale == 1, otherwise @c mScaledBuffer.
    ///
    ///  3. **Blit** (neon-blit.frag) - scaled path only: bilinear composite of
    ///     the scaled FBO onto the backbuffer.
    ///
    /// The scaled path pre-multiplies the rect size, line width, cutoffs and
    /// sample spacing by the scale on the CPU, so neon.frag never learns what
    /// resolution it is running at.
    class NeonRenderer : public BaseRenderer
    {
    public:
        NeonRenderer() = default;
        virtual ~NeonRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);

        void rebuildGradientLUT(const Config &config);
        /// Quantise a float LUT (@p lutSize * 4 RGBA) to RGBA8 and upload it
        /// to mGradientLUT.
        void uploadGradientLUT(const std::vector<float> &lut, int lutSize);
        /// Bake each segment's colorStops into one row of mSegmentLUT
        /// (SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS). Rows for segments with
        /// empty stops are left zero-filled - the shader falls back to the
        /// base gradient in that case (see the vec4.w flag in SegmentBlock).
        void rebuildSegmentLUT(const Config &config);
        /// Bake each arc's colorStops into one row of mArcLUT
        /// (ARC_LUT_WIDTH x MAX_ARCS). Rows for arcs with empty stops are
        /// left zero-filled - the shader falls back to the base gradient at
        /// those samples (see the vec4.w flag in ArcBlock).
        void rebuildArcLUT(const Config &config);

        /// Effective resolution scale: NeonConfig::resolutionScale clamped to
        /// (0, 1]. Exactly 1 selects the direct-to-backbuffer path.
        static float resolutionScale(const Config &config);
        /// Effective gather sample count: NeonConfig::numSamples clamped to
        /// [1, NEON_MAX_LOOP_SAMPLES].
        static int sampleCount(const Config &config);

        // --- Per-frame passes, in the order Render() runs them ---------------
        // Shared contract: each one binds its own shader and its own render
        // target, and returns with the DEFAULT framebuffer and the full
        // viewport bound. Blend state is owned by Render() except where a pass
        // documents otherwise.

        /// Packs Config::neon's segments and arcs into the SegmentBlock /
        /// ArcBlock UBOs and binds them. Runs before any pass because BOTH the
        /// emission pre-pass (per-sample colour + coverage) and the main pass
        /// (continuous filament gate) read them. Arc / segment positions are
        /// normalised perimeter coords, so resolutionScale does not apply.
        void packLightBlocks(const Config &config);
        /// Pass 0 - bakes the perimeter emission table into mEmissionBuffer.
        /// Expects packLightBlocks() to have run. Toggles GL_BLEND off for the
        /// duration (it is a table write, not a composite) and back on after.
        void renderEmissionPass(float time, const Config &config, int numSamples,
                                int viewportWidth, int viewportHeight);
        /// Pass 1 (opaque modes only) - fullscreen black rounded-rect fill so
        /// the neon has something opaque to composite over. Always drawn on
        /// the backbuffer at FULL resolution, even on the scaled path, so its
        /// rounded corners anti-alias against the real pixel grid.
        void renderOpaqueFill(const Config &config, const glm::vec2 &center);
        /// Pass 2 - the neon gather on the tight glow quad. The only pass that
        /// draws the effect itself. Renders into mScaledBuffer when
        /// @p scale < 1 (sizing and clearing it first), otherwise straight
        /// onto the backbuffer.
        void renderNeonPass(const Config &config, int viewportWidth, int viewportHeight,
                            int numSamples, float scale);
        /// Pass 3 (scaled path only) - bilinear composite of mScaledBuffer
        /// onto the backbuffer.
        void renderBlitPass(const Config &config);
        /// Debug overlay - the baked colour ring as a strip at the geometry
        /// centre. Draws unblended so it stays readable over the tone-mapped
        /// glow; leaves GL_BLEND disabled for the caller to restore.
        void renderGradientLUTStrip(const Config &config, float time, const glm::mat4 &mvp);
        /// Debug overlay - a filled disc per colour stop at its perimeter
        /// position. Switches to straight alpha blending for the anti-aliased
        /// edges.
        void renderColorStopMarkers(const Config &config, const glm::mat4 &proj,
                                    const glm::vec2 &center);

    private:
        Config mCurrentConfig;
        /// Main pass (neon.frag). One program for every sample count: the
        /// gather's trip count is the uNumSamples uniform, and the hand-written
        /// unroll inside the loop makes that as fast as the compile-time bound
        /// a second program used to buy.
        ShaderProgram mShaderProgram;
        ShaderProgram mEmissionShader;                                 ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode background fill (black-rect.frag).
        ShaderProgram mBlitShader;                                     ///< Scaled-path upscale to full res (neon-blit.frag).
        ShaderProgram mLUTDebugShader;                                 ///< Debug LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                               ///< Debug per-stop marker (neon-stop-marker.frag).
        VertexArray mVertexArray{"NeonRenderer"};                      ///< Tight glow quad (rect + earlyOut).
        VertexArray mFullVertexArray{"NeonRenderer.Full"};             ///< Viewport-covering quad: emission pre-pass, opaque fill, blit.
        VertexArray mLUTStripVertexArray{"NeonRenderer.LUTStrip"};     ///< Small centred quad for the LUT debug strip.
        VertexArray mStopMarkerVertexArray{"NeonRenderer.StopMarker"}; ///< Unit quad ([-1,+1]) used to draw each stop marker.
        glm::vec2 mLUTStripHalfSize{0.0f};                             ///< Half extents of the LUT strip in local px (matches mLUTStripVertexArray).

        /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
        /// block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonRenderer.SegmentBlock"};
        /// Backs neon.frag's std140 `LoopSamplesBlock` - vec4[N] where .xy
        /// holds the perimeter point in render-target pixels.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};
        /// Backs neon.frag's std140 `ArcBlock` (uArcCount + uArcs[MAX_ARCS]).
        UniformBuffer mArcBlock{"NeonRenderer.ArcBlock"};

        /// Target of the main pass when resolutionScale < 1: an RGBA8 buffer
        /// at scale * viewport, bilinear-blitted back to full res afterwards.
        /// Never allocated on the full-res path.
        Framebuffer mScaledBuffer{"NeonRenderer.Scaled"};

        /// Target of the emission pre-pass: NEON_MAX_LOOP_SAMPLES x 1, one texel
        /// per perimeter sample, sampled with texelFetch (GL_NEAREST). Ideally
        /// RGBA16F - a segment's contribution is boost-scaled and several can
        /// stack, so values above 1.0 are normal. Falls back to RGBA8 when the
        /// driver won't render to a float format, which clamps those
        /// highlights; mEmissionIsFloat records which one we got.
        Framebuffer mEmissionBuffer{"NeonRenderer.Emission"};
        bool mEmissionIsFloat = false;

        float mSampleSpacing = 0.0f;
        float mPerimeter = 0.0f;  ///< Rounded-rect perimeter in target px; sizes the pre-pass's pixel-space arc feather.
        float mQuadMargin = 0.0f; ///< Draw-quad margin (target px from rect edge); shader fades the bloom out by here.

        /// Baked colour ring as a 1xN RGBA8 texture (sampled with v=0.5 in the shader).
        /// Each shader sample becomes a single texture lookup instead of an in-shader stops loop + HSV blend
        Texture2D mGradientLUT;

        /// Per-segment gradient atlas - one row per segment, each row is that
        /// segment's stops baked head-to-tail across its span. Empty-stops
        /// segments leave their row zero; the shader detects that via the
        /// per-segment hasStops flag (SegmentBlock's vec4.w) and falls back to
        /// the base gradient at those samples.
        Texture2D mSegmentLUT;
        /// Cached snapshot of the last-baked segments so per-frame
        /// OnConfigChanged only re-uploads mSegmentLUT when they actually
        /// changed (matches how mTargetStops guards mGradientLUT rebuilds).
        std::vector<SegmentBoost> mBakedSegments;
        /// Reusable scratch for the merged transient+preserved segment list
        /// (Config::FillEffectiveSegments). Held as a member so the per-frame
        /// UBO pack / dirty check do no heap allocation after warmup.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Per-arc gradient atlas - one row per arc, each row is that arc's
        /// stops baked head-to-tail. Same shape/purpose as mSegmentLUT; the
        /// shader uses ArcBlock's vec4.w to skip the fetch when an arc has
        /// no stops (inherit-base case).
        Texture2D mArcLUT;
        std::vector<Arc> mBakedArcs;

        // --- Gradient cross-fade -------------------------------------------
        // When the colour stops change we don't snap the LUT: we bake the new
        // ring into mLUTTarget, snapshot the currently-shown ring into mLUTFrom,
        // and let Update() blend From->Target into mLUTDisplay over
        // colorTransitionDuration seconds. All three are float RGBA
        // (mLUTBakedSize * 4); mLUTDisplay is what gets quantised+uploaded.
        // Cross-fading in LUT space handles stop sets that differ in count or
        // position (there's no per-stop pairing to worry about).
        //
        // gradientLutSize is runtime-tunable, so when it changes the buffer
        // size changes too and we can't lerp element-wise - we snap in that
        // case (mLUTBakedSize tracks the current width so we can detect it).
        std::vector<float> mLUTTarget;  ///< Freshly baked destination ring.
        std::vector<float> mLUTFrom;    ///< Ring shown when the current fade began.
        std::vector<float> mLUTDisplay; ///< Currently-uploaded (blended) ring.
        int mLUTBakedSize = 0;          ///< Width (texels) of the buffers above; 0 until first bake.
        bool mHasBakedLUT = false;      ///< False until the first bake seeds the buffers.
        bool mFading = false;           ///< True while a cross-fade is in flight.
        float mFadeElapsed = 0.0f;      ///< Seconds into the current fade.
        float mFadeDuration = 0.0f;     ///< Snapshot of the duration for this fade.
        /// (stops, blendSpace) behind mLUTTarget - a new bake only restarts the
        /// fade when these actually change (SetConfig fires OnConfigChanged
        /// every frame with an unchanged config).
        std::vector<ColorStop> mTargetStops;
        BlendSpace mTargetBlendSpace = BlendSpace::RGB;
    };
}

#endif
