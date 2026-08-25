#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"
#include "gl/framebuffer.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// Full-resolution single-pass neon renderer.
    ///
    /// Draws a tight quad over the rect + earlyOut margin and runs one
    /// fragment shader that composes filament + halo + bloom in one pass.
    /// Per-fragment work: an analytic rounded-box SDF, a gather loop over
    /// @c NEON_MAX_LOOP_SAMPLES perimeter samples (positions live in a UBO),
    /// and per-sample lookups into three baked LUTs:
    ///   - @c uGradientLUT      - the base colour ring.
    ///   - @c uSegmentLUT       - per-segment gradient atlas (one row per segment).
    ///   - @c uArcLUT           - per-arc gradient atlas (one row per arc).
    ///
    /// Arc gating uses winner-take-all: for each sample the arc with the
    /// largest @c mask*intensity owns the colour and emission there. See
    /// neon.frag for the full compose. Visual parameters come from
    /// @c Config::neon; @c Config::arcs / @c Config::segmentBoosts drive
    /// their respective UBOs and atlases.
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
        // --- Setup and config-driven rebuilds (not per-frame) --------------
        bool setupShaders();
        void setupGeometry(const Config &config);
        void rebuildLoopSamples(const Config &config);
        void rebuildGradientLUT(const Config &config);
        /// Quantise a float LUT (GRADIENT_LUT_SIZE * 4 RGBA) to RGBA8 and
        /// upload it to mGradientLUT.
        void uploadGradientLUT(const std::vector<float> &lut);
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

        // --- Per-frame pass list, declared in PASS-NUMBER order -------------
        // The numbering is the pipeline order from docs/emission-prepass.md,
        // and the .cpp defines them in this same order - keep all three in
        // step. The data dependency is the real contract: pass 0 bakes the
        // table pass 2 reads, and pass 1's fill must land before pass 2
        // composites the glow over it.
        //
        // NOTE @ref Render executes pass 0 AFTER pass 1 here, which is the one
        // place declaration order and call order differ. The emission table
        // feeds only pass 2, so deferring it past the opaqueOnly early-out
        // lets fill-only debug mode skip a UBO upload and a draw it would
        // never sample. NeonOptimizedRenderer has no such inversion - there
        // both are inside the same guard, so it runs 0, 1, 2a, 2b in order.

        // STATE OWNERSHIP. `Render` owns blend state - enable and func - and
        // sets it immediately before each pass that depends on it, so no pass
        // touches GL_BLEND and the whole blend timeline reads in one place.
        // A pass owns its shader, and a pass that RETARGETS the framebuffer
        // restores it (an excursion, unlike a mode). Preconditions each pass
        // relies on are stated in its @pre below.
        //
        // The passes take the pieces of the frame transform they actually use.
        // @ref Render derives them once: `center` reaches the screen by two
        // routes - folded into @p mvp for the glow, and added to each stop's
        // perimeter point for the debug markers - so deriving it in one place
        // is what keeps the markers aligned with the glow they annotate.

        /// Pack the segment + arc UBOs. Called before the emission pre-pass
        /// because BOTH passes read them: the pre-pass to bake the per-sample
        /// emission, the main pass for the continuous filament gate.
        void packLightBlocks(const Config &config);

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmissionBuffer. Retargets the framebuffer and viewport, so it
        /// restores both before returning - see docs/emission-prepass.md.
        /// @pre Blending disabled - a table write is not a composite.
        void renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Pass 1: opaque-mode background fill on a fullscreen NDC quad. The
        /// fragment shader shapes coverage from an analytic rounded-box SDF
        /// read off gl_FragCoord. Caller guards on @c opaqueMode != NONE.
        void renderOpaqueFill(const glm::vec2 &center, const Config &config);

        /// Pass 2: the neon gather on the tight glow quad. Reads the emission
        /// table produced by @ref renderEmissionPass, so it must run after it.
        void renderNeonPass(const glm::mat4 &mvp, float time, const Config &config);

        /// Debug pass: the baked gradient LUT as a strip at the geometry
        /// centre. Caller guards on @c showGradientLUT.
        /// @pre Blending disabled - the strip overwrites the glow beneath it.
        void renderGradientLUTStrip(const glm::mat4 &mvp, float time, const Config &config);

        /// Debug pass: one filled disc per colour stop at its perimeter
        /// position. Caller guards on @c showColorStops + a non-empty list.
        /// @pre Straight-alpha blending, for the discs' anti-aliased edges.
        /// @note Takes @p proj, not the composed mvp: every marker gets its own
        ///       model matrix, so it needs the projection un-premultiplied.
        void renderColorStopMarkers(const glm::mat4 &proj, const glm::vec2 &center,
                                    float halfWidth, float halfHeight, const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
        ShaderProgram mEmissionShader;                                 ///< Perimeter emission pre-pass (neon-emission.frag).
        ShaderProgram mBlackRectShader;                                ///< Opaque-mode black background fill (black-rect.frag).
        ShaderProgram mLUTDebugShader;                                 ///< Debug LUT strip (neon-lut-debug.frag).
        ShaderProgram mStopMarkerShader;                               ///< Debug per-stop marker (neon-stop-marker.frag).
        VertexArray mVertexArray{"NeonRenderer"};                      ///< Tight glow quad (rect + earlyOut).
        VertexArray mFullVertexArray{"NeonRenderer.Full"};             ///< Viewport-covering quad for the opaque fill.
        VertexArray mLUTStripVertexArray{"NeonRenderer.LUTStrip"};     ///< Small centred quad for the LUT debug strip.
        VertexArray mStopMarkerVertexArray{"NeonRenderer.StopMarker"}; ///< Unit quad ([-1,+1]) used to draw each stop marker.
        glm::vec2 mLUTStripHalfSize{0.0f};                             ///< Half extents of the LUT strip in local px (matches mLUTStripVertexArray).

        /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
        /// block holding uSegmentCount + uSegments[]).
        UniformBuffer mSegmentBlock{"NeonRenderer.SegmentBlock"};
        /// Backs neon.frag's std140 `LoopSamplesBlock` - vec4[NUM_LOOP_SAMPLES]
        /// where .xy holds the perimeter point in rect-local pixels.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};
        /// Backs neon.frag's std140 `ArcBlock` (uArcCount + uArcs[MAX_ARCS]).
        UniformBuffer mArcBlock{"NeonRenderer.ArcBlock"};

        float mQuadMargin = 0.0f; ///< Draw-quad margin (px from rect edge); shader fades the bloom out by here.

        /// Baked colour ring as a 1×N RGBA32F texture (sampled with v=0.5 in the shader).
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

        /// Perimeter emission table, NEON_MAX_LOOP_SAMPLES x 2 (see
        /// neon-emission.frag for the row packing). Rebuilt every frame by
        /// @ref renderEmissionPass and read by the gather with texelFetch.
        Framebuffer mEmissionBuffer{"NeonRenderer.Emission"};
        /// Latched the first time the driver refuses an RGBA16F emission
        /// target, after which the renderer asks only for RGBA8. Not just a
        /// log guard: @c Framebuffer::Resize treats a format change as a
        /// reallocation, so re-requesting 16F every frame would churn the
        /// texture + FBO twice per frame. RGBA8 is a working fallback, but it
        /// clamps arc intensities and stacked segment boosts above 1.0.
        bool mEmissionFloatUnavailable = false;
        std::vector<Arc> mBakedArcs;

        // --- Gradient cross-fade -------------------------------------------
        // When the colour stops change we don't snap the LUT: we bake the new
        // ring into mLUTTarget, snapshot the currently-shown ring into mLUTFrom,
        // and let Update() blend From->Target into mLUTDisplay over
        // colorTransitionDuration seconds. All three are float RGBA
        // (GRADIENT_LUT_SIZE * 4); mLUTDisplay is what gets quantised+uploaded.
        // Cross-fading in LUT space handles stop sets that differ in count or
        // position (there's no per-stop pairing to worry about).
        std::vector<float> mLUTTarget;  ///< Freshly baked destination ring.
        std::vector<float> mLUTFrom;    ///< Ring shown when the current fade began.
        std::vector<float> mLUTDisplay; ///< Currently-uploaded (blended) ring.
        bool mHasBakedLUT = false;      ///< False until the first bake seeds the buffers.
        bool mFading = false;           ///< True while a cross-fade is in flight.
        float mFadeElapsed = 0.0f;      ///< Seconds into the current fade.
        float mFadeDuration = 0.0f;     ///< Snapshot of the duration for this fade.
        /// (stops, blendSpace) behind mLUTTarget - a new bake only restarts the
        /// fade when these actually change. OnConfigChanged fires whenever ANY
        /// field of the composited config moves (a slider, or an animation
        /// re-compositing the active config every frame), so the gradient
        /// inputs are usually unchanged when it arrives.
        std::vector<ColorStop> mTargetStops;
        BlendSpace mTargetBlendSpace = BlendSpace::RGB;
    };
}

#endif
