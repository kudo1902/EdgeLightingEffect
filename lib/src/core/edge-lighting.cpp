#include "core/edge-lighting.h"

namespace EdgeLighting
{

    EdgeLightingEffect::EdgeLightingEffect()
        : mTime(0.0f)
    {
        mAnimation.SetSpeed(mConfig.speed);
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

        float progress = mAnimation.GetProgress();
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, progress, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float progress = mAnimation.GetProgress();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, progress, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        mConfig = config;
        mAnimation.SetSpeed(mConfig.speed);
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

} // namespace EdgeLighting
