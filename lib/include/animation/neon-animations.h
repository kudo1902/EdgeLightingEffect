#ifndef _EDGE_LIGHTING_NEON_ANIMATIONS_H_
#define _EDGE_LIGHTING_NEON_ANIMATIONS_H_

#include "animation/animation.h"
#include "animation/easing-function.h"
#include "animation/modulator.h"
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
    /// @details Default 1 Hz oscillation between 0.4 and 1.0.
    class IntensityPulse : public Animation
    {
    public:
        /// @param frequency Cycles per second (Hz).
        /// @param min       Minimum intensity (lower bound).
        /// @param max       Maximum intensity (upper bound).
        IntensityPulse(float frequency = 1.0f, float min = 0.4f, float max = 1.0f)
            : mOsc(frequency, min, max) {}
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
        /// @param frequency     On/off cycles per second (Hz).
        /// @param offIntensity  Value when the strobe is off.
        /// @param onIntensity   Value when the strobe is on.
        IntensityStrobe(float frequency = 4.0f,
                        float offIntensity = 0.0f,
                        float onIntensity = 1.0f)
            : mOsc(frequency, offIntensity, onIntensity, 0.0f, Waveform::SQUARE) {}
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
        /// @param frequency Cycles per second.
        /// @param minRadius Minimum glow radius in pixels.
        /// @param maxRadius Maximum glow radius in pixels.
        GlowRadiusBreath(float frequency = 0.5f,
                         float minRadius = 4.0f,
                         float maxRadius = 12.0f)
            : mOsc(frequency, minRadius, maxRadius) {}
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
        /// @param frequency Cycles per second.
        /// @param min       Minimum bloom strength.
        /// @param max       Maximum bloom strength.
        BloomPulse(float frequency = 0.7f,
                   float min = 0.1f,
                   float max = 0.6f)
            : mOsc(frequency, min, max) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.bloomStrength = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    // -------------------------------------------------------------------------
    // neon.hueRotationRate
    // -------------------------------------------------------------------------

    /// @brief Flip the sign of @c neon.hueRotationRate periodically.
    /// @details Alternates direction every @p reverseEvery seconds with an
    ///          abrupt square-wave transition.
    class HueRotationReverse : public Animation
    {
    public:
        /// @param baseRate     Absolute rotation rate in cycles/second.
        /// @param reverseEvery Seconds between sign flips.
        HueRotationReverse(float baseRate = 0.5f, float reverseEvery = 4.0f)
            : mBaseRate(baseRate), mPeriod(reverseEvery) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            int n = static_cast<int>(std::floor(elapsed / mPeriod));
            cfg.neon.hueRotationRate = (n & 1) ? -mBaseRate : mBaseRate;
        }

    private:
        float mBaseRate;
        float mPeriod;
    };

    /// @brief Smoothly ease the hue rotation direction.
    /// @details Uses a triangle wave so direction reverses gradually — no
    ///          abrupt flips.
    class HueRotationEaseReverse : public Animation
    {
    public:
        /// @param maxRate Maximum absolute rotation rate.
        /// @param period  Full forward-backward cycle in seconds.
        HueRotationEaseReverse(float maxRate = 0.5f, float period = 6.0f)
            : mOsc(1.0f / period, -maxRate, maxRate, 0.0f, Waveform::TRIANGLE) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.hueRotationRate = mOsc.Evaluate(elapsed);
        }

    private:
        Oscillator mOsc;
    };

    // -------------------------------------------------------------------------
    // Travelling segment
    // -------------------------------------------------------------------------

    /// @brief Spin a bright Gaussian peak around the perimeter.
    /// @details Uses a sawtooth on @c segmentPosition so the peak wraps around
    ///          and restarts from 0.
    class SegmentTravel : public Animation
    {
    public:
        /// @param secondsPerRevolution Time for one full lap in seconds.
        /// @param length               Width of the bright peak [0, 1].
        /// @param boost                Peak intensity multiplier.
        SegmentTravel(float secondsPerRevolution = 3.0f,
                      float length = 0.15f,
                      float boost = 4.0f)
            : mPosOsc(1.0f / secondsPerRevolution, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH),
              mLength(length), mBoost(boost) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.segmentPosition = mPosOsc.Evaluate(elapsed);
            cfg.neon.segmentLength = mLength;
            cfg.neon.segmentBoost = mBoost;
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mBoost;
    };

    /// @brief Swing the bright spot back and forth.
    /// @details Uses a triangle wave on @c segmentPosition.
    class SegmentBounce : public Animation
    {
    public:
        /// @param period Full back-and-forth cycle in seconds.
        /// @param length  Width of the bright peak [0, 1].
        /// @param boost   Peak intensity multiplier.
        SegmentBounce(float period = 4.0f,
                      float length = 0.20f,
                      float boost = 4.0f)
            : mPosOsc(1.0f / period, 0.0f, 1.0f, 0.0f, Waveform::TRIANGLE),
              mLength(length), mBoost(boost) {}

        void Apply(Config &cfg, float elapsed) const override
        {
            cfg.neon.segmentPosition = mPosOsc.Evaluate(elapsed);
            cfg.neon.segmentLength = mLength;
            cfg.neon.segmentBoost = mBoost;
        }

    private:
        Oscillator mPosOsc;
        float mLength;
        float mBoost;
    };

    // -------------------------------------------------------------------------
    // Outline tracer
    // -------------------------------------------------------------------------

    /// @brief One-shot animation that progressively lights up the outline.
    /// @details Sweeps @c neon.arcLength from 0 to 1 over @p duration seconds,
    ///          ending fully lit and held. Does NOT touch @c neon.arcStart —
    ///          set that externally (slider, code, another animation) to
    ///          choose where the trace begins. This separation lets users
    ///          drag the Arc Start slider freely while the tracer is running.
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
            cfg.neon.arcLength = mEase.Evaluate(elapsed);
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
    //   - one-arg : loops forever (duration = -1)
    //   - two-arg : one-shot for the given duration
    // Mode can be flipped at runtime via SetDuration.

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

    /// @brief Drive @c neon.hueRotationRate with an arbitrary modulator.
    class HueRotationRateCurve : public Animation
    {
    public:
        explicit HueRotationRateCurve(ModulatorPtr mod) : Animation(), mMod(std::move(mod)) {}
        HueRotationRateCurve(ModulatorPtr mod, float duration) : Animation(duration), mMod(std::move(mod)) {}
        void Apply(Config &cfg, float elapsed) const override
        {
            if (mMod)
            {
                cfg.neon.hueRotationRate = mMod->Evaluate(elapsed);
            }
        }

    private:
        ModulatorPtr mMod;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_ANIMATIONS_H_
