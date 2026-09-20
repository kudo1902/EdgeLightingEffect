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
        // OVER A SNAPSHOT, not over mAnimations directly, because this tick can
        // re-enter the manager. Animation::Update fires OnComplete and
        // OnStateChanged - both reachable from the C ABI
        // (el_animation_set_on_complete_callback) - and the use OnComplete's
        // own documentation recommends, "chain B after A", is exactly a
        // callback that detaches A and attaches B. Either call mutates
        // mAnimations while a range-for holds an iterator into it.
        //
        // Measured under AddressSanitizer with three one-shots, the first two
        // detaching themselves from OnComplete: a container-overflow read at
        // the loop below on the tick that completed them. A detach alone stays
        // inside the vector's allocation; an attach that reallocates turns the
        // same read into a heap-use-after-free.
        //
        // The snapshot also fixes the LIFETIME half, which an index-based loop
        // would not: mAnimations may hold the last reference to an animation,
        // so detaching from inside its own callback would destroy it while its
        // Update is still on the stack. Copying the shared_ptrs keeps every
        // animation alive for the whole of its own tick.
        //
        // assign() reuses mTickList's capacity, so this allocates nothing after
        // the first call - see the member's declaration.
        mTickList.assign(mAnimations.begin(), mAnimations.end());
        for (const AnimationPtr &a : mTickList)
        {
            a->Update(dt);
        }
        // Do not hold the strong references past the tick; see the member.
        mTickList.clear();
    }

    void AnimationManager::Apply(Config &target) const
    {
        // Layer every animation on top of the incoming base in attach order.
        // Stopped animations no-op (Animation::Apply skips them), so their
        // field stays at the base value - except a hold-final-value one-shot
        // that has completed, which keeps writing its terminal value.
        //
        // No snapshot here, unlike Update, and the reason is that this half
        // cannot re-enter: Animation::Apply fires no callback and reaches no
        // state transition (Animation::transitionTo is called only from Play /
        // Pause / Stop / Update), so nothing it runs can touch mAnimations. It
        // routes through the ApplyAt hook, which is documented as pure - a
        // subclass that breaks that and mutates the manager from inside it
        // needs the snapshot moved here too.
        for (const AnimationPtr &a : mAnimations)
        {
            a->Apply(target);
        }
    }

} // namespace EdgeLighting
