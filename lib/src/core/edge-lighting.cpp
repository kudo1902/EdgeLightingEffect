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
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, rawProgress, 0.0f, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float rawProgress = mAnimation.GetProgress();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, rawProgress, 0.0f, mTime, mConfig);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        mConfig = config;
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
