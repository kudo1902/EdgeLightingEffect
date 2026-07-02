#ifndef _EDGE_LIGHTING_ANIMATION_H_
#define _EDGE_LIGHTING_ANIMATION_H_

#include "core/config.h"
#include <algorithm>
#include <cmath>
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
    /// an @ref Animation owns its modulator(s), its own play state, its own
    /// elapsed-time accumulator, and writes results into a target @ref Config
    /// field.  Groups combine multiple animations in parallel.
    ///
    /// @{

    /// @brief How an animation behaves once it finishes one full cycle.
    ///
    /// @ref Animation::GetDuration is the length of ONE cycle in both modes:
    /// - @c LOOP     — the cycle repeats forever; elapsed wraps at duration
    ///                 (matches how DOM / CSS / GreenSock / Unity Animator
    ///                 all define animation duration).
    /// - @c ONE_SHOT — the animation completes after exactly one cycle and
    ///                 fires @ref Animation::OnComplete.
    ///
    /// If @c mDuration is 0 the modulator inside the subclass is expected to
    /// own its own periodicity (e.g. an @c Oscillator with an explicit
    /// period) and elapsed advances monotonically. That's the fallback path
    /// existing Oscillator-based subclasses take.
    typedef enum class PlaybackMode
    {
        LOOP,    ///< Elapsed wraps at duration; never completes.
        ONE_SHOT ///< Runs for one cycle (= duration) then completes.
    } PlaybackMode;

    /// @brief Per-animation state.
    ///
    /// Every @ref Animation carries its own state independent of any other
    /// animation and independent of the effect's @ref Clock — pausing one
    /// animation does not affect the others, and the driver still calls
    /// @ref Animation::Update / @ref Animation::Apply on paused animations so
    /// they can hold their last-written value.
    ///
    ///   Stopped   — initial state and after @ref Animation::Stop.  Elapsed
    ///               is 0 and does NOT advance.  @ref Animation::Apply is a
    ///               no-op: the target config field is left as-is (use
    ///               @ref Animation::Reset to write the modulator's t=0
    ///               baseline explicitly).
    ///   Playing   — @ref Animation::Update advances elapsed by
    ///               @c dt * @ref Animation::GetSpeed.  @ref Animation::Apply
    ///               writes the current modulator value.
    ///   Paused    — elapsed is frozen at its last-Playing value.
    ///               @ref Animation::Apply still runs and holds that value.
    ///   Completed — set automatically when a @c ONE_SHOT animation's elapsed
    ///               crosses @ref Animation::GetDuration.  Elapsed is clamped
    ///               to duration, does not advance, and @ref Animation::Apply
    ///               keeps writing the modulator's final value.  Fires
    ///               @ref Animation::OnComplete on entry.
    typedef enum class AnimationState
    {
        Stopped,
        Playing,
        Paused,
        Completed
    } AnimationState;

    /// @brief Base class for all animations.
    ///
    /// ## Lifecycle
    ///
    /// Each animation owns its own play state (@ref AnimationState) and
    /// elapsed-time accumulator.  The driver calls @ref Update once per frame
    /// with the frame delta; the animation advances elapsed only when it is
    /// @c Playing.  @ref Apply is then called and writes the modulator's
    /// value into the target @ref Config field (unless the animation is
    /// @c Stopped, in which case Apply is a no-op — call @ref Reset to force
    /// the t=0 baseline into the config once).
    ///
    /// ## State control (per-animation)
    ///
    /// - @ref Play  — Stopped/Completed → Playing (elapsed = 0);
    ///                Paused → Playing (elapsed continues).
    /// - @ref Pause — Playing → Paused (elapsed frozen).
    /// - @ref Stop  — any → Stopped (elapsed = 0, frozen; Apply becomes no-op).
    /// - @ref Reset — writes the modulator's t=0 value into config and
    ///                zeroes elapsed; does NOT change state.  Works from any
    ///                state so it can act as "rewind while playing" or
    ///                "restore baseline while stopped".
    ///
    /// ## Playback mode and duration
    ///
    /// - @ref PlaybackMode — does this animation loop forever or stop?
    /// - @c mDuration      — length of ONE animation cycle in seconds.
    ///                       Consulted by both modes:
    ///                         * @c LOOP wraps elapsed at duration so the
    ///                           modulator sees a periodic signal;
    ///                         * @c ONE_SHOT completes after one cycle.
    ///                       Set to 0 when the modulator owns its own
    ///                       periodicity (existing oscillator subclasses).
    ///
    /// Construct a looper with the default ctor or a one-shot with the
    /// @c float ctor.  Subclasses whose internal modulators depend on the
    /// duration override @ref OnDurationChanged to rebuild them in lockstep.
    ///
    /// ## Callbacks
    ///
    /// - @ref OnComplete       — fired once when a @c ONE_SHOT transitions
    ///                           into @c Completed.  Never fires for loopers.
    ///                           Use it to chain animations sequentially.
    /// - @ref OnStateChanged   — fired on every state transition with (old, new).
    ///                           Redundant with @c OnComplete for the completion
    ///                           edge but composes cleaner for arbitrary graphs
    ///                           (e.g. UI indicators that need to track all four
    ///                           states).
    ///
    /// @code
    ///     auto pulse = std::make_shared<IntensityPulse>(0.5f);
    ///     pulse->Play();
    ///
    ///     // ... per frame:
    ///     pulse->Update(dt);
    ///     pulse->Apply(cfg);
    ///
    ///     // Pause it while another animation runs unaffected:
    ///     pulse->Pause();
    ///
    ///     // Chain: when this one-shot completes, start the next.
    ///     pulse->OnComplete = [next]() { next->Play(); };
    /// @endcode
    class Animation
    {
    public:
        /// @brief Construct a looping animation with duration 0.
        /// @details Initial state is @c Stopped: @ref Apply is a no-op until
        ///          the caller calls @ref Play. If you want the modulator's
        ///          t=0 output baked into the config immediately (so the
        ///          animated field starts at the modulator's baseline
        ///          instead of whatever value was previously there), call
        ///          @ref Reset once on the fresh animation — it writes
        ///          ApplyAt(cfg, 0) without changing state.
        Animation() = default;

        /// @brief Construct a one-shot animation that ends after @p duration seconds.
        explicit Animation(float duration)
            : mMode(PlaybackMode::ONE_SHOT), mDuration(duration) {}

        virtual ~Animation() = default;

        // --- Control -----------------------------------------------------

        /// @brief Enter the @c Playing state.
        /// @details From @c Stopped or @c Completed, elapsed is reset to 0.
        ///          From @c Paused, elapsed continues from its frozen value
        ///          (there is no separate "Resume" method — @c Play from
        ///          @c Paused *is* resume).  From @c Playing, this is a
        ///          no-op (no state change, no callback).
        virtual void Play()
        {
            if (mState == AnimationState::Playing)
            {
                return;
            }
            if (mState == AnimationState::Stopped || mState == AnimationState::Completed)
            {
                mElapsed = 0.0f;
            }
            transitionTo(AnimationState::Playing);
        }

        /// @brief Freeze elapsed at its current value; @ref Apply keeps writing it.
        /// @note Only valid from @c Playing; no-op otherwise.
        virtual void Pause()
        {
            if (mState != AnimationState::Playing)
            {
                return;
            }
            transitionTo(AnimationState::Paused);
        }

        /// @brief Reset elapsed to 0 and enter @c Stopped.
        /// @details @ref Apply becomes a no-op after @c Stop — the target
        ///          config field is left as-is.  Call @ref Reset if you want
        ///          the modulator's t=0 baseline written back into config.
        ///          No-op if already Stopped.
        virtual void Stop()
        {
            if (mState == AnimationState::Stopped)
            {
                return;
            }
            mElapsed = 0.0f;
            transitionTo(AnimationState::Stopped);
        }

        /// @brief Zero elapsed and write the modulator's t=0 value into @p cfg.
        /// @details Works from any state; the state itself is unchanged.
        ///          Use as a "rewind while playing" (Playing stays Playing,
        ///          the animation replays from the beginning) or as
        ///          "restore baseline" (Stopped stays Stopped but the config
        ///          field is put back to the modulator's initial output).
        virtual void Reset(Config &cfg)
        {
            mElapsed = 0.0f;
            ApplyAt(cfg, 0.0f);
        }

        // --- Drive -------------------------------------------------------

        /// @brief Advance elapsed by @p dt when Playing; may transition
        ///        Playing → Completed for one-shots.
        /// @param dt Frame delta in seconds.
        /// @details No-op when Paused, Stopped, or Completed (elapsed is
        ///          frozen).  Speed is folded in: elapsed accumulates
        ///          @c dt * @ref GetSpeed.
        virtual void Update(float dt)
        {
            if (mState != AnimationState::Playing)
            {
                return;
            }
            mElapsed += dt * mSpeed;
            // Duration semantics: mDuration is the length of ONE animation
            // cycle, uniform across both modes.
            //   LOOP     — wrap mElapsed at mDuration so ApplyAt sees a
            //              periodic signal that resets every cycle.
            //   ONE_SHOT — complete after exactly one cycle.
            //   mDuration == 0 opts out: the modulator is expected to own its
            //   own periodicity (e.g. an internal Oscillator with its own
            //   period) and elapsed advances monotonically. All existing
            //   Oscillator-based subclasses fall into this path.
            if (mDuration > 0.0f)
            {
                if (mMode == PlaybackMode::LOOP)
                {
                    mElapsed = std::fmod(mElapsed, mDuration);
                }
                else if (mMode == PlaybackMode::ONE_SHOT && mElapsed >= mDuration)
                {
                    mElapsed = mDuration;
                    transitionTo(AnimationState::Completed);
                    if (OnComplete)
                    {
                        OnComplete();
                    }
                }
            }
        }

        /// @brief Write the modulator's current value into @p cfg.
        /// @details No-op when @c Stopped.  For @c Playing / @c Paused /
        ///          @c Completed the subclass's @ref ApplyAt is invoked with
        ///          the current @c mElapsed.
        /// @note Virtual so composite animations (@ref AnimationGroup) can
        ///       bypass the Stopped-skip and always forward to children —
        ///       the group's own state is a broadcast label, not a gate on
        ///       whether children execute.
        virtual void Apply(Config &cfg) const
        {
            if (mState == AnimationState::Stopped)
            {
                return;
            }
            ApplyAt(cfg, mElapsed);
        }

        // --- Introspection -----------------------------------------------

        AnimationState GetState() const { return mState; }
        float GetElapsed() const { return mElapsed; }
        bool IsPlaying() const { return mState == AnimationState::Playing; }
        bool IsPaused() const { return mState == AnimationState::Paused; }
        bool IsStopped() const { return mState == AnimationState::Stopped; }
        bool IsCompleted() const { return mState == AnimationState::Completed; }

        // --- Playback mode / duration / speed ----------------------------

        /// @brief Current playback mode (loop vs one-shot).
        /// @note Virtual so composite animations (e.g. @ref AnimationGroup)
        ///       can derive it from their children.
        virtual PlaybackMode GetPlaybackMode() const { return mMode; }

        /// @brief Set the playback mode. Does NOT touch the duration.
        void SetPlaybackMode(PlaybackMode mode) { mMode = mode; }

        /// @brief Length of one animation cycle in seconds.
        /// @details In @c LOOP mode, elapsed wraps at this value so the
        ///          modulator sees a periodic signal. In @c ONE_SHOT mode,
        ///          the animation completes after one cycle. A value of 0
        ///          opts out — the modulator owns its own periodicity and
        ///          elapsed advances monotonically. Virtual so composite
        ///          animations can derive it from their children.
        virtual float GetDuration() const { return mDuration; }

        /// @brief Set the wall-clock duration in seconds. Does NOT touch the mode.
        /// @details Triggers @ref OnDurationChanged so subclasses can rebuild
        ///          any duration-dependent internal modulators (e.g. an
        ///          @ref Ease whose visual transition must match the
        ///          completion latch).
        void SetDuration(float duration)
        {
            if (duration != mDuration)
            {
                mDuration = duration;
                OnDurationChanged(duration);
            }
        }

        /// @brief Set the playback rate multiplier.
        /// @details 1.0 = normal speed (default), 2.0 = double, 0.5 = half.
        ///          Setting it to 0 keeps the animation Playing but freezes
        ///          elapsed accumulation — semantically equivalent to Pause
        ///          for the value, but the state stays Playing.  Use
        ///          @ref Pause when you want the state to reflect it.
        void SetSpeed(float speed) { mSpeed = std::max(0.0f, speed); }

        /// @brief Current playback rate multiplier. 1.0 = normal.
        float GetSpeed() const { return mSpeed; }

        // --- Callbacks ---------------------------------------------------

        /// @brief Fired exactly once when a @c ONE_SHOT completes.
        /// @note Never fires for @c LOOP animations.  Prefer for the
        ///       common "chain B after A" case.
        std::function<void()> OnComplete;

        /// @brief Fired on every state transition, with (previous, current).
        /// @note Redundant with @ref OnComplete for the completion edge but
        ///       lets UI code observe all four states with a single callback.
        std::function<void(AnimationState /*prev*/, AnimationState /*now*/)> OnStateChanged;

    protected:
        /// @brief Subclass hook — write the modulator@elapsed value into @p cfg.
        /// @details The public @ref Apply routes through here (skipping the
        ///          call entirely when @c Stopped), and @ref Reset invokes it
        ///          with @c elapsed = 0.  Subclasses should keep this pure
        ///          (no side effects other than writing @p cfg).
        virtual void ApplyAt(Config &cfg, float elapsed) const = 0;

        /// @brief Hook for subclasses to rebuild duration-dependent state.
        /// @details Called by @ref SetDuration when the duration actually changes.
        ///          Default is a no-op.
        virtual void OnDurationChanged(float /*newDuration*/) {}

    private:
        void transitionTo(AnimationState next)
        {
            AnimationState prev = mState;
            if (prev == next)
            {
                return;
            }
            mState = next;
            if (OnStateChanged)
            {
                OnStateChanged(prev, next);
            }
        }

        // Default state is Stopped — a newly-constructed animation does
        // nothing on Apply until the caller explicitly Play()s it. Callers
        // that want the modulator's t=0 baseline written into the config
        // (typical "seed the field so a Stopped animation still shows its
        // initial value") should call @ref Reset(cfg) once at setup — that
        // triggers @ref ApplyAt(cfg, 0) without changing state. The C ABI
        // (@c el_animation_apply) preserves its old stateless semantics by
        // auto-Playing when it finds the animation Stopped.
        AnimationState mState = AnimationState::Stopped;
        float mElapsed = 0.0f;
        PlaybackMode mMode = PlaybackMode::LOOP;
        float mDuration = 0.0f;
        float mSpeed = 1.0f;

    public:
        /// @brief Directly set the elapsed accumulator (advanced).
        /// @details Normally elapsed is advanced by @ref Update. Explicit
        ///          setting is useful for stateless callers (like the C ABI
        ///          where the caller tracks elapsed themselves) and for
        ///          scrubbing / testing. Does NOT change state or trigger
        ///          @ref OnComplete on its own — the next @ref Update tick
        ///          will complete a one-shot whose elapsed has crossed
        ///          @ref GetDuration.
        void SetElapsed(float elapsed) { mElapsed = std::max(0.0f, elapsed); }
    };

    /// @brief Shared owning reference to an Animation.
    using AnimationPtr = std::shared_ptr<Animation>;

    /// @brief Container of animations updated and applied in registration order.
    ///
    /// @details Behaviour:
    /// - @ref Add / @ref Remove work live — children can be added or removed
    ///   while the group's other children keep running.
    /// - @ref Play / @ref Pause / @ref Stop / @ref Reset are broadcast to
    ///   every child (convenience for "control everything at once"); each
    ///   child can also be controlled individually via its own methods.
    /// - @ref Update / @ref Apply are forwarded to every child.
    /// - Group state / duration / mode are derived aggregates:
    ///     * Playing if any child is Playing (else Paused > Completed > Stopped).
    ///     * Mode is LOOP if any child loops, else ONE_SHOT.
    ///     * Duration is the longest child duration (0 if any child loops).
    ///
    /// Later-added children write their fields on top of earlier ones — the
    /// natural "base → modulation" layering.  Prefer @ref OnComplete on
    /// individual children for sequencing (chain B to fire when A finishes).
    class AnimationGroup : public Animation
    {
    public:
        AnimationGroup() = default;

        // --- Composition -------------------------------------------------

        /// @brief Append a child. Added children start in whatever state they
        ///        already carry (typically Stopped — call @c child->Play() or
        ///        @ref Play on the group to start them).
        void Add(AnimationPtr animation)
        {
            if (animation)
            {
                mAnimations.push_back(std::move(animation));
            }
        }

        /// @brief Detach a child (by shared_ptr identity).  The child keeps
        ///        running if the caller still holds a reference.
        /// @return true if the child was found and removed.
        bool Remove(const AnimationPtr &animation)
        {
            auto it = std::find(mAnimations.begin(), mAnimations.end(), animation);
            if (it == mAnimations.end())
            {
                return false;
            }
            mAnimations.erase(it);
            return true;
        }

        void Clear() { mAnimations.clear(); }
        bool IsEmpty() const { return mAnimations.empty(); }
        size_t GetSize() const { return mAnimations.size(); }
        const std::vector<AnimationPtr> &GetChildren() const { return mAnimations; }

        // --- Broadcast control ------------------------------------------
        //
        // These override the base's control methods to also fan out to every
        // child.  The base's per-animation state still tracks the group's own
        // state (so `IsPlaying()` on the group reflects the last broadcast
        // command), but child states can drift if a child is controlled
        // individually.

        void Play() override
        {
            Animation::Play();
            for (const auto &a : mAnimations) a->Play();
        }
        void Pause() override
        {
            Animation::Pause();
            for (const auto &a : mAnimations) a->Pause();
        }
        void Stop() override
        {
            Animation::Stop();
            for (const auto &a : mAnimations) a->Stop();
        }
        void Reset(Config &cfg) override
        {
            Animation::Reset(cfg);
            for (const auto &a : mAnimations) a->Reset(cfg);
        }

        // --- Drive -------------------------------------------------------

        void Update(float dt) override
        {
            Animation::Update(dt);
            for (const auto &a : mAnimations) a->Update(dt);
        }

        /// @brief Forward Apply to every child unconditionally.
        /// @details Overrides the base Apply's "skip when Stopped" behaviour:
        ///          the group's own state is a broadcast convenience, not a
        ///          gate on children. A Playing child in a Stopped group must
        ///          still write to config, otherwise Play on the child does
        ///          nothing when the group defaults to Stopped. Each child's
        ///          own Apply performs its own state check.
        void Apply(Config &cfg) const override
        {
            for (const auto &a : mAnimations) a->Apply(cfg);
        }

        // --- Derived introspection --------------------------------------

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

    protected:
        /// @brief Forward Apply to every child in registration order.
        /// @details Each child's @ref Apply performs its own state check
        ///          (Stopped children are skipped inside Animation::Apply).
        void ApplyAt(Config &cfg, float /*elapsed*/) const override
        {
            for (const auto &a : mAnimations)
            {
                a->Apply(cfg);
            }
        }

    private:
        std::vector<AnimationPtr> mAnimations;
    };

    /// @}

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ANIMATION_H_
