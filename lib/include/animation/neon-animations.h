#ifndef _EDGE_LIGHTING_NEON_ANIMATIONS_H_
#define _EDGE_LIGHTING_NEON_ANIMATIONS_H_

#include "animation/animation.h"
#include "animation/easing-function.h"
#include "animation/modulator.h"
#include "util/geometry-utils.h"
#include <cmath>

namespace EdgeLighting
{
    /// @defgroup neon_animations Neon Animations
    /// @brief Ready-made @ref Animation subclasses that drive specific
    ///        @ref NeonConfig fields.
    ///
    /// Each class wraps one particular use of the @ref Modulator primitives.
    /// Constructors take only the parameters that matter for the effect,
    /// defaulting the rest to sensible values — call sites are typically one
    /// line.
    ///
    /// Looping animations use the default `Animation()` ctor (duration = -1).
    /// One-shot animations pass their duration via the `Animation(float)` ctor.
    /// For one-shots whose internal modulator depends on duration (the eased
    /// fades), @ref OnDurationChanged is overridden to rebuild the modulator
    /// when @ref SetDuration is called mid-flight.
    ///
    /// @{

    // -------------------------------------------------------------------------
    // neon.intensity
    // -------------------------------------------------------------------------

    /// @brief Sine-wave breathe on @c neon.intensity.
    /// @details Default one-second cycle between 0.4 and 1.0.
    class IntensityPulse : public Animation
    {
    public:
        /// @param duration Seconds per cycle.
        /// @param min      Minimum intensity (lower bound).
        /// @param max      Maximum intensity (upper bound).
        IntensityPulse(float duration = 1.0f, float min = 0.4f, float max = 1.0f)
            : mOsc(1.0f / duration, min, max) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    /// @brief Square-wave on/off strobe on @c neon.intensity.
    class IntensityStrobe : public Animation
    {
    public:
        /// @param duration      Seconds per on+off cycle.
        /// @param offIntensity  Value when the strobe is off.
        /// @param onIntensity   Value when the strobe is on.
        IntensityStrobe(float duration = 0.25f,
                        float offIntensity = 0.0f,
                        float onIntensity = 1.0f)
            : mOsc(1.0f / duration, offIntensity, onIntensity, 0.0f, Waveform::SQUARE) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    /// @brief One-shot eased fade-in on @c neon.intensity.
    /// @details Ramps from 0 up to a target value. @ref SetDuration rebuilds
    ///          the internal @ref Ease so the visual fade stays in sync with
    ///          the completion latch.
    class IntensityFadeIn : public Animation
    {
    public:
        /// @param targetIntensity Value to fade up to.
        /// @param duration        Fade duration in seconds.
        /// @param curve           Easing curve for the transition.
        IntensityFadeIn(float targetIntensity = 1.0f,
                        float duration = 0.4f,
                        EasingFunction::Curve curve = EasingFunction::OutCubic)
            : Animation(duration),
              mTarget(targetIntensity), mCurve(curve),
              mEase(0.0f, targetIntensity, duration, curve, /*loop=*/false) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mEase.Evaluate(elapsed);
        }

    protected:
        void OnDurationChanged(float d) override
        {
            mEase = Ease(0.0f, mTarget, d, mCurve, false);
        }

    private:
        float mTarget;
        EasingFunction::Curve mCurve;
        Ease mEase;
    };

    /// @brief One-shot eased fade-out on @c neon.intensity to 0.
    /// @details Mirror of @ref IntensityFadeIn.
    class IntensityFadeOut : public Animation
    {
    public:
        /// @param startIntensity Value to fade from.
        /// @param duration       Fade duration in seconds.
        /// @param curve          Easing curve for the transition.
        IntensityFadeOut(float startIntensity = 1.0f,
                         float duration = 0.4f,
                         EasingFunction::Curve curve = EasingFunction::InCubic)
            : Animation(duration),
              mStart(startIntensity), mCurve(curve),
              mEase(startIntensity, 0.0f, duration, curve, /*loop=*/false) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mEase.Evaluate(elapsed);
        }

    protected:
        void OnDurationChanged(float d) override
        {
            mEase = Ease(mStart, 0.0f, d, mCurve, false);
        }

    private:
        float mStart;
        EasingFunction::Curve mCurve;
        Ease mEase;
    };

    // -------------------------------------------------------------------------
    // neon.glowRadius
    // -------------------------------------------------------------------------

    /// @brief Sine breathe on @c neon.glowRadius.
    /// @details The halo "throbs" wider and narrower.
    class GlowRadiusBreath : public Animation
    {
    public:
        /// @param duration  Seconds per cycle.
        /// @param minRadius Minimum glow radius in pixels.
        /// @param maxRadius Maximum glow radius in pixels.
        GlowRadiusBreath(float duration = 2.0f,
                         float minRadius = 4.0f,
                         float maxRadius = 12.0f)
            : mOsc(1.0f / duration, minRadius, maxRadius) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.glowRadius = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    // -------------------------------------------------------------------------
    // neon.bloomStrength
    // -------------------------------------------------------------------------

