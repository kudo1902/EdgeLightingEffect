#ifndef _EDGE_LIGHTING_ANIMATION_H_
#define _EDGE_LIGHTING_ANIMATION_H_

#include "core/config.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <utility>

namespace EdgeLighting
{
    /// @defgroup animations Animations
    /// @brief High-level wrappers that map modulator output onto @ref Config fields.
    ///
    /// While @ref Modulator is the low-level primitive (a pure @c time→float),
    /// an @ref Animation owns its modulator(s) and writes the result into a
    /// target @ref Config field.  Groups combine multiple animations in sequence
    /// or in parallel.
    ///
    /// @{

    /// @brief How an animation behaves once it finishes one full cycle.
    ///
    /// Separated from the underlying modulator's cycle length so the two
    /// concepts can move independently:
    /// - @c Modulator (Oscillator / Ease / Sequence) defines *what one cycle
    ///   looks like* and *how long that cycle is*.
    /// - @c PlaybackMode defines *whether the animation keeps cycling forever
    ///   or stops after a given total play time*.
    ///
    /// E.g. a 2-second sine pulse can be:
    ///   - @c LOOP   : continuous breathing, never completes (default).
    ///   - @c ONE_SHOT with duration 6.0 s: plays 3 cycles, then OnComplete.
    typedef enum class PlaybackMode
    {
        LOOP,    ///< Plays forever; never completes; OnComplete never fires.
        ONE_SHOT ///< Plays for @ref Animation::GetDuration seconds, then completes.
    } PlaybackMode;

    /// @brief Base class for all animations.
    ///
    /// ## Lifecycle
    ///
    /// An animation is driven by an *elapsed* time supplied by the caller —
    /// the number of seconds since the animation began, NOT absolute clock
    /// time. The driver (e.g. the demo UI) snapshots `clock.GetTime()` when
    /// an animation is selected and subtracts that snapshot when calling
    /// @ref Apply, so every animation starts from t = 0.
    ///
    /// ## Playback mode and duration
    ///
    /// The base class owns two pieces of state:
    /// - @ref PlaybackMode  — does this animation loop forever or stop?
    /// - @c mDuration       — only consulted when the mode is @c ONE_SHOT.
    ///
    /// Construct a looper with the default ctor; construct a one-shot with
    /// the @c float ctor. Mutate live with @ref SetDuration / @ref SetLooping;
    /// subclasses whose internal modulators depend on the duration override
    /// @ref OnDurationChanged to rebuild them in lockstep.
    ///
    /// ## Completion
    ///
    /// One-shot animations fire @ref OnComplete exactly once when @c elapsed
    /// first crosses @ref GetDuration. Looping animations never complete and
    /// never fire.
    ///
    /// @code
    ///     // Looping: 2-second sine pulse, runs forever.
    ///     auto pulse = std::make_shared<IntensityPulse>(0.5f);
    ///
    ///     // One-shot: same pulse, but stops + fires OnComplete after 6 s.
    ///     pulse->SetDuration(6.0f);
    ///     pulse->OnComplete = []() { LOG_I("Pulse done."); };
    ///
    ///     // Back to looping at any point:
    ///     pulse->SetLooping();
    /// @endcode
    class Animation
    {
    public:
        /// @brief Construct a looping animation.
        Animation() : mMode(PlaybackMode::LOOP), mDuration(0.0f) {}

        /// @brief Construct a one-shot animation that ends after @p duration seconds.
        explicit Animation(float duration)
            : mMode(PlaybackMode::ONE_SHOT), mDuration(duration) {}

        virtual ~Animation() = default;

        /// @brief Evaluate modulators and write results into @p config.
        /// @param config  Target configuration to modify.
        /// @param elapsed Seconds since the animation began.
        /// @note Should be pure (no internal state advanced by this call).
        virtual void Apply(Config &config, float elapsed) const = 0;

        /// @brief Current playback mode (loop vs one-shot).
        /// @note Virtual so composite animations (e.g. @ref AnimationGroup)
        ///       can derive it from their children.
        virtual PlaybackMode GetPlaybackMode() const { return mMode; }

        /// @brief Total play duration in seconds for @c ONE_SHOT mode.
        /// @note Undefined / unused when mode is @c LOOP. Virtual so composite
        ///       animations can derive it from their children.
        virtual float GetDuration() const { return mDuration; }

