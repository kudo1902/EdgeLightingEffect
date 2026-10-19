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
    /// CPU, in double, to a bounded phase (@ref GetCyclePhase,
    /// @ref WrapPeriod) - the neon's hue phase, the flare's ray rotation, the
    /// droplets' wrapped time - and uploads that.
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

    /// @brief How far into its current cycle a rate of @p cyclesPerSecond is
    /// at @p time: the fractional part of `time * cyclesPerSecond`, computed
    /// in double and only then narrowed, in [0, 1).
    ///
    /// What a shader is handed in place of `uTime * rate`: bounded, so it
    /// keeps a float's full precision at any uptime, and 0 exactly at a rate
    /// of 0 or a time of 0.
    inline float GetCyclePhase(double time, double cyclesPerSecond)
    {
        const float phase = static_cast<float>(WrapPeriod(time * cyclesPerSecond, 1.0));
        // Narrowing a value just under 1 can round up to 1.0f.
        return (phase >= 1.0f) ? 0.0f : phase;
    }

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CLOCK_H_
