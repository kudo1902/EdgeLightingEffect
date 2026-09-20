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
    ///   Before Play - field == base value (never-played animations no-op in Apply).
    ///   During Play - field == modulator output.
    ///   After Stop  - field settles at whatever the animation's
    ///                 @ref Animation::EndAction specifies (HOLD_CURRENT by
    ///                 default: the value the modulator was writing at the
    ///                 moment of stop). HOLD_END / HOLD_START / RESTORE pick
    ///                 different resting values. To let the base config show
    ///                 through after Stop, @ref Detach the animation - there's
    ///                 no dedicated "revert" mode because Detach already gives
    ///                 that behaviour.
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
        ///
        /// @note REENTRANT. @ref Animation::Update fires @c OnComplete and
        ///       @c OnStateChanged, and those callbacks may call @ref Attach,
        ///       @ref Detach or @ref DetachAll on this manager - which is the
        ///       whole point of @c OnComplete's "chain B after A" use. The
        ///       list is snapshotted for the tick, so:
        ///         - a detach during the tick still lets the already-ticked
        ///           animations finish this call, and takes effect from the
        ///           next one;
        ///         - an attach during the tick is NOT ticked this call. It
        ///           starts on the next, which is also what it would get had
        ///           the host attached it a moment later.
        ///       See the definition for what went wrong without the snapshot.
        void Update(float dt);

        /// @brief Apply every attached animation onto @p target in attach order
        ///        (forwards to each @ref Animation::Apply). Stopped animations
        ///        no-op, leaving @p target's field at its incoming (base) value.
        void Apply(Config &target) const;

    private:
        std::vector<AnimationPtr> mAnimations;
        /// The list @ref Update is currently ticking, snapshotted from
        /// @c mAnimations so a callback may mutate that one mid-tick. Held as
        /// a member rather than built as a local so the per-frame path does no
        /// heap allocation after warmup - the same trade
        /// @c EdgeLightingEffect::mScratchConfig and
        /// @c NeonRenderer::mEffectiveSegments make.
        ///
        /// Cleared at the end of each tick: the snapshot holds a strong
        /// reference to every animation in it, and keeping those alive between
        /// frames would make a detached animation outlive its detach.
        std::vector<AnimationPtr> mTickList;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_MANAGER_H_
