#ifndef _EDGE_LIGHTING_CLOCK_H_
#define _EDGE_LIGHTING_CLOCK_H_

#include <cmath>

namespace EdgeLighting
{
    /// @brief Play/pause time accumulator.
    ///
    /// Owned by @ref EdgeLightingEffect. Each frame, @ref Update returns the
    /// delta that gets forwarded to every attached animation - a paused clock
    /// returns 0, so all animations freeze in lockstep without needing per-
    /// animation state changes.
    ///
    /// The absolute @ref GetTime is what every renderer is handed as its
    /// @c time. It is kept in DOUBLE: an effect may run for days, and a float
    /// accumulator loses the frame step - at 60 fps it advanced 10 s of frames
    /// by 9.375 s after half a day, 18.75 s after three, and by nothing at all
    /// past 2^19 s (~6.07 days), where the step is under half an ulp and the
    /// clock froze. A double keeps a 1/60 s step exact to well under a
    /// microsecond for millennia.
    ///
    /// No shader is handed this time directly: a float uniform of an absolute
    /// time has the same problem on the GPU. Each renderer reduces it on the
    /// CPU, in double, to a bounded phase - the neon's hue phase, the flare's
    /// ray rotation, the droplets' wrapped time - and uploads that. Those are
    /// driven by rates the config holds, so each is a @ref PhaseAccumulator,
    /// which integrates the rate over this time rather than multiplying the
    /// two (@ref GetCyclePhase, @ref WrapPeriod are the constant-rate forms).
    class Clock
    {
    public:
        Clock() = default;

        /// @brief Start accumulating time.
        /// @note The clock starts playing by default.
        void Play() { mIsPlaying = true; }

        /// @brief Freeze time at the current value.
        void Pause() { mIsPlaying = false; }

        /// @brief Pause and reset accumulated time to 0.
        void Stop()
        {
            mIsPlaying = false;
            mTime = 0.0;
        }

        /// @brief Reset accumulated time to 0 without changing play state.
        void Reset() { mTime = 0.0; }

        /// @brief Advance the clock by @p deltaTime.
        /// @param deltaTime Frame delta in seconds.
        /// @return How far the clock advanced this call - @p deltaTime while
        ///         playing, 0 while paused. Lets callers drive animations in
        ///         lockstep with the clock's play state.
        float Update(float deltaTime)
        {
            if (!mIsPlaying)
            {
                return 0.0f;
            }
            mTime += static_cast<double>(deltaTime);
            return deltaTime;
        }

        /// @brief Jump to an explicit time value.
        /// @param time New clock time in seconds (useful for scrubbing/testing).
        void SetTime(double time) { mTime = time; }

        /// @brief Current accumulated time in seconds.
        double GetTime() const { return mTime; }

        /// @brief Whether the clock is currently accumulating time.
        bool IsPlaying() const { return mIsPlaying; }

    private:
        double mTime = 0.0;
        bool mIsPlaying = true;
    };

    /// @brief @p value wrapped into [0, @p period), in double.
    ///
    /// The floor form rather than @c std::fmod so a negative @p value (a
    /// negative rate) wraps to the same non-negative range. A @p value just
    /// under a multiple of @p period can round to exactly @p period, which is
    /// returned as 0.
    /// @param period Must be positive.
    inline double WrapPeriod(double value, double period)
    {
        const double wrapped = value - period * std::floor(value / period);
        if (wrapped >= period || wrapped < 0.0)
        {
            return 0.0;
        }
        return wrapped;
    }

    /// @brief How far into its current cycle a CONSTANT rate of
    /// @p cyclesPerSecond is at @p time: the fractional part of
    /// `time * cyclesPerSecond`, computed in double and only then narrowed, in
    /// [0, 1).
    ///
    /// Bounded, so it keeps a float's full precision at any uptime, and 0
    /// exactly at a rate of 0 or a time of 0. Right only while the rate holds:
    /// a renderer whose rate is a config field, which a host or an animation
    /// may change, takes its phase from a @ref PhaseAccumulator instead - which
    /// returns exactly this until the rate first changes.
    inline float GetCyclePhase(double time, double cyclesPerSecond)
    {
        const float phase = static_cast<float>(WrapPeriod(time * cyclesPerSecond, 1.0));
        // Narrowing a value just under 1 can round up to 1.0f.
        return (phase >= 1.0f) ? 0.0f : phase;
    }

