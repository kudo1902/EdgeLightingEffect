#include "core/edge-lighting.h"
#include "util/log-util.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{

    EdgeLightingEffect::EdgeLightingEffect()
        : mTime(0.0f)
    {
    }

    bool EdgeLightingEffect::Initialize()
    {
        bool success = true;
        for (auto &renderer : mRenderers)
        {
            if (!renderer->Initialize())
            {
                success = false;
            }
        }
        return success;
    }

    void EdgeLightingEffect::Update(float deltaTime)
    {
        mAnimation.Update(deltaTime);
        if (mAnimation.IsPlaying())
        {
            mTime += deltaTime;
        }

        float rawProgress = mAnimation.GetProgress();
        float headPos = computeHeadPos();
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, rawProgress, headPos, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float rawProgress = mAnimation.GetProgress();
        float headPos = computeHeadPos();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, rawProgress, headPos, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        mConfig = config;

        mAnimation.SetSpeed(mConfig.stroke.speed);

        for (auto &renderer : mRenderers)
        {
            renderer->OnConfigChanged(mConfig);
        }
    }

    const Config &EdgeLightingEffect::GetConfig() const
    {
        return mConfig;
    }

    void EdgeLightingEffect::AddRenderer(std::shared_ptr<BaseRenderer> renderer)
    {
        if (renderer)
        {
            mRenderers.push_back(renderer);
            renderer->OnConfigChanged(mConfig);
        }
    }

    Animation &EdgeLightingEffect::GetAnimation()
    {
        return mAnimation;
    }

    float EdgeLightingEffect::computeHeadPos() const
    {
        float rawProgress;
        if (mConfig.path.startPos == mConfig.path.endPos)
        {
            rawProgress = glm::fract(mTime * mConfig.stroke.speed);
        }
        else
        {
            rawProgress = mAnimation.GetProgress();
        }
        return glm::mix(mConfig.path.startPos, mConfig.path.endPos, rawProgress);
    }

} // namespace EdgeLighting
