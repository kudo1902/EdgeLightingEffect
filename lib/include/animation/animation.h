#ifndef _EDGE_LIGHTING_ANIMATION_H_
#define _EDGE_LIGHTING_ANIMATION_H_

#include <functional>

namespace EdgeLighting
{
    class Animation
    {
    public:
        Animation();
        ~Animation() = default;

        void Play();
        void Pause();
        void Stop();
        void Update(float deltaTime);

        void SetSpeed(float speed);
        float GetSpeed() const;

        void SetProgress(float progress);
        float GetProgress() const;

        bool IsPlaying() const;

        std::function<void(float progress)> OnProgress;
        std::function<void()> OnLoopCompleted;

    private:
        bool mIsPlaying = true;
        float mSpeed = 1.0f;    // Loops per second
        float mProgress = 0.0f; // 0.0 to 1.0
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_H_