    /// @brief Sine breathe on @c neon.bloomStrength.
    /// @details The wide spill swells in and out.
    class BloomPulse : public Animation
    {
    public:
        /// @param duration Seconds per cycle.
        /// @param min      Minimum bloom strength.
        /// @param max      Maximum bloom strength.
        BloomPulse(float duration = 1.5f,
                   float min = 0.1f,
                   float max = 0.6f)
            : mOsc(1.0f / duration, min, max) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.bloomStrength = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    // -------------------------------------------------------------------------
    // Travelling segment
    // -------------------------------------------------------------------------

    /// @brief A bright dot travelling around a full colour ring.
    /// @details Lays down a full-ring colour segment plus a short, bright,
    ///          white dot segment whose start sweeps around the perimeter
    ///          (sawtooth), wrapping seamlessly. Replaces the whole segment list.
    class SegmentTravel : public Animation
    {
    public:
        /// @param duration Seconds for one full revolution.
        /// @param length   Width of the spot as a fraction of the perimeter.
        /// @param level    Emission weight at the spot (>1 = brighter than base).
        SegmentTravel(float duration = 3.0f,
                      float length = 0.15f,
                      float level = 4.0f)
            : mPosOsc(1.0f / duration, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH),
              mLength(length), mLevel(level) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            // Full colour ring + a bright white dot whose centre rides the
            // oscillator (start offset by -length/2; wraps fine for negatives).
            LitSegment dot{mPosOsc.Evaluate(elapsed) - mLength * 0.5f, mLength, mLevel};
            cfg.neon.segments = {DefaultRingSegment(), dot};
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mLevel;
    };

    /// @brief A bright dot swinging back and forth along a full colour ring.
    /// @details Triangle wave on the dot's position. See @ref SegmentTravel.
    class SegmentBounce : public Animation
    {
    public:
        /// @param duration Seconds for one full back-and-forth cycle.
        /// @param length   Width of the spot as a fraction of the perimeter.
        /// @param level    Emission weight at the spot (>1 = brighter than base).
        SegmentBounce(float duration = 4.0f,
                      float length = 0.20f,
                      float level = 4.0f)
            : mPosOsc(1.0f / duration, 0.0f, 1.0f, 0.0f, Waveform::TRIANGLE),
              mLength(length), mLevel(level) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            LitSegment dot{mPosOsc.Evaluate(elapsed) - mLength * 0.5f, mLength, mLevel};
            cfg.neon.segments = {DefaultRingSegment(), dot};
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mLevel;
    };

    /// @brief Light the two side edges (left + right) and nothing else.
    /// @details Recomputes the two segments from geometry each frame (via
    ///          @ref GeometryUtils::GetEdgeArc) so they stay glued to the side
    ///          edges if the rect is resized; the rest of the perimeter stays
    ///          dark — the classic two-bar edge light. Static by default; pair
    ///          with @ref IntensityPulse to make the pair breathe.
    class SideEdges : public Animation
    {
    public:
        /// @param coverage Fraction of each edge's straight run that is lit
        ///                 (1 = full edge; <1 leaves a gap toward the corners).
        /// @param level    Emission weight of the two bars.
        SideEdges(float coverage = 0.95f, float level = 1.0f)
            : mCoverage(coverage), mLevel(level) {}

        void Apply(Config &cfg, float /*elapsed*/) const override
        {
            cfg.neon.segments = {
                edgeSegment(GeometryUtils::GetEdgeArc(Edge::LEFT, cfg.geometry)),
                edgeSegment(GeometryUtils::GetEdgeArc(Edge::RIGHT, cfg.geometry)),
            };
        }

    private:
        // GetEdgeArc returns {start, fullLength}; centre the shortened bar within
        // the edge so a <1 coverage leaves an equal gap at both corners.
        LitSegment edgeSegment(const glm::vec2 &edge) const
        {
            float len = edge.y * mCoverage;
            float start = edge.x + (edge.y - len) * 0.5f;
            return LitSegment{start, len, mLevel};
        }

        float mCoverage;
        float mLevel;
    };

    /// @brief Two side edges, each with its OWN colour and a white spot that
    ///        travels along it — a showcase of per-segment colour + nested spots.
    /// @details Left bar red->orange, right bar blue->cyan; a white-hot accent
    ///          slides up the left bar and down the right. All baked per-sample
    ///          on the CPU (ColorUtils::BuildSampleData), so it costs nothing
    ///          extra in the shader. Geometry-driven, so it tracks rect resizes.
    class SideEdgesAccent : public Animation
    {
    public:
        /// @param duration Seconds for the accent to traverse one bar.
        explicit SideEdgesAccent(float duration = 2.5f)
            : mTravel(1.0f / duration, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            float t = mTravel.Evaluate(elapsed);

            LitSegment left = edgeBar(GeometryUtils::GetEdgeArc(Edge::LEFT, cfg.geometry));
            left.colorStops = {{0.0f, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)},
                               {1.0f, glm::vec4(1.0f, 0.5f, 0.0f, 1.0f)}};
            left.spot = makeWhiteSpot(t);

