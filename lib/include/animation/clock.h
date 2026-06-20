#ifndef _EDGE_LIGHTING_CLOCK_H_
#define _EDGE_LIGHTING_CLOCK_H_

namespace EdgeLighting
{
    /// @brief Play/pause time accumulator.
    ///
    /// Drives all @ref Modulator evaluations — `clock.GetTime()` is the
    /// canonical input to any animation in the system.
    ///
    /// Pausing freezes time accumulation, so any modulator sampled with the
    /// clock's time will hold its last value until play resumes.
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
            mTime = 0.0f;
        }

        /// @brief Reset accumulated time to 0 without changing play state.
        void Reset() { mTime = 0.0f; }

        /// @brief Advance the clock by @p deltaTime.
        /// @param deltaTime Frame delta in seconds.
        /// @note No-op when paused.
        void Update(float deltaTime)
        {
            if (mIsPlaying)
            {
                mTime += deltaTime;
            }
        }

        /// @brief Jump to an explicit time value.
        /// @param time New clock time in seconds (useful for scrubbing/testing).
        void SetTime(float time) { mTime = time; }

        /// @brief Current accumulated time in seconds.
        float GetTime() const { return mTime; }

        /// @brief Whether the clock is currently accumulating time.
        bool IsPlaying() const { return mIsPlaying; }

    private:
        float mTime = 0.0f;
        bool mIsPlaying = true;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CLOCK_H_
