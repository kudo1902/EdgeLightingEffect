#include "core/edge-lighting.h"
#include "util/contour-tracer.h"
#include "util/log-util.h"
#include "util/stb-image.h"
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
        mAnimation.OnLoopCompleted = [this]()
        {
            if (!mConfig.path.closed)
            {
                mAnimation.Pause();
            }
        };

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
        if (mConfig.path.closed && mConfig.path.startPos == mConfig.path.endPos)
        {
            rawProgress = glm::fract(mTime * mConfig.stroke.speed);
        }
        else
        {
            rawProgress = mAnimation.GetProgress();
        }
        return glm::mix(mConfig.path.startPos, mConfig.path.endPos, rawProgress);
    }

    bool EdgeLightingEffect::SetMaskFromFile(const char *path)
    {
        int w = 0, h = 0, n = 0;
        unsigned char *pixels = stbi_load(path, &w, &h, &n, 4);
        if (!pixels)
        {
            LOG_E("Failed to load mask image '%s': %s", path, stbi_failure_reason());
            return false;
        }

        bool ok = SetMaskFromPixels(pixels, w, h);
        stbi_image_free(pixels);

        if (ok)
        {
            LOG_I("Mask loaded from '%s' (%dx%d, %d contour points)", path, w, h,
                  (int)mConfig.path.points.size());
        }
        return ok;
    }

    bool EdgeLightingEffect::SetMaskFromPixels(const uint8_t *pixels, int w, int h)
    {
        auto contour = TraceOutermostContour(pixels, w, h,
                                             mConfig.geometry.width,
                                             mConfig.geometry.height);

        if (contour.empty())
        {
            LOG_E("No foreground found in mask (%dx%d)", w, h);
            return false;
        }

        mConfig.path.points = std::move(contour);
        mConfig.path.source = PathSource::MASK;
        mConfig.path.closed = true;

        for (auto &r : mRenderers)
        {
            r->OnConfigChanged(mConfig);
        }

        return true;
    }

} // namespace EdgeLighting
