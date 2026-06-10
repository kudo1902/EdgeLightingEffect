#include "core/edge-lighting.h"

namespace EdgeLighting
{

    EdgeLightingEffect::EdgeLightingEffect() : time_(0.0f)
    {
        animation_.SetSpeed(config_.speed);
    }

    bool EdgeLightingEffect::Initialize()
    {
        bool success = true;
        for (auto &renderer : renderers_)
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
        animation_.Update(deltaTime);
        if (animation_.IsPlaying())
        {
            time_ += deltaTime;
        }

        float progress = animation_.GetProgress();
        for (auto &renderer : renderers_)
        {
            renderer->Update(deltaTime, progress, time_, config_);
        }
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        float progress = animation_.GetProgress();
        for (auto &renderer : renderers_)
        {
            renderer->Render(viewportWidth, viewportHeight, progress, time_, config_);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        config_ = config;
        animation_.SetSpeed(config_.speed);
        for (auto &renderer : renderers_)
        {
            renderer->OnConfigChanged(config_);
        }
    }

    const Config &EdgeLightingEffect::GetConfig() const
    {
        return config_;
    }

    void EdgeLightingEffect::AddRenderer(std::shared_ptr<BaseRenderer> renderer)
    {
        if (renderer)
        {
            renderers_.push_back(renderer);
            renderer->OnConfigChanged(config_);
        }
    }

    Animation &EdgeLightingEffect::GetAnimation()
    {
        return animation_;
    }

} // namespace EdgeLighting