        /// @brief Switch to @c ONE_SHOT mode with the given duration.
        /// @details Triggers @ref OnDurationChanged so subclasses can rebuild
        ///          any duration-dependent internal modulators.
        void SetDuration(float duration)
        {
            bool changed = (mMode != PlaybackMode::ONE_SHOT) || (duration != mDuration);
            mMode = PlaybackMode::ONE_SHOT;
            mDuration = duration;
            if (changed)
            {
                OnDurationChanged(duration);
            }
        }

        /// @brief Switch back to @c LOOP mode. Duration becomes irrelevant.
        void SetLooping()
        {
            mMode = PlaybackMode::LOOP;
        }

        /// @brief True iff mode is @c ONE_SHOT and @p elapsed has reached duration.
        bool IsComplete(float elapsed) const
        {
            return GetPlaybackMode() == PlaybackMode::ONE_SHOT && elapsed >= GetDuration();
        }

        /// @brief Callback fired exactly once when the animation completes.
        /// @note Never fires for looping animations.
        std::function<void()> OnComplete;

        /// @brief Set the playback rate multiplier.
        /// @details 1.0 = normal speed (default), 2.0 = double, 0.5 = half.
        ///          Setting it to 0 effectively pauses the animation.
        /// @note    The driver is expected to accumulate elapsed time as
        ///          @c elapsed += dt * GetSpeed() each frame (rather than
        ///          recomputing from clock time) so that changing speed mid-
        ///          flight resumes from the current position rather than
        ///          fast-forwarding.
        void SetSpeed(float speed)
        {
            mSpeed = std::max(0.0f, speed);
        }

        /// @brief Current playback rate multiplier. 1.0 = normal.
        float GetSpeed() const { return mSpeed; }

    protected:
        /// @brief Hook for subclasses to rebuild duration-dependent state.
        /// @details Called by @ref SetDuration when the duration actually changes.
        ///          Default is a no-op.
        virtual void OnDurationChanged(float /*newDuration*/) {}

    private:
        PlaybackMode mMode;
        float mDuration;
        float mSpeed = 1.0f;
    };

    /// @brief Shared owning reference to an Animation.
    using AnimationPtr = std::shared_ptr<Animation>;

    /// @brief Stack of animations applied in registration order.
    /// @details Later animations overwrite earlier ones if they target the same
    ///          field, giving a natural "base → modulation" layering.
    ///
    /// Mode + duration are derived from the children:
    /// - If any child loops, the group loops too.
    /// - Otherwise the group's duration is the longest child's duration.
    class AnimationGroup : public Animation
    {
    public:
        AnimationGroup() : Animation() {}

        void Add(AnimationPtr animation)
        {
            if (animation)
            {
                mAnimations.push_back(std::move(animation));
            }
        }

        void Clear() { mAnimations.clear(); }
        bool IsEmpty() const { return mAnimations.empty(); }
        size_t GetSize() const { return mAnimations.size(); }

        void Apply(Config &config, float elapsed) const override
        {
            // Cascade child speeds: each child receives the group's elapsed
            // scaled by its own speed. With constant speeds this is the
            // mathematical product (group_speed × child_speed); during the
            // first frame after a child speed change there's a small jump,
            // acceptable for an interactive demo.
            for (const auto &a : mAnimations)
            {
                a->Apply(config, elapsed * a->GetSpeed());
            }
        }

        PlaybackMode GetPlaybackMode() const override
        {
            for (const auto &a : mAnimations)
            {
                if (a->GetPlaybackMode() == PlaybackMode::LOOP)
                {
                    return PlaybackMode::LOOP;
                }
            }
            return mAnimations.empty() ? PlaybackMode::LOOP : PlaybackMode::ONE_SHOT;
        }

        float GetDuration() const override
        {
            float maxD = 0.0f;
            for (const auto &a : mAnimations)
            {
                if (a->GetPlaybackMode() == PlaybackMode::LOOP)
                {
                    return 0.0f;
                }
                maxD = std::max(maxD, a->GetDuration());
            }
            return maxD;
        }

    private:
        std::vector<AnimationPtr> mAnimations;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_H_
