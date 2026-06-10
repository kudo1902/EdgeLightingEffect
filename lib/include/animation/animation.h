#ifndef EDGE_LIGHTING_ANIMATION_H
#define EDGE_LIGHTING_ANIMATION_H

#include <functional>

namespace EdgeLighting
{
    class Animation
    {
    public:
        Animation();
        ~Animation() = default;

        // Public API (PascalCase)
        void Play();
        void Pause();
        void Stop();
        void Update(float deltaTime);

        void SetSpeed(float speed);
        float GetSpeed() const;

        void SetProgress(float progress);
        float GetProgress() const;

        bool IsPlaying() const;

        // Event callbacks (prefixed with 'On')
        std::function<void(float progress)> OnProgress;
        std::function<void()> OnLoopCompleted;

    private:
        bool isPlaying_ = true;
        float speed_ = 1.0f;    // Loops per second
        float progress_ = 0.0f; // 0.0 to 1.0
    };

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_ANIMATION_H