    /// @brief A phase driven by a rate that may CHANGE while it runs: the
    /// integral of the rate over the clock's time, wrapped into a period.
    ///
    /// `rate * time` is that integral only while the rate holds. Under a
    /// moving rate its slope is `rate + time * d(rate)/dt`, so the phase raced
    /// by an amount that grows with the uptime: the HueRotationEaseReverse
    /// preset (a triangle between -0.8 and 0.8 rev/s) moved the hue a mean
    /// 0.12 turn a frame started 10 s into the clock and 0.44 started 60 s in,
    /// against the 0.013 its rate allows - noise - and a rate set from a slider
    /// jumped the phase by `time * change`.
    ///
    /// So the phase is kept as an anchor - a phase at a time - and advanced
    /// from it at the current rate; a new rate re-anchors at the time it is
    /// first seen, with the phase the old rate had reached, so the phase stays
    /// continuous and moves at the rate it is given. Until the rate first
    /// changes the anchor is (0, 0), and the phase is `rate * time` wrapped,
    /// bit for bit what @ref GetCyclePhase / @ref WrapPeriod return - a
    /// constant rate, the default and every check scene, renders exactly as
    /// before at every time.
    ///
    /// Asking twice at one time with one rate returns one phase, so a renderer
    /// advances it from its Update - every frame, whatever it draws, so a rate
    /// that moved while the layer was hidden is integrated as it moved - and
    /// reads it again in its draw, for a host that renders without updating.
    /// A paused clock holds the phase; a clock set back (Clock::SetTime,
    /// Stop, Reset) runs it back at the current rate. Two accumulators fed
    /// the same times and rates hold the same phase, which is how the debug
    /// strip keeps the neon's hue.
    ///
    /// A non-finite rate counts as 0 and a non-finite time moves nothing, so
    /// neither can leave a NaN in the anchor for good.
    class PhaseAccumulator
    {
    public:
        /// The phase at @p time, in [0, @p period), for @p rate units per
        /// second. @p period must be positive and the same on every call.
        double Advance(double time, double rate, double period)
        {
            rate = std::isfinite(rate) ? rate : 0.0;
            if (!std::isfinite(time))
            {
                return mAnchorPhase;
            }
            if (!mStarted)
            {
                mStarted = true;
                mRate = rate;
            }
            else if (rate != mRate)
            {
                mAnchorPhase = WrapPeriod(mAnchorPhase + (time - mAnchorTime) * mRate, period);
                mAnchorTime = time;
                mRate = rate;
            }
            const double phase = WrapPeriod(mAnchorPhase + (time - mAnchorTime) * mRate, period);
            if (!std::isfinite(phase))
            {
                // An overflowing product: start again from this time.
                mAnchorPhase = 0.0;
                mAnchorTime = time;
                return 0.0;
            }
            return phase;
        }

        /// @ref Advance over one cycle, narrowed: the [0, 1) phase of a rate
        /// in cycles per second, as @ref GetCyclePhase narrows it.
        float AdvanceCyclePhase(double time, double cyclesPerSecond)
        {
            const float phase = static_cast<float>(Advance(time, cyclesPerSecond, 1.0));
            // Narrowing a value just under 1 can round up to 1.0f.
            return (phase >= 1.0f) ? 0.0f : phase;
        }

    private:
        bool mStarted = false;     ///< A rate has been seen; until then there is nothing to re-anchor from.
        double mRate = 0.0;        ///< The rate the phase is advancing at since mAnchorTime.
        double mAnchorTime = 0.0;  ///< The clock time of the anchor.
        double mAnchorPhase = 0.0; ///< The phase at mAnchorTime, in [0, period).
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CLOCK_H_
