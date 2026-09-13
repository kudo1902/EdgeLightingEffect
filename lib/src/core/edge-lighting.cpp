#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <algorithm> // std::max - Render clamps the cross-fade delta
#include <utility>   // std::swap - refreshActiveConfig swaps the composite scratch

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

        // RAW delta, deliberately not routed through the clock: this is what
        // the render side differences for GradientRingLUT::Tick, and a colour
        // change has to keep fading whatever the clock is doing - paused,
        // stopped, reset or scrubbed.
        mRawAccumulatedTime += static_cast<double>(deltaTime);

        // Unconditional, unlike the publish in SetConfig - the two times move
        // every frame even when the config does not, and they are half of what
        // the render side consumes.
        publishSnapshot();
    }

    void EdgeLightingEffect::Render(int viewportWidth, int viewportHeight)
    {
        const bool fresh = mSnapshots.AcquireLatest();
        const ConfigSnapshot &snapshot = mSnapshots.Current();

        // Gated on the generation, not on `fresh`: a snapshot can arrive with
        // the config unmoved (only time advanced), and conversely a re-entered
        // Render on the SAME snapshot must not re-notify.
        if (snapshot.configGeneration != mNotifiedGeneration)
        {
            for (auto &renderer : mRenderers)
            {
                renderer->OnConfigChanged(snapshot.config);
            }
            mNotifiedGeneration = snapshot.configGeneration;
        }

        // Exactly once per PUBLISHED frame, which is what `fresh` buys. The
        // renderer tick is the cross-fade advance, and it is not idempotent:
        // GradientRingLUT::Tick re-uploads the ring and reports true whenever a
        // fade is in flight, even for a zero delta, which would re-bake the
        // neon emission table for nothing. The demo renders twice in one frame
        // on the capture path, so this is a live case, not a hypothetical.
        if (fresh)
        {
            // Clamped because a host is free to hand Update a negative delta;
            // the fade must not run backwards.
            // Differenced in double, handed on as float: the DIFFERENCE is a
            // frame delta and tiny, so it loses nothing on the way out. It is
            // the running total that needed the width.
            // Differenced in double, handed on as float: the DIFFERENCE is a
            // frame delta and tiny, so it loses nothing on the way out. It is
            // the running total that needed the width.
            const float fadeDelta = static_cast<float>(
                std::max(0.0, snapshot.rawAccumulatedTime - mRenderedRawAccumulatedTime));
            mRenderedRawAccumulatedTime = snapshot.rawAccumulatedTime;
            for (auto &renderer : mRenderers)
            {
                renderer->Update(fadeDelta, snapshot.clockTime, snapshot.config);
            }
        }

        for (auto &renderer : mRenderers)
        {
            renderer->Render(viewportWidth, viewportHeight, snapshot.clockTime, snapshot.config);
        }
    }

    void EdgeLightingEffect::SetConfig(const Config &config)
    {
        if (config == mBaseConfig)
        {
            return;
        }
        mBaseConfig = config;
        // Only when the COMPOSITE moved. A base change an animation fully
        // overrides leaves the active config - and therefore the snapshot -
        // identical to what was last published, so there is nothing to send.
        if (refreshActiveConfig())
        {
            publishSnapshot();
        }
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
        // The held SNAPSHOT, not mActiveConfig. Both carry a composited config,
        // which is what a renderer joining mid-animation needs, but only one of
        // them is this side's to read: mActiveConfig belongs to the data side,
        // which mutates it by swapping its vectors out from under any reader
        // (refreshActiveConfig). AddRenderer is a render-side call - it runs
        // Initialize, which is GL - so reaching across was a data race for any
        // C++ host following the split this class documents. The C ABI never
        // saw it, because el_effect_init_with_renderers holds the handle lock
        // across the whole thing. See review-findings I31.
        //
        // No AcquireLatest here, deliberately: the new renderer joins with
        // exactly what its peers are currently drawing rather than with
        // something newer they have not seen. If a newer snapshot is already
        // published, the next Render acquires it and notifies EVERY renderer,
        // this one included - which is also why mNotifiedGeneration must not be
        // touched here. Advancing it would make the others miss that
        // generation.
        //
        // Renderers gate their own rebuilds on shader validity, so this is also
        // safe on the pre-Initialize path where nothing is compiled yet.
        renderer->OnConfigChanged(mSnapshots.Current().config);
    }

    Clock &EdgeLightingEffect::GetClock() { return mClock; }
    const Clock &EdgeLightingEffect::GetClock() const { return mClock; }

    bool EdgeLightingEffect::refreshActiveConfig()
    {
        if (mAnimationManager->GetCount() == 0)
        {
            if (mActiveConfig == mBaseConfig)
            {
                return false;
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
                return false;
            }
            // SWAP, not move-assign. A move would leave the scratch holding
            // moved-from (empty) vectors, and the next frame's assignment would
            // then have to allocate all of them again - which is the cost this
            // is here to remove. The swap hands the scratch the buffers the
            // outgoing active config owned: already allocated, already the
            // right size for the config it is about to be handed again.
            std::swap(mActiveConfig, mScratchConfig);
        }

        // Reached only when the composite really moved, which is the whole
        // value of this counter: the render side gets to skip OnConfigChanged
        // on an unchanged frame without repeating the deep compare that just
        // happened above.
        ++mConfigGeneration;
        return true;
    }

    void EdgeLightingEffect::publishSnapshot()
    {
        ConfigSnapshot &slot = mSnapshots.BeginWrite();
        // Copy-ASSIGN into a slot that already owns the right-sized buffers,
        // for the reason spelled out on mScratchConfig: this is a per-frame
        // path, and a copy-construct here would allocate every vector the
        // config owns, every frame, forever.
        slot.config = mActiveConfig;
        slot.clockTime = mClock.GetTime();
        slot.rawAccumulatedTime = mRawAccumulatedTime;
        slot.configGeneration = mConfigGeneration;
        mSnapshots.Publish();
    }

} // namespace EdgeLighting
