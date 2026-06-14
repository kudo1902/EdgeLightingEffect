#ifndef _EDGE_LIGHTING_ANIMATION_H_
#define _EDGE_LIGHTING_ANIMATION_H_

#include <functional>

namespace EdgeLighting
{
    /// Simple looping animation timer.
    ///
    /// Drives @ref mProgress from 0.0 to 1.0 at a configurable speed (in loops
    /// per second). Fires @ref OnProgress and @ref OnLoopCompleted callbacks.
    class Animation
    {
    public:
        Animation();
        ~Animation() = default;

        /// Starts or resumes the animation timer.
        void Play();

        /// Pauses the animation timer (freezes progress).
        void Pause();

        /// Stops the animation and resets progress to 0.
        void Stop();

        /// Advances the animation by @p deltaTime seconds.
        /// Fires @ref OnProgress and @ref OnLoopCompleted as needed.
        void Update(float deltaTime);

        /// Sets the animation speed in loops per second.
        void SetSpeed(float speed);

        /// Returns the current speed (loops per second).
        float GetSpeed() const;

        /// Jumps to a specific progress value in [0, 1].
        void SetProgress(float progress);

        /// Returns the current progress in [0, 1].
        float GetProgress() const;

        /// Returns true if the animation is currently playing.
        bool IsPlaying() const;

        /// Called every frame with the current progress.
        std::function<void(float progress)> OnProgress;

        /// Called when a full loop (0 → 1) completes.
        std::function<void()> OnLoopCompleted;

    private:
        bool mIsPlaying = true;
        float mSpeed = 1.0f;    ///< Loops per second
        float mProgress = 0.0f; ///< Current position in [0, 1]
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_H_
