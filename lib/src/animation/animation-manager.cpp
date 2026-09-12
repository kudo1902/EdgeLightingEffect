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
        //
        // Walk a COPY, not mAnimations. Animation::Update fires OnComplete and
        // OnStateChanged synchronously, those are host callbacks, and the most
        // natural thing to write in one is "detach me now" - which used to
        // erase from the vector this loop was iterating. The result was half
        // the list silently skipped and then a segfault. See the re-entrancy
        // note on the declaration for the semantics this fixes them to.
        mTickScratch = mAnimations;
        for (const AnimationPtr &a : mTickScratch)
        {
            a->Update(dt);
        }
        // Dropped here rather than left holding references until the next tick:
        // an animation detached during the loop should not be kept alive by the
        // manager afterwards. clear() keeps the capacity for the next frame.
        mTickScratch.clear();
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
