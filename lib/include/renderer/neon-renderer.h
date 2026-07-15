#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/texture-2d.h"
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
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
        /// Quantise a float LUT (GRADIENT_LUT_SIZE * 4 RGBA) to RGBA8 and
        /// upload it to mGradientLUT.
        void uploadGradientLUT(const std::vector<float> &lut);
        /// Bake each segment's colorStops into one row of mSegmentLUT
        /// (SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS). Rows for segments with
        /// empty stops are left zero-filled - the shader falls back to the
        /// base gradient in that case (see the vec4.w flag in SegmentBlock).
        void rebuildSegmentLUT(const Config &config);

    private:
        Config mCurrentConfig;
        ShaderProgram mShaderProgram;
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

        float mSampleSpacing = 0.0f;
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
        /// fade when these actually change (SetConfig fires OnConfigChanged
        /// every frame with an unchanged config).
        std::vector<ColorStop> mTargetStops;
        BlendSpace mTargetBlendSpace = BlendSpace::RGB;
    };
}

#endif
