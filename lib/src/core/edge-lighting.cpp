#include "core/edge-lighting.h"
#include "util/log-util.h"
#include "util/gl-utils.h"

namespace EdgeLighting
{

    EdgeLightingEffect::EdgeLightingEffect() = default;

    bool EdgeLightingEffect::Initialize()
    {
        GLUtils::LogExtensions();
        GLUtils::LogCaps();

        bool allOk = true;
        for (auto it = mRenderers.begin(); it != mRenderers.end();)
        {
            if (!(*it)->Initialize())
            {
                LOG_E("EdgeLightingEffect: a renderer failed to initialize — removing it.");
                it = mRenderers.erase(it);
                allOk = false;
            }
            else
            {
                ++it;
            }
        }
        return allOk;
    }

    void EdgeLightingEffect::Update(float deltaTime)
    {
        mClock.Update(deltaTime);
        float t = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, t, mConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float t = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, t, mConfig);
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

    Clock &EdgeLightingEffect::GetClock() { return mClock; }
    const Clock &EdgeLightingEffect::GetClock() const { return mClock; }

} // namespace EdgeLighting
