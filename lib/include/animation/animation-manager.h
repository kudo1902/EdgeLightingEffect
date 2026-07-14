#ifndef _EDGE_LIGHTING_ANIMATION_MANAGER_H_
#define _EDGE_LIGHTING_ANIMATION_MANAGER_H_

#include "animation/animation.h"
#include "core/config.h"
#include <vector>

namespace EdgeLighting
{
    /// @brief The set of animations that overlay a @ref Config.
    ///
    /// A manager is a plain value type: it owns only the animation list, not a
    /// base config and not an effect. @ref EdgeLightingEffect embeds one and
    /// drives it each frame - the effect owns the base config, so per-frame the
    /// flow is:
    ///
    /// @code
    ///     manager.Update(clockDelta);   // tick every animation
    ///     Config active = base;         // start from the untouched base
    ///     manager.Apply(active);        // layer every animation on top
    ///     // ... hand `active` to the renderers
    /// @endcode
    ///
    /// The manager keeps no per-animation bookkeeping - each @ref Animation owns
    /// its own play state, elapsed accumulator, and completion/hold behaviour.
    ///
    /// ## Field lifetime (the three phases)
    ///
    ///   Before Play - field == base value (Stopped animations no-op in Apply).
    ///   During Play - field == modulator output.
    ///   After Stop  - field reverts to the base value (active is rebuilt from
    ///                 base each frame).
    ///
    /// The exception is a @c ONE_SHOT flagged with
    /// @ref Animation::SetHoldFinalValue(true): once it completes it stays
    /// parked at its end value and keeps applying it in @ref Apply, so the
    /// field settles at the target instead of snapping back. That behaviour
    /// lives entirely in @ref Animation; the manager just applies it.
    ///
    /// An @ref AnimationGroup is itself an @ref Animation, so a whole group can
    /// be @ref Attach ed as a single phase-locked unit.
    class AnimationManager
    {
    public:
        AnimationManager() = default;

        // --- Attach / detach ---------------------------------------------

        /// @brief Attach a standalone animation. Ignores null and duplicates.
        /// @details The animation keeps whatever play state it carries - call
        ///          @c anim->Play() to start it.
        void Attach(const AnimationPtr &animation);

        /// @brief Detach by shared_ptr identity.
        /// @return true if it was attached and removed.
        bool Detach(const AnimationPtr &animation);

        /// @brief Detach every animation.
        void DetachAll() { mAnimations.clear(); }

        /// @brief True if @p animation is currently attached.
        bool Contains(const AnimationPtr &animation) const;

        /// @brief Number of attached animations.
        size_t GetCount() const { return mAnimations.size(); }

        /// @brief Attached animation at @p index, in attach order.
        /// @note Bounds are the caller's responsibility (see @ref GetCount).
        const AnimationPtr &GetAnimation(size_t index) const
        {
            return mAnimations[index];
        }

        // --- Drive -------------------------------------------------------
        //
        // Mirror the two-phase Animation::Update / Animation::Apply split: one
        // advances time, the other writes the current values into a config.

        /// @brief Advance every attached animation by @p dt (forwards to each
        ///        @ref Animation::Update).
        /// @param dt Time to advance (typically the effect clock's delta, so a
        ///           paused clock freezes every animation).
        void Update(float dt);

        /// @brief Apply every attached animation onto @p target in attach order
        ///        (forwards to each @ref Animation::Apply). Stopped animations
        ///        no-op, leaving @p target's field at its incoming (base) value.
        void Apply(Config &target) const;

    private:
        std::vector<AnimationPtr> mAnimations;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_MANAGER_H_
