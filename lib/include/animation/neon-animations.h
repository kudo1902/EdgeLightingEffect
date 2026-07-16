#ifndef _EDGE_LIGHTING_NEON_ANIMATIONS_H_
#define _EDGE_LIGHTING_NEON_ANIMATIONS_H_

#include "animation/animation.h"
#include "animation/easing-function.h"
#include "animation/modulator.h"
#include "util/log-util.h"
#include <cmath>

namespace EdgeLighting
{
    /// @defgroup neon_animations Neon Animations
    /// @brief Ready-made @ref Animation subclasses that drive specific
    ///        @ref NeonConfig fields.
    ///
    /// Each class wraps one particular use of the @ref Modulator primitives.
    /// Constructors take only the parameters that matter for the effect,
    /// defaulting the rest to sensible values - call sites are typically one
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
    /// @details Default one-second cycle between 0.4 and 1.0. @ref SetDuration
    ///          rebuilds the internal Oscillator so the debug UI's Duration
    ///          slider live-adjusts the breathing period.
    class IntensityPulse : public Animation
    {
    public:
        /// @param duration Seconds per cycle.
        /// @param min      Minimum intensity (lower bound).
        /// @param max      Maximum intensity (upper bound).
        IntensityPulse(float duration = 1.0f, float min = 0.4f, float max = 1.0f)
            : mOsc(1.0f / duration, min, max), mMin(min), mMax(max)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mOsc.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedIntensity = cfg.neon.intensity; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.intensity = mSavedIntensity; }
        void OnDurationChanged(float d) override
        {
            mOsc = Oscillator(1.0f / d, mMin, mMax);
        }

