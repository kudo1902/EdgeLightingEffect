#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/neon-renderer.h"
#include "renderer/droplets-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "renderer/spotlight-renderer.h"
#include "renderer/debug-renderer.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <utility> // std::swap - refreshActiveConfig swaps the composite scratch
#include <algorithm> // std::find / find_if - the renderer- and layer-order checks

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

    const std::vector<std::shared_ptr<BaseRenderer>> &EdgeLightingEffect::GetRenderers() const
    {
        return mRenderers;
    }

    bool EdgeLightingEffect::SetRendererOrder(const std::vector<std::shared_ptr<BaseRenderer>> &order)
    {
        // A permutation check by identity. Quadratic, but over a handful of
        // layers, and only when a host asks for a new order - never per frame.
        // Equal sizes plus "every entry is registered and appears once" is
        // enough: n distinct members of an n-element set are all of it.
        if (order.size() != mRenderers.size())
        {
            LOG_E("EdgeLightingEffect: SetRendererOrder given %zu renderers, %zu registered - "
                  "order unchanged.",
                  order.size(), mRenderers.size());
            return false;
        }
        for (size_t i = 0; i < order.size(); i++)
        {
            const bool registered =
                std::find(mRenderers.begin(), mRenderers.end(), order[i]) != mRenderers.end();
            const bool repeated =
                std::find(order.begin(), order.begin() + i, order[i]) != order.begin() + i;
            if (!registered || repeated)
            {
                LOG_E("EdgeLightingEffect: SetRendererOrder entry %zu is %s - order unchanged.",
                      i, registered ? "a duplicate" : "not a registered renderer");
                return false;
            }
        }
        mRenderers = order;
        return true;
    }

    std::shared_ptr<BaseRenderer> EdgeLightingEffect::CreateRenderer(RendererLayer layer)
    {
        switch (layer)
        {
        case RendererLayer::NEON:
        {
            return std::make_shared<NeonRenderer>();
        }
        case RendererLayer::DROPLETS:
        {
            return std::make_shared<DropletsRenderer>();
        }
        case RendererLayer::LENS_FLARE:
        {
            return std::make_shared<LensFlareRenderer>();
        }
        case RendererLayer::SPOTLIGHT:
        {
            return std::make_shared<SpotlightRenderer>();
        }
        case RendererLayer::DEBUG:
        {
            return std::make_shared<DebugRenderer>();
        }
        case RendererLayer::CUSTOM:
        default:
        {
            return nullptr;
        }
        }
    }

    const std::vector<RendererLayer> &EdgeLightingEffect::GetDefaultLayerOrder()
    {
        // The order the demo and the C ABI's mask-based init have always
        // registered in. Also the list of every named layer: IsValidLayerList
        // reads "known" off it, so a new layer is added here and in
        // CreateRenderer - nowhere else.
        static const std::vector<RendererLayer> ORDER = {
            RendererLayer::NEON,
            RendererLayer::DROPLETS,
            RendererLayer::LENS_FLARE,
            RendererLayer::SPOTLIGHT,
            RendererLayer::DEBUG,
        };
        return ORDER;
    }

    bool EdgeLightingEffect::IsValidLayerList(const std::vector<RendererLayer> &order)
    {
        const std::vector<RendererLayer> &known = GetDefaultLayerOrder();
        for (size_t i = 0; i < order.size(); i++)
        {
            if (std::find(known.begin(), known.end(), order[i]) == known.end())
            {
                LOG_E("EdgeLightingEffect: layer entry %zu (0x%x) names no layer.",
                      i, static_cast<unsigned>(order[i]));
                return false;
            }
            if (std::find(order.begin(), order.begin() + i, order[i]) != order.begin() + i)
            {
                LOG_E("EdgeLightingEffect: layer entry %zu (0x%x) is repeated.",
                      i, static_cast<unsigned>(order[i]));
                return false;
            }
        }
        return true;
    }

    bool EdgeLightingEffect::AddRenderer(RendererLayer layer)
    {
        // One renderer per named layer, or the layer calls could not tell
        // which of two to move.
        for (const auto &renderer : mRenderers)
        {
            if (renderer->GetLayer() == layer && layer != RendererLayer::CUSTOM)
            {
                LOG_E("EdgeLightingEffect: layer 0x%x is already registered - not added.",
                      static_cast<unsigned>(layer));
                return false;
            }
        }
        std::shared_ptr<BaseRenderer> renderer = CreateRenderer(layer);
        if (!renderer)
        {
            LOG_E("EdgeLightingEffect: 0x%x names no renderer - not added.",
                  static_cast<unsigned>(layer));
            return false;
        }
        const size_t before = mRenderers.size();
        AddRenderer(renderer);
        // AddRenderer(ptr) drops a late registration that fails to initialise.
        return mRenderers.size() > before;
    }

    std::vector<RendererLayer> EdgeLightingEffect::GetLayerOrder() const
    {
        std::vector<RendererLayer> order;
        order.reserve(mRenderers.size());
        for (const auto &renderer : mRenderers)
        {
            if (renderer->GetLayer() != RendererLayer::CUSTOM)
            {
                order.push_back(renderer->GetLayer());
            }
        }
        return order;
    }

    bool EdgeLightingEffect::SetLayerOrder(const std::vector<RendererLayer> &order)
    {
        if (!IsValidLayerList(order))
        {
            return false;
        }
        const std::vector<RendererLayer> current = GetLayerOrder();
        if (order.size() != current.size())
        {
            LOG_E("EdgeLightingEffect: SetLayerOrder given %zu layers, %zu registered - "
                  "order unchanged.",
                  order.size(), current.size());
            return false;
        }

        // Refill the named slots in the new order; custom renderers keep
        // theirs. Both lists are duplicate-free and the same size, so finding
        // every entry of `order` makes it a permutation of `current`.
        std::vector<std::shared_ptr<BaseRenderer>> renderers = mRenderers;
        size_t next = 0;
        for (auto &slot : renderers)
        {
            if (slot->GetLayer() == RendererLayer::CUSTOM)
            {
                continue;
            }
            const RendererLayer want = order[next++];
            auto it = std::find_if(mRenderers.begin(), mRenderers.end(),
                                   [want](const std::shared_ptr<BaseRenderer> &renderer) {
                                       return renderer->GetLayer() == want;
                                   });
            if (it == mRenderers.end())
            {
                LOG_E("EdgeLightingEffect: layer 0x%x is not registered - order unchanged.",
                      static_cast<unsigned>(want));
                return false;
            }
            slot = *it;
        }
        return SetRendererOrder(renderers);
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
            // Copy-ASSIGN into the member scratch, never a local copy-construct
            // - the assignment reuses the vector capacity already in there, so
            // this whole path allocates nothing once it has run a frame. See
            // mScratchConfig for the measurements behind that.
            mScratchConfig = mBaseConfig;
            mAnimationManager->Apply(mScratchConfig);
            if (mScratchConfig == mActiveConfig)
            {
                return;
            }
            // SWAP, not move-assign. A move would leave the scratch holding
            // moved-from (empty) vectors, and the next frame's assignment would
            // then have to allocate all of them again - which is the cost this
            // is here to remove. The swap hands the scratch the buffers the
            // outgoing active config owned: already allocated, already the
            // right size for the config it is about to be handed again.
            std::swap(mActiveConfig, mScratchConfig);
        }

        for (auto &renderer : mRenderers)
        {
            renderer->OnConfigChanged(mActiveConfig);
        }
    }

} // namespace EdgeLighting
