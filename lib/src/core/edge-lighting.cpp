#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <utility> // std::swap - refreshActiveConfig swaps the composite scratch

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
        // The one piece of GL state this fan-out owns, and it is here rather
        // than in the renderers because it is a property of the whole effect:
        // NOTHING this library draws is meant to be face-culled. Every layer is
        // a flat screen-space quad, ring or strip with no back side to hide, so
        // a cull state can only ever delete pixels that were meant to be there.
        //
        // It is not hypothetical. The GL context is not always this library's
        // alone - on an embedded surface view (Tizen Evas_GL, Android
        // GLSurfaceView) a video pipeline or web engine shares it and leaves
        // its own state behind, and none of it shows up on a desktop demo where
        // GL_CULL_FACE is off by default and nothing ever enables it. Measured
        // offscreen at 640x360, with GL_CULL_FACE on and the host's winding
        // order reversed to GL_CW, every layer but the debug bounding box (a
        // GL_LINE_LOOP, which culling does not apply to) rendered 0 pixels.
        //
        // ONE scope for the whole frame rather than one per renderer: it is a
        // single glIsEnabled either way, the renderers all want the same
        // answer, and a layer added later gets the guarantee without having to
        // know it needed one. Renderers still own their own BLEND state, which
        // differs per layer and so cannot be hoisted the same way.
        //
        // The host's setting is put back as this scope unwinds - a caller that
        // had culling on for its own geometry finds it on again afterwards.
        GLUtils::NoCullScope noCull;

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