    private:
        Oscillator mOsc;
        float mMin;
        float mMax;
        float mSavedIntensity = 0.0f;
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
            : mOsc(1.0f / duration, offIntensity, onIntensity, 0.0f, Waveform::SQUARE),
              mOff(offIntensity), mOn(onIntensity)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mOsc.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedIntensity = cfg.neon.intensity; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.intensity = mSavedIntensity; }
        void OnDurationChanged(float d) override
        {
            mOsc = Oscillator(1.0f / d, mOff, mOn, 0.0f, Waveform::SQUARE);
        }

    private:
        Oscillator mOsc;
        float mOff;
        float mOn;
        float mSavedIntensity = 0.0f;
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

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mEase.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedIntensity = cfg.neon.intensity; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.intensity = mSavedIntensity; }
        void OnDurationChanged(float d) override
        {
            mEase = Ease(0.0f, mTarget, d, mCurve, false);
        }

    private:
        float mTarget;
        EasingFunction::Curve mCurve;
        Ease mEase;
        float mSavedIntensity = 0.0f;
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

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.intensity = mEase.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedIntensity = cfg.neon.intensity; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.intensity = mSavedIntensity; }
        void OnDurationChanged(float d) override
        {
            mEase = Ease(mStart, 0.0f, d, mCurve, false);
        }

    private:
        float mStart;
        EasingFunction::Curve mCurve;
        Ease mEase;
        float mSavedIntensity = 0.0f;
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
            : mOsc(1.0f / duration, minRadius, maxRadius),
              mMin(minRadius), mMax(maxRadius)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.glowRadius = mOsc.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedGlowRadius = cfg.neon.glowRadius; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.glowRadius = mSavedGlowRadius; }
        void OnDurationChanged(float d) override
        {
            mOsc = Oscillator(1.0f / d, mMin, mMax);
        }

    private:
        Oscillator mOsc;
        float mMin;
        float mMax;
        float mSavedGlowRadius = 0.0f;
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
            : mOsc(1.0f / duration, min, max), mMin(min), mMax(max)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.bloomStrength = mOsc.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedBloom = cfg.neon.bloomStrength; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.bloomStrength = mSavedBloom; }
        void OnDurationChanged(float d) override
        {
            mOsc = Oscillator(1.0f / d, mMin, mMax);
        }

    private:
        Oscillator mOsc;
        float mMin;
        float mMax;
        float mSavedBloom = 0.0f;
    };

    // -------------------------------------------------------------------------
    // neon.hueRotationRate
    // -------------------------------------------------------------------------

    /// @brief Flip the sign of @c neon.hueRotationRate periodically.
    /// @details A full forward-backward cycle takes @p duration seconds, with
    ///          an abrupt square-wave transition at the half-cycle mark.
    class HueRotationReverse : public Animation
    {
    public:
        /// @param baseRate Absolute rotation rate in cycles/second.
        /// @param duration Seconds for one full forward-backward cycle (sign flips at duration/2).
        HueRotationReverse(float baseRate = 0.5f, float duration = 8.0f)
            : mBaseRate(baseRate), mHalfPeriod(duration * 0.5f)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            int n = static_cast<int>(std::floor(elapsed / mHalfPeriod));
            cfg.neon.hueRotationRate = (n & 1) ? -mBaseRate : mBaseRate;
        }

        void CaptureBaseline(const Config &cfg) override { mSavedHueRate = cfg.neon.hueRotationRate; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.hueRotationRate = mSavedHueRate; }
        void OnDurationChanged(float d) override
        {
            mHalfPeriod = d * 0.5f;
        }

    private:
        float mBaseRate;
        float mHalfPeriod;
        float mSavedHueRate = 0.0f;
    };

    /// @brief Smoothly ease the hue rotation direction.
    /// @details Uses a triangle wave so direction reverses gradually - no
    ///          abrupt flips.
    class HueRotationEaseReverse : public Animation
    {
    public:
        /// @param maxRate  Maximum absolute rotation rate.
        /// @param duration Seconds for one full forward-backward cycle.
        HueRotationEaseReverse(float maxRate = 0.5f, float duration = 6.0f)
            : mOsc(1.0f / duration, -maxRate, maxRate, 0.0f, Waveform::TRIANGLE),
              mMaxRate(maxRate)
        {
            SetDuration(duration);
        }
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            cfg.neon.hueRotationRate = mOsc.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedHueRate = cfg.neon.hueRotationRate; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.hueRotationRate = mSavedHueRate; }
        void OnDurationChanged(float d) override
        {
            mOsc = Oscillator(1.0f / d, -mMaxRate, mMaxRate, 0.0f, Waveform::TRIANGLE);
        }

    private:
        Oscillator mOsc;
        float mMaxRate;
        float mSavedHueRate = 0.0f;
    };

    // -------------------------------------------------------------------------
    // Travelling segment
    // -------------------------------------------------------------------------

    namespace
    {
        /// Grow @c cfg.neon.segmentBoosts up to @p index inclusive so a
        /// travelling-segment animation can safely write to
        /// @c segmentBoosts[index] regardless of the current size.
        /// New entries are initialised with the animation's own length/boost
        /// so a freshly added animation immediately produces a visible bump.
        inline SegmentBoost &EnsureSegmentSlot(Config &cfg, size_t index,
                                               float defaultLength, float defaultBoost)
        {
            if (cfg.neon.segmentBoosts.size() <= index)
            {
                cfg.neon.segmentBoosts.resize(index + 1, SegmentBoost{0.0f, defaultLength, defaultBoost});
            }
            return cfg.neon.segmentBoosts[index];
        }
    }

    /// @brief Spin a bright Gaussian peak around the perimeter.
    /// @details Uses a sawtooth on the target segment's @c position so the peak
    ///          wraps around and restarts from 0. Writes into
    ///          @c cfg.neon.segmentBoosts[index], auto-growing the vector to
    ///          include that slot if needed.
    class SegmentTravel : public Animation
    {
    public:
        /// @param duration Seconds for one full revolution.
        /// @param length   Width of the bright peak [0, 1].
        /// @param boost    Peak intensity multiplier.
        /// @param index    Which entry in @c segmentBoosts to drive (default 0).
        SegmentTravel(float duration = 3.0f,
                      float length = 0.15f,
                      float boost = 4.0f,
                      size_t index = 0)
            : mPosOsc(1.0f / duration, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH),
              mLength(length), mBoost(boost), mIndex(index)
        {
            SetDuration(duration);
        }

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            auto &s = EnsureSegmentSlot(cfg, mIndex, mLength, mBoost);
            s.position = mPosOsc.Evaluate(elapsed);
            s.length = mLength;
            s.boost = mBoost;
        }

        // Segment animations auto-grow segmentBoosts and mutate a specific
        // slot in-place. Snapshotting the whole vector is the simplest way
        // to restore the pre-play state - including "vector was empty" or
        // "other segments had different values at snapshot time".
        void CaptureBaseline(const Config &cfg) override { mSavedBoosts = cfg.neon.segmentBoosts; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.segmentBoosts = mSavedBoosts; }
        void OnDurationChanged(float d) override
        {
            mPosOsc = Oscillator(1.0f / d, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH);
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mBoost;
        size_t mIndex;
        std::vector<SegmentBoost> mSavedBoosts;
    };

    /// @brief Swing the bright spot back and forth.
    /// @details Uses a triangle wave on the target segment's @c position.
    ///          Writes into @c cfg.neon.segmentBoosts[index] (auto-grown).
    class SegmentBounce : public Animation
    {
    public:
        /// @param duration Seconds for one full back-and-forth cycle.
        /// @param length   Width of the bright peak [0, 1].
        /// @param boost    Peak intensity multiplier.
        /// @param index    Which entry in @c segmentBoosts to drive (default 0).
        SegmentBounce(float duration = 4.0f,
                      float length = 0.20f,
                      float boost = 4.0f,
                      size_t index = 0)
            : mPosOsc(1.0f / duration, 0.0f, 1.0f, 0.0f, Waveform::TRIANGLE),
              mLength(length), mBoost(boost), mIndex(index)
        {
            SetDuration(duration);
        }

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            auto &s = EnsureSegmentSlot(cfg, mIndex, mLength, mBoost);
            s.position = mPosOsc.Evaluate(elapsed);
            s.length = mLength;
            s.boost = mBoost;
        }

        void CaptureBaseline(const Config &cfg) override { mSavedBoosts = cfg.neon.segmentBoosts; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.segmentBoosts = mSavedBoosts; }
        void OnDurationChanged(float d) override
        {
            mPosOsc = Oscillator(1.0f / d, 0.0f, 1.0f, 0.0f, Waveform::TRIANGLE);
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mBoost;
        size_t mIndex;
        std::vector<SegmentBoost> mSavedBoosts;
    };

    // -------------------------------------------------------------------------
    // Outline tracer
    // -------------------------------------------------------------------------

    /// @brief One-shot animation that progressively lights up the outline.
    /// @details Sweeps @c neon.arcs[0].length from 0 to 1 over @p duration
    ///          seconds, ending fully lit and held. Does NOT touch
    ///          @c arcs[0].start - set that externally (slider, code, another
    ///          animation) to choose where the trace begins.
    ///
    /// Auto-grows @c cfg.neon.arcs to contain at least entry [0] on first
    /// write (default config already has one full-perimeter arc, so this is
    /// only a safety net for hosts that clear the vector).
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

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            if (cfg.neon.arcs.empty())
            {
                cfg.neon.arcs.push_back(Arc{});
            }
            cfg.neon.arcs[0].length = mEase.Evaluate(elapsed);
        }

        void CaptureBaseline(const Config &cfg) override { mSavedArcs = cfg.neon.arcs; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.arcs = mSavedArcs; }
        void OnDurationChanged(float d) override
        {
            mEase = Ease(0.0f, 1.0f, d, mCurve, false);
        }

    private:
        EasingFunction::Curve mCurve;
        Ease mEase;
        std::vector<Arc> mSavedArcs;
    };

    /// @brief Three-phase arc wipe: grow, chase, shrink.
    /// @details A "wipe" that draws the neon arc from @c startPos, races it
    ///          around the perimeter at a fixed maximum length, and shrinks it
    ///          away as the tail reaches @c endPos.
    ///
    /// - **Phase 1 - grow**  : @c arcStart stays at @c startPos; @c arcLength
    ///                         grows 0 → @c maxLength.  The head moves out
    ///                         from @c startPos.
    /// - **Phase 2 - chase** : @c arcLength stays at @c maxLength; both head
    ///                         and tail advance at the same speed.  The head
    ///                         travels from @c (startPos + maxLength) to
    ///                         @c endPos.
    /// - **Phase 3 - shrink**: head parks at @c endPos; tail catches up.
    ///                         @c arcLength shrinks @c maxLength → 0.
    ///
    /// The per-phase durations are chosen so the head and tail move at the
    /// same constant speed across all three phases - no visible acceleration
    /// at phase boundaries.  Given a total @c duration D:
    ///     speed = (totalDist + maxLength) / D
    ///     t1 = t3 = maxLength / speed
    ///     t2 = D − 2·t1
    /// where @c totalDist is the head's sweep from @c startPos to @c endPos
    /// (unwrapped forward, so @c endPos < @c startPos wraps around through 0).
    ///
    /// Positions can wrap. @c endPos <= @c startPos means "go around" (add 1
    /// to the unwrapped endpoint), so a wipe from 0.8 → 0.2 travels 0.4 units
    /// forward through 0 rather than 0.6 units backward. In particular,
    /// **@c startPos == @c endPos is treated as a full loop** (totalDist = 1)
    /// so a default `ArcWipe(3.0)` sweeps the whole perimeter.
    ///
    /// An @c EasingFunction::Curve can shape the wall-clock progression: the
    /// eased time drives the phase math, so an @c OutCubic (say) starts the
    /// grow fast, blazes through the chase, and lingers on the shrink. Default
    /// is @c Linear (constant head/tail speed).
    ///
    /// Defaults to @c ONE_SHOT since a wipe is naturally a fire-and-forget
    /// gesture; toggle @ref SetPlaybackMode to LOOP for a repeating chase.
    /// @ref SetDuration rebuilds the phase timings so the slider works live.
    class ArcWipe : public Animation
    {
    public:
        /// @param duration   Total wipe time in seconds.
        /// @param startPos   Perimeter position [0, 1) where the tail begins;
        ///                   arcStart stays here during phase 1.
        /// @param endPos     Perimeter position [0, 1) where both ends meet
        ///                   at the end of the wipe. The head parks here at
        ///                   the end of phase 2; the tail closes the gap
        ///                   during phase 3 and reaches @p endPos exactly
        ///                   when arcLength hits 0. Equal to @p startPos →
        ///                   full loop.
        /// @param maxLength  Arc length during the chase phase [0, 1).
        /// @param curve      Easing applied to the whole wipe timeline.
        ArcWipe(float duration = 2.0f,
                float startPos = 0.0f,
                float endPos = 1.0f,
                float maxLength = 0.25f,
                EasingFunction::Curve curve = EasingFunction::Linear)
            : Animation(duration),
              mStartPos(startPos),
              mEndPos(endPos),
              mMaxLength(maxLength),
              mCurve(curve)
        {
            computePhases(duration);
        }

        void ApplyAt(Config &cfg, float elapsed) const override
        {
            float arcStart = mStartPos;
            float arcLength = 0.0f;

            // Apply the easing curve to the timeline. Curve maps [0, 1] to
            // [0, 1]; multiply back by duration so t remains in seconds and
            // the phase-boundary comparisons below still make sense.
            float t = elapsed;
            float dur = GetDuration();
            if (mCurve && dur > 0.0f)
            {
                t = mCurve(elapsed / dur) * dur;
            }

            if (t <= mT1)
            {
                // Phase 1: grow. Tail fixed at startPos, head advances.
                float p = mT1 > 0.0f ? (t / mT1) : 1.0f;
                arcStart = mStartPos;
                arcLength = p * mEffMaxLength;
            }
            else if (t <= mT1 + mT2)
            {
                // Phase 2: chase. Both head and tail advance in lockstep,
                // length constant. Chase ends when the HEAD reaches endPos,
                // so the tail has covered (totalDist - effMaxLength).
                float p = mT2 > 0.0f ? ((t - mT1) / mT2) : 1.0f;
                arcStart = mStartPos + p * (mTotalDist - mEffMaxLength);
                arcLength = mEffMaxLength;
            }
            else
            {
                // Phase 3: shrink. Head parks at endPos; the tail travels its
                // final effMaxLength stretch to reach endPos as arcLength winds
                // down to 0. arcStart at end == endPos, arcLength == 0.
                float p = mT3 > 0.0f ? ((t - mT1 - mT2) / mT3) : 1.0f;
                arcLength = (1.0f - p) * mEffMaxLength;
                arcStart = mStartPos + mTotalDist - arcLength;
            }

            // Wrap arcStart to [0, 1) - the shader's arcInside handles the
            // wrap-around at start+length, but the raw arcStart itself must
            // live in [0, 1).
            arcStart = arcStart - std::floor(arcStart);

            if (cfg.neon.arcs.empty())
            {
                cfg.neon.arcs.push_back(Arc{});
            }
            cfg.neon.arcs[0].start = arcStart;
            cfg.neon.arcs[0].length = arcLength;
        }

        void CaptureBaseline(const Config &cfg) override { mSavedArcs = cfg.neon.arcs; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.arcs = mSavedArcs; }
        void OnDurationChanged(float d) override { computePhases(d); }

    private:
        void computePhases(float duration)
        {
            // Unwrap endPos forward so an endPos <= startPos means "loop
            // around through 0" - the wipe always advances CCW/forward, never
            // reverses.
            float endUnwrapped = mEndPos;
            if (endUnwrapped <= mStartPos)
            {
                endUnwrapped += 1.0f;
            }
            mTotalDist = endUnwrapped - mStartPos;
            // Effective max length: clamp to totalDist so a wipe from 0.1 → 0.2
            // with maxLength=0.5 doesn't overshoot endPos by growing the arc
            // to 0.5. With effMax == totalDist the chase phase vanishes (t2=0)
            // and the wipe becomes grow-then-shrink between the two endpoints.
            mEffMaxLength = std::min(mMaxLength, mTotalDist);

            // Constant-speed timing at rate s = (totalDist + effMaxLength) / duration:
            //   Phase 1: head advances effMaxLength while tail sits at startPos.
            //   Phase 2: both head and tail advance (totalDist - effMaxLength);
            //            ends when the head reaches endPos.
            //   Phase 3: head parked at endPos; tail closes the final
            //            effMaxLength stretch as arcLength winds down to 0.
            //   t1 = t3 = effMaxLength / s  ;  t2 = (totalDist - effMaxLength) / s
            if (duration > 0.0f)
            {
                float speed = (mTotalDist + mEffMaxLength) / duration;
                mT1 = (speed > 0.0f) ? (mEffMaxLength / speed) : 0.0f;
                mT3 = mT1;
                mT2 = duration - mT1 - mT3;
            }
            else
            {
                mT1 = mT2 = mT3 = 0.0f;
            }
        }

        float mStartPos;
        float mEndPos;
        float mMaxLength;
        EasingFunction::Curve mCurve;
        float mTotalDist = 0.0f;
        float mEffMaxLength = 0.0f;
        float mT1 = 0.0f;
        float mT2 = 0.0f;
        float mT3 = 0.0f;
        std::vector<Arc> mSavedArcs;
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
    // Mode and duration are independent at runtime - see Animation::SetPlaybackMode
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
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.intensity = mMod->Evaluate(elapsed);
            }
        }

        void CaptureBaseline(const Config &cfg) override { mSavedIntensity = cfg.neon.intensity; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.intensity = mSavedIntensity; }

    private:
        ModulatorPtr mMod;
        float mSavedIntensity = 0.0f;
    };

    /// @brief Drive @c neon.glowRadius with an arbitrary modulator.
    class GlowRadiusCurve : public Animation
    {
    public:
        explicit GlowRadiusCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        GlowRadiusCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.glowRadius = mMod->Evaluate(elapsed);
            }
        }

        void CaptureBaseline(const Config &cfg) override { mSavedGlowRadius = cfg.neon.glowRadius; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.glowRadius = mSavedGlowRadius; }

    private:
        ModulatorPtr mMod;
        float mSavedGlowRadius = 0.0f;
    };

    /// @brief Drive @c neon.bloomStrength with an arbitrary modulator.
    class BloomStrengthCurve : public Animation
    {
    public:
        explicit BloomStrengthCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        BloomStrengthCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.bloomStrength = mMod->Evaluate(elapsed);
            }
        }

        void CaptureBaseline(const Config &cfg) override { mSavedBloom = cfg.neon.bloomStrength; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.bloomStrength = mSavedBloom; }

    private:
        ModulatorPtr mMod;
        float mSavedBloom = 0.0f;
    };

    /// @brief Drive @c neon.hueRotationRate with an arbitrary modulator.
    class HueRotationRateCurve : public Animation
    {
    public:
        explicit HueRotationRateCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        HueRotationRateCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void ApplyAt(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.hueRotationRate = mMod->Evaluate(elapsed);
            }
        }

        void CaptureBaseline(const Config &cfg) override { mSavedHueRate = cfg.neon.hueRotationRate; }

    protected:
        void RestoreBaseline(Config &cfg) const override { cfg.neon.hueRotationRate = mSavedHueRate; }

    private:
        ModulatorPtr mMod;
        float mSavedHueRate = 0.0f;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_ANIMATIONS_H_
