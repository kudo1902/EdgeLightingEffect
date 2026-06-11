#include "animation/animation.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{

    Animation::Animation()
        : mIsPlaying(true),
          mSpeed(1.0f),
          mProgress(0.0f)
    {
    }

    void Animation::Play()
    {
        mIsPlaying = true;
    }

    void Animation::Pause()
    {
        mIsPlaying = false;
    }

    void Animation::Stop()
    {
        mIsPlaying = false;
        mProgress = 0.0f;
        if (OnProgress)
        {
            OnProgress(mProgress);
        }
    }

    void Animation::Update(float deltaTime)
    {
        if (!mIsPlaying)
            return;

        mProgress += mSpeed * deltaTime;
        if (mProgress >= 1.0f)
        {
            mProgress = glm::fract(mProgress);
            if (OnLoopCompleted)
            {
                OnLoopCompleted();
            }
        }

        if (OnProgress)
        {
            OnProgress(mProgress);
        }
    }

    void Animation::SetSpeed(float speed)
    {
        mSpeed = speed;
    }

    float Animation::GetSpeed() const
    {
        return mSpeed;
    }

    void Animation::SetProgress(float progress)
    {
        mProgress = glm::clamp(progress, 0.0f, 1.0f);
        if (OnProgress)
        {
            OnProgress(mProgress);
        }
    }

    float Animation::GetProgress() const
    {
        return mProgress;
    }

    bool Animation::IsPlaying() const
    {
        return mIsPlaying;
    }

} // namespace EdgeLighting
