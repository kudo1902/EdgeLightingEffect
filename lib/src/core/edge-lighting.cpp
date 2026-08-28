#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "util/log-util.h"
#include "util/gl-utils.h"

namespace EdgeLighting
{

    EdgeLightingEffect::EdgeLightingEffect()
        : mAnimationManager(std::make_unique<AnimationManager>())
    {
    }

    EdgeLightingEffect::~EdgeLightingEffect() = default;

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
        // From here on AddRenderer has to initialise what it is handed - this
        // loop is not coming round again for it.
        mInitialized = true;
        return allOk;
    }

    void EdgeLightingEffect::Update(float deltaTime)
    {
        mAnimationManager->Update(mClock.Update(deltaTime));
        refreshActiveConfig();

        float time = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Update(deltaTime, time, mActiveConfig);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float t = mClock.GetTime();
        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, t, mActiveConfig);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        if (config == mBaseConfig)
        {
            return;
        }
        mBaseConfig = config;
        refreshActiveConfig();
    }

    const Config &EdgeLightingEffect::GetConfig() const { return mBaseConfig; }
    const Config &EdgeLightingEffect::GetActiveConfig() const { return mActiveConfig; }

    void EdgeLightingEffect::Attach(const AnimationPtr &a) { mAnimationManager->Attach(a); }
    bool EdgeLightingEffect::Detach(const AnimationPtr &a) { return mAnimationManager->Detach(a); }
    AnimationManager &EdgeLightingEffect::GetAnimationManager() { return *mAnimationManager; }
    const AnimationManager &EdgeLightingEffect::GetAnimationManager() const { return *mAnimationManager; }

    void EdgeLightingEffect::AddRenderer(std::shared_ptr<BaseRenderer> renderer)
    {
        if (!renderer)
        {
            return;
        }

        // Registering after Initialize used to leave the renderer with no
        // shaders: Initialize walks the list exactly once, so nothing would
        // ever compile them, and the renderer's Render then drew with program
        // 0 every frame. Initialise it here instead, and drop it if that fails
        // - the same contract Initialize applies to the batch.
        if (mInitialized && !renderer->Initialize())
        {
            LOG_E("EdgeLightingEffect: renderer registered after Initialize failed "
                  "to initialize - not added.");
            return;
        }

        mRenderers.push_back(renderer);
        // Hand over the current composited config, not the base: a renderer
        // joining mid-animation should see what every other renderer sees.
        // Renderers gate their own rebuilds on shader validity, so this is also
        // safe on the pre-Initialize path where nothing is compiled yet.
        renderer->OnConfigChanged(mActiveConfig);
    }

    Clock &EdgeLightingEffect::GetClock() { return mClock; }
    const Clock &EdgeLightingEffect::GetClock() const { return mClock; }

    void EdgeLightingEffect::refreshActiveConfig()
    {
        if (mAnimationManager->GetCount() == 0)
        {
            if (mActiveConfig == mBaseConfig)
            {
                return;
            }
            mActiveConfig = mBaseConfig;
        }
        else
        {
            Config active = mBaseConfig;
            mAnimationManager->Apply(active);
            if (active == mActiveConfig)
            {
                return;
            }
            mActiveConfig = std::move(active);
        }

        for (auto &renderer : mRenderers)
        {
            renderer->OnConfigChanged(mActiveConfig);
        }
    }

} // namespace EdgeLighting
