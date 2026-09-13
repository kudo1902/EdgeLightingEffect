#ifndef _EDGE_LIGHTING_CLOCK_H_
#define _EDGE_LIGHTING_CLOCK_H_

namespace EdgeLighting
{
    /// @brief Play/pause time accumulator.
    ///
    /// Owned by @ref EdgeLightingEffect. Each frame, @ref Update returns the
    /// delta that gets forwarded to every attached animation - a paused clock
    /// returns 0, so all animations freeze in lockstep without needing per-
    /// animation state changes.
    ///
    /// The absolute @ref GetTime is still available for renderers whose shaders
    /// use it directly (e.g. the neon hue-rotation animation reads @c uTime
    /// and multiplies by @c hueRotationRate).
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
        /// DOUBLE, like @c EdgeLightingEffect::mRawAccumulatedTime and for the
        /// same reason: as a float this stopped advancing after about 58 hours
        /// at 120 Hz, because the ULP had grown past the per-frame increment.
        /// Renderers receive it at full precision and each reduces it for its
        /// own shader - see @c TimeUtils::WrapHueTime and review-findings I33b.
        double mTime = 0.0;
        bool mIsPlaying = true;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CLOCK_H_
