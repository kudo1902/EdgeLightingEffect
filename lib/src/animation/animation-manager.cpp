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
        // The walk must be over a container NOTHING a callback can reach, which
        // rules out two things rather than one:
        //   - not mAnimations, because a callback can detach or attach; and
        //   - not a shared member either, because a callback can re-enter
        //     Update, and the nested tick assigning to that member would
        //     reallocate the buffer this loop is iterating. A member scratch
        //     fixed the first hazard and introduced the second.
        // So: swap the member into a LOCAL for the duration of the tick. At
        // depth 0 the local arrives carrying the capacity the previous tick
        // left, so the copy still allocates nothing in steady state. A nested
        // tick finds the member empty and allocates its own - the right trade
        // for a path that should be rare.
        std::vector<AnimationPtr> ticking;
        ticking.swap(mTickScratch);
        ticking = mAnimations;
        for (const AnimationPtr &a : ticking)
        {
            a->Update(dt);
        }
        // Cleared before the capacity goes back, so an animation detached during
        // the loop is not kept alive by the manager afterwards.
        ticking.clear();
        ticking.swap(mTickScratch);
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
