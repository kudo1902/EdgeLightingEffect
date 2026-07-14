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
                LOG_E("EdgeLightingEffect: a renderer failed to initialize - removing it.");
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
        // The clock reports how far it advanced (0 while paused) and the
        // animations advance in lockstep, so pausing the clock freezes them.
        mAnimationManager.Update(mClock.Update(deltaTime));
        refreshActiveConfig();

        float time = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, time, mActiveConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float time = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, time, mActiveConfig);
        }
    }

    // Config: SetConfig sets the authored base; GetActiveConfig is that base
    // with every attached animation overlaid (what the renderers draw).
    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        mBaseConfig = config;
        refreshActiveConfig();
    }

    const Config &EdgeLightingEffect::GetConfig() const { return mBaseConfig; }
    const Config &EdgeLightingEffect::GetActiveConfig() const { return mActiveConfig; }

    // Animations: thin forwarders to the embedded manager.
    void EdgeLightingEffect::Attach(const AnimationPtr &a) { mAnimationManager.Attach(a); }
    bool EdgeLightingEffect::Detach(const AnimationPtr &a) { return mAnimationManager.Detach(a); }
    AnimationManager &EdgeLightingEffect::Animations() { return mAnimationManager; }
    const AnimationManager &EdgeLightingEffect::Animations() const { return mAnimationManager; }

    void EdgeLightingEffect::AddRenderer(std::shared_ptr<BaseRenderer> renderer)
    {
        if (renderer)
        {
            mRenderers.push_back(renderer);
            renderer->OnConfigChanged(mActiveConfig);
        }
    }

    Clock &EdgeLightingEffect::GetClock() { return mClock; }
    const Clock &EdgeLightingEffect::GetClock() const { return mClock; }

    // Rebuild the active config = base + every animation overlay, notifying
    // renderers only when it actually changed so idle frames don't rebuild
    // derived GPU state.
    void EdgeLightingEffect::refreshActiveConfig()
    {
        Config active = mBaseConfig;
        mAnimationManager.Apply(active);
        if (active == mActiveConfig)
        {
            return;
        }
        mActiveConfig = active;
        for (auto &renderer : mRenderers)
        {
            renderer->OnConfigChanged(mActiveConfig);
        }
    }

} // namespace EdgeLighting
