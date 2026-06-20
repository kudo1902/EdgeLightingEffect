#ifndef _EDGE_LIGHTING_MODULATOR_H_
#define _EDGE_LIGHTING_MODULATOR_H_

#include "animation/easing-function.h"
#include "util/constants.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace EdgeLighting
{
    /// @defgroup modulators Modulators
    /// @brief Time → float signal sources for driving animated parameters.
    ///
    /// The whole animation system reduces to one idea: every animated parameter
    /// is a function of time. A Modulator IS that function. Concrete modulators
    /// implement different shapes (constant, oscillator, ease curve, …) and can
    /// be composed (multiplier, adder, sequence) to build arbitrarily complex
    /// behaviour without growing the orchestrator.
    ///
    /// Usage pattern, per frame:
    /// @code
    ///     clock.Update(deltaTime);
    ///     float t = clock.GetTime();
    ///     cfg.neon.intensity  = pulse.Evaluate(t);
    ///     cfg.neon.glowRadius = glowSweep.Evaluate(t);
    /// @endcode
    ///
    /// No coupling to Config / no internal pointer state — modulators are pure
    /// functions of time, so they're trivially shareable, testable, and
    /// composable.
    ///
    /// @{

    /// @brief Pure virtual base for all modulators.
    /// @details A Modulator is a callable that maps a time value to a float.
    ///          Subclasses must implement @ref Evaluate.
    class Modulator
    {
    public:
        virtual ~Modulator() = default;

        /// @brief Evaluate the modulator at the given time.
        /// @param time Current clock time in seconds.
        /// @return The modulated value at @p time.
        /// @note Implementations should be pure (no side effects, no state
        ///       advanced by this call).
        virtual float Evaluate(float time) const = 0;
    };

    /// @brief Shared owning reference to a Modulator.
    using ModulatorPtr = std::shared_ptr<Modulator>;

    /// @brief Always returns the same constant value.
    /// @details Useful as a sentinel, placeholder, or fixed offset in
    ///          compositions.
    class Constant : public Modulator
    {
    public:
        /// @param value The value returned for every time input.
        explicit Constant(float value) : mValue(value) {}
        float Evaluate(float) const override { return mValue; }

    private:
        float mValue;
    };

    /// @brief Periodic waveform shapes for @ref Oscillator.
    typedef enum class Waveform
    {
        SINE,     ///< Smooth periodic (default).
        TRIANGLE, ///< Linear up + linear down.
        SQUARE,   ///< Hard on/off — useful for strobes.
        SAWTOOTH  ///< Linear ramp 0→1, snap back.
    } Waveform;

    /// @brief Periodic modulator producing a waveform in [min, max].
    /// @details Output cycles at the given frequency, shaped by the chosen
    ///          @ref Waveform.
    /// @code
    ///     Oscillator pulse(1.0f, 0.3f, 1.0f);              // 1 Hz sine, 0.3..1.0
    ///     Oscillator strobe(4.0f, 0.0f, 1.0f, 0.0f,         // 4 Hz square
    ///                       Waveform::SQUARE);
    /// @endcode
    class Oscillator : public Modulator
    {
    public:
        /// @param frequency Cycles per second (Hz).
        /// @param min       Lower bound of the output range.
        /// @param max       Upper bound of the output range.
        /// @param phase     Initial phase offset in cycles [0, 1).
        /// @param waveform  Shape of the oscillation.
        Oscillator(float frequency,
                   float min = 0.0f,
                   float max = 1.0f,
                   float phase = 0.0f,
                   Waveform waveform = Waveform::SINE)
            : mFreq(frequency), mMin(min), mMax(max), mPhase(phase), mWave(waveform)
        {
        }

        float Evaluate(float time) const override
        {
            float t = mFreq * time + mPhase;
            t -= std::floor(t);
            float w;
            switch (mWave)
            {
            case Waveform::SINE:
            {
                w = 0.5f + 0.5f * std::sin(t * 2.0f * PI);
                break;
            }
            case Waveform::TRIANGLE:
            {
                w = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);
                break;
            }
            case Waveform::SQUARE:
            {
                w = (t < 0.5f) ? 0.0f : 1.0f;
                break;
            }
            case Waveform::SAWTOOTH:
            default:
            {
                w = t;
                break;
            }
            }
            return mMin + (mMax - mMin) * w;
        }

    private:
        float mFreq;
        float mMin, mMax;
        float mPhase;
        Waveform mWave;
    };

    /// @brief One-shot or looping interpolation shaped by an easing curve.
    /// @details Transitions from @p from to @p to over @p duration seconds.
    ///          Loops back to the start when @p loop is true.
    /// @code
    ///     Ease fadeIn(0.0f, 1.0f, 0.3f, EasingFunction::OutCubic);        // 300 ms fade-in
    ///     Ease wobble(5.0f, 20.0f, 2.0f, EasingFunction::InOutSine, true); // looping 2 s
    /// @endcode
    class Ease : public Modulator
    {
    public:
        /// @param from     Start value at time 0.
        /// @param to       End value after @p duration seconds.
        /// @param duration Length of the transition in seconds.
        /// @param curve    Easing curve that shapes the interpolation.
        /// @param loop     If true, the transition repeats when time exceeds duration.
        Ease(float from,
             float to,
             float duration,
             EasingFunction::Curve curve = EasingFunction::Linear,
             bool loop = false)
            : mFrom(from), mTo(to), mDuration(duration), mCurve(curve), mLoop(loop)
        {
        }

        float Evaluate(float time) const override
        {
            if (mDuration <= 0.0f)
            {
                return mTo;
            }
            float t = time / mDuration;
            if (mLoop)
            {
                t -= std::floor(t);
            }
            else
            {
                t = std::min(1.0f, std::max(0.0f, t));
            }
            float eased = mCurve ? mCurve(t) : t;
            return mFrom + (mTo - mFrom) * eased;
        }

    private:
        float mFrom, mTo;
        float mDuration;
        EasingFunction::Curve mCurve;
        bool mLoop;
    };

    /// @brief Chain modulators back-to-back in time.
    /// @details Each segment runs for its own duration. When the last segment
    ///          ends, the sequence either loops (default) or holds the final value.
    /// @code
    ///     auto flash = std::make_shared<Sequence>();
    ///     flash->Append(std::make_shared<Constant>(1.0f), 0.05f);
    ///     flash->Append(std::make_shared<Constant>(0.0f), 0.05f);
    ///     flash->Append(std::make_shared<Constant>(1.0f), 0.05f);
    ///     flash->Append(std::make_shared<Ease>(1.0f, 0.6f, 1.0f, EasingFunction::OutCubic), 1.0f);
    /// @endcode
    class Sequence : public Modulator
    {
    public:
        /// @brief Append a modulator segment.
        /// @param mod      The modulator to run during this segment.
        /// @param duration How long the segment lasts in seconds.
        void Append(ModulatorPtr mod, float duration)
        {
            mTotalDuration += duration;
            mSegments.push_back({std::move(mod), duration});
        }

        /// @brief Enable or disable looping.
        void SetLoop(bool loop) { mLoop = loop; }

        float Evaluate(float time) const override
        {
            if (mSegments.empty())
            {
                return 0.0f;
            }
            float t = time;
            if (mLoop && mTotalDuration > 0.0f)
            {
                t = std::fmod(t, mTotalDuration);
            }
            for (const auto &seg : mSegments)
            {
                if (t < seg.second)
                {
                    return seg.first->Evaluate(t);
                }
                t -= seg.second;
            }
            return mSegments.back().first->Evaluate(mSegments.back().second);
        }

    private:
        std::vector<std::pair<ModulatorPtr, float>> mSegments;
        float mTotalDuration = 0.0f;
        bool mLoop = true;
    };

    /// @brief Product of two modulators.
    /// @details Useful for amplitude-modulating an oscillator with a slow
    ///          envelope (e.g. fade-in × pulse).
    class Multiplier : public Modulator
    {
    public:
        /// @param a First modulator.
        /// @param b Second modulator.
        Multiplier(ModulatorPtr a, ModulatorPtr b) : mA(std::move(a)), mB(std::move(b)) {}
        float Evaluate(float time) const override { return mA->Evaluate(time) * mB->Evaluate(time); }

    private:
        ModulatorPtr mA, mB;
    };

    /// @brief Sum of two modulators.
    /// @details Useful for layering (e.g. base level + sparkle).
    class Adder : public Modulator
    {
    public:
        /// @param a First modulator.
        /// @param b Second modulator.
        Adder(ModulatorPtr a, ModulatorPtr b) : mA(std::move(a)), mB(std::move(b)) {}
        float Evaluate(float time) const override { return mA->Evaluate(time) + mB->Evaluate(time); }

    private:
        ModulatorPtr mA, mB;
    };

    /// @brief Rescale a modulator's output into [outMin, outMax].
    /// @details Handy adaptor when you want to use easing curves directly
    ///          without writing an Ease modulator.
    class Remap : public Modulator
    {
    public:
        /// @param inner  Modulator whose [0,1]-ish output to rescale.
        /// @param outMin Lower bound of the output range.
        /// @param outMax Upper bound of the output range.
        Remap(ModulatorPtr inner, float outMin, float outMax)
            : mInner(std::move(inner)), mMin(outMin), mMax(outMax) {}
        float Evaluate(float time) const override
        {
            float v = mInner->Evaluate(time);
            return mMin + (mMax - mMin) * v;
        }

    private:
        ModulatorPtr mInner;
        float mMin, mMax;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_MODULATOR_H_