            LitSegment right = edgeBar(GeometryUtils::GetEdgeArc(Edge::RIGHT, cfg.geometry));
            right.colorStops = {{0.0f, glm::vec4(0.0f, 0.4f, 1.0f, 1.0f)},
                                {1.0f, glm::vec4(0.0f, 1.0f, 1.0f, 1.0f)}};
            right.spot = makeWhiteSpot(1.0f - t);

            cfg.neon.segments = {left, right};
        }

    private:
        // GetEdgeArc returns {start, fullLength}; centre a 0.95-coverage bar.
        static LitSegment edgeBar(const glm::vec2 &edge)
        {
            float len = edge.y * 0.95f;
            return LitSegment{edge.x + (edge.y - len) * 0.5f, len, 1.0f};
        }

        // @p travel in [0,1] maps the spot's START so it stays fully inside the
        // bar (start runs 0 -> 1-length). spot.position is the start.
        static Spot makeWhiteSpot(float travel)
        {
            Spot s;
            s.length = 0.18f;
            s.position = travel * (1.0f - s.length);
            s.boost = 4.0f;
            s.colorStops = {{0.0f, glm::vec4(1.0f)}, {1.0f, glm::vec4(1.0f)}};
            return s;
        }

        Oscillator mTravel;
    };

    // -------------------------------------------------------------------------
    // Outline tracer
    // -------------------------------------------------------------------------

    /// @brief One-shot animation that progressively lights up the outline.
    /// @details Grows a single colour-ring segment from @c si = 0 forward until
    ///          it covers the whole perimeter over @p duration seconds, ending
    ///          fully lit and held — the rect "draws" itself. Replaces the
    ///          segment list (the perimeter is dark wherever the segment hasn't
    ///          reached yet).
    ///
    /// Combine with @ref IntensityFadeIn for a smoother appearance, or chain
    /// into a @ref SegmentTravel afterwards via @ref Animation::OnComplete for
    /// a "draw then keep travelling" routine.
    ///
    /// @ref SetDuration rebuilds the internal @ref Ease in lockstep with the
    /// completion latch.
    class OutlineTracer : public Animation
    {
    public:
        /// @param duration Total draw time in seconds.
        /// @param curve    Easing curve for the sweep.
        OutlineTracer(float duration = 2.0f,
                      EasingFunction::Curve curve = EasingFunction::OutCubic)
            : Animation(duration),
              mCurve(curve),
              mEase(0.0f, 1.0f, duration, curve, /*loop=*/false) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            // Grow a segment starting at si = 0, length 0 -> 1 (position = start),
            // carrying the default colour gradient.
            LitSegment seg = DefaultRingSegment();
            seg.position = 0.0f;
            seg.length = mEase.Evaluate(elapsed);
            cfg.neon.segments = {seg};
        }

    protected:
        void OnDurationChanged(float d) override
        {
            mEase = Ease(0.0f, 1.0f, d, mCurve, false);
        }

    private:
        EasingFunction::Curve mCurve;
        Ease mEase;
    };

    // -------------------------------------------------------------------------
    // Generic adapters
    // -------------------------------------------------------------------------
    // Each adapter drives a single Config field with any Modulator.
    // Useful for one-off sequence or composition cases that don't justify a
    // dedicated subclass.
    //
    // Two constructors:
    //   - one-arg : loops forever (duration = 0)
    //   - two-arg : one-shot for the given duration
    // Mode and duration are independent at runtime — see Animation::SetPlaybackMode
    // and Animation::SetDuration.

    /// @brief Drive @c neon.intensity with an arbitrary modulator.
    class IntensityCurve : public Animation
    {
    public:
        /// @param mod Modulator producing the intensity value (looping).
        explicit IntensityCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}

        /// @param mod      Modulator producing the intensity value.
        /// @param duration One-shot duration in seconds.
        IntensityCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.intensity = mMod->Evaluate(elapsed);
            }
        }

    private:
        ModulatorPtr mMod;
    };

    /// @brief Drive @c neon.glowRadius with an arbitrary modulator.
    class GlowRadiusCurve : public Animation
    {
    public:
        explicit GlowRadiusCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        GlowRadiusCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.glowRadius = mMod->Evaluate(elapsed);
            }
        }

    private:
        ModulatorPtr mMod;
    };

    /// @brief Drive @c neon.bloomStrength with an arbitrary modulator.
    class BloomStrengthCurve : public Animation
    {
    public:
        explicit BloomStrengthCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        BloomStrengthCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.bloomStrength = mMod->Evaluate(elapsed);
            }
        }

    private:
        ModulatorPtr mMod;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_ANIMATIONS_H_
