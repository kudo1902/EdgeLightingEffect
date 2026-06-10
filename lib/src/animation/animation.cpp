#include "animation/animation.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{

    Animation::Animation() : isPlaying_(true), speed_(1.0f), progress_(0.0f) {}

    void Animation::Play()
    {
        isPlaying_ = true;
    }

    void Animation::Pause()
    {
        isPlaying_ = false;
    }

    void Animation::Stop()
    {
        isPlaying_ = false;
        progress_ = 0.0f;
        if (OnProgress)
        {
            OnProgress(progress_);
        }
    }

    void Animation::Update(float deltaTime)
    {
        if (!isPlaying_)
            return;

        progress_ += speed_ * deltaTime;
        if (progress_ >= 1.0f)
        {
            progress_ = glm::fract(progress_);
            if (OnLoopCompleted)
            {
                OnLoopCompleted();
            }
        }

        if (OnProgress)
        {
            OnProgress(progress_);
        }
    }

    void Animation::SetSpeed(float speed)
    {
        speed_ = speed;
    }

    float Animation::GetSpeed() const
    {
        return speed_;
    }

    void Animation::SetProgress(float progress)
    {
        progress_ = glm::clamp(progress, 0.0f, 1.0f);
        if (OnProgress)
        {
            OnProgress(progress_);
        }
    }

    float Animation::GetProgress() const
    {
        return progress_;
    }

    bool Animation::IsPlaying() const
    {
        return isPlaying_;
    }

} // namespace EdgeLighting
