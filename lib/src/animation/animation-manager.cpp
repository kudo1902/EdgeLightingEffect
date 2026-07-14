#include "animation/animation-manager.h"
#include <algorithm>

namespace EdgeLighting
{
    void AnimationManager::Attach(const AnimationPtr &animation)
    {
        if (!animation || Contains(animation))
        {
            return;
        }
        mAnimations.push_back(animation);
    }

    bool AnimationManager::Detach(const AnimationPtr &animation)
    {
        auto it = std::find(mAnimations.begin(), mAnimations.end(), animation);
        if (it == mAnimations.end())
        {
            return false;
        }
        mAnimations.erase(it);
        return true;
    }

    bool AnimationManager::Contains(const AnimationPtr &animation) const
    {
        return std::find(mAnimations.begin(), mAnimations.end(), animation) !=
               mAnimations.end();
    }

    void AnimationManager::Update(float dt)
    {
        // Each animation owns its own state / elapsed / completion + hold
        // behaviour, so advancing is just a per-animation tick.
        for (const AnimationPtr &a : mAnimations)
        {
            a->Update(dt);
        }
    }

    void AnimationManager::Apply(Config &target) const
    {
        // Layer every animation on top of the incoming base in attach order.
        // Stopped animations no-op (Animation::Apply skips them), so their
        // field stays at the base value - except a hold-final-value one-shot
        // that has completed, which keeps writing its terminal value.
        for (const AnimationPtr &a : mAnimations)
        {
            a->Apply(target);
        }
    }

} // namespace EdgeLighting
