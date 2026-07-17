#ifndef _EDGE_LIGHTING_ANIMATION_H_
#define _EDGE_LIGHTING_ANIMATION_H_

#include "core/config.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
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
    /// - @c LOOP     - the cycle repeats forever; elapsed wraps at duration
    ///                 (matches how DOM / CSS / GreenSock / Unity Animator
    ///                 all define animation duration).
    /// - @c ONE_SHOT - the animation completes after exactly one cycle and
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

    /// @brief What the animation writes into its target field once it enters
    ///        the @c STOPPED state (either from natural one-shot completion or
    ///        from an explicit @ref Animation::Stop).
    ///
    /// Every option below only kicks in once the animation has actually run at
    /// least once (@c mHasRun true). A freshly-constructed, never-played
    /// animation always no-ops in Apply so the field stays at whatever the
    /// base config has - there's no meaningful "final value" to hold or
    /// baseline to restore yet.
    ///
    /// The Playing / Paused path is unaffected: the animation writes the
    /// current modulator value in both states. This only controls the frame
    /// after the state transitions to Stopped.
    ///
    /// Note: there is no "revert to base" option. If you want an attached
    /// animation to stop writing and let the base config show through, either
    /// @ref AnimationManager::Detach it, or leave it as never-played. Both
    /// give the fall-through-to-base behaviour for free.
    typedef enum class EndAction
    {
        HOLD_CURRENT = 0, ///< Field settles at @c ApplyAt(cfg, elapsed-at-stop) - wherever the animation was paused/stopped. (Default.)
        HOLD_END,         ///< Field settles at @c ApplyAt(cfg, mDuration).
        HOLD_START,       ///< Field settles at @c ApplyAt(cfg, 0) - the modulator's t=0 value.
        RESTORE           ///< Field settles at the value it had immediately before @ref Animation::Play. Requires a subclass override of @ref Animation::CaptureBaseline / @ref Animation::RestoreBaseline; the base default is a no-op (falls through to base).
    } EndAction;

    /// @brief Per-animation state.
    ///
    /// Every @ref Animation carries its own state independent of any other
    /// animation and independent of the effect's @ref Clock - pausing one
    /// animation does not affect the others, and the driver still calls
    /// @ref Animation::Update / @ref Animation::Apply on paused animations so
    /// they can hold their last-written value.
    ///
    ///   Stopped   - initial state and after @ref Animation::Stop.  Elapsed
    ///               is 0 and does NOT advance.  @ref Animation::Apply is a
    ///               no-op: the target config field is left as-is (use
    ///               @ref Animation::Reset to write the modulator's t=0
    ///               baseline explicitly).
    ///   Playing   - @ref Animation::Update advances elapsed by
    ///               @c dt * @ref Animation::GetSpeed.  @ref Animation::Apply
    ///               writes the current modulator value.
    ///   Paused    - elapsed is frozen at its last-Playing value.
    ///               @ref Animation::Apply still runs and holds that value.
    ///
    /// When a @c ONE_SHOT animation's elapsed crosses
    /// @ref Animation::GetDuration it auto-transitions to @c Stopped (elapsed
    /// reset to 0) and fires @ref Animation::OnComplete once. There is no
    /// separate "Completed" state - completion is just Stopped-with-callback.
    typedef enum class AnimationState
    {
        STOPPED,
        PLAYING,
        PAUSED
    } AnimationState;

    /// @brief Base class for all animations.
    ///
    /// ## Lifecycle
    ///
    /// Each animation owns its own play state (@ref AnimationState) and
    /// elapsed-time accumulator. The driver calls @ref Update once per frame
    /// with the frame delta; the animation advances elapsed only when it is
    /// @c PLAYING. @ref Apply then writes to the target @ref Config field:
    /// while PLAYING / PAUSED it forwards to @ref ApplyAt with the current
    /// elapsed; while STOPPED it dispatches on @ref EndAction (default
    /// @c HOLD_CURRENT). A freshly constructed, never-played animation is a
    /// no-op regardless of end action - see @c mHasRun.
    ///
    /// ## State control (per-animation)
    ///
    /// - @ref Play  - Stopped -> Playing (elapsed = 0);
    ///                Paused -> Playing (elapsed continues).
    /// - @ref Pause - Playing -> Paused (elapsed frozen).
    /// - @ref Stop  - any -> Stopped (elapsed preserved so HOLD_CURRENT and
    ///                @ref GetProgress reflect the stop-time position).
    ///                Apply then writes per @ref EndAction.
    /// - @ref Reset - writes the modulator's t=0 value into config and
    ///                zeroes elapsed; does NOT change state. Works from any
    ///                state so it can act as "rewind while playing" or
    ///                "restore baseline while stopped".
    ///
    /// ## Playback mode and duration
    ///
    /// - @ref PlaybackMode - does this animation loop forever or stop?
    /// - @c mDuration      - length of ONE animation cycle in seconds.
    ///                       Consulted by both modes:
    ///                         * @c LOOP wraps elapsed at duration so the
    ///                           modulator sees a periodic signal;
    ///                         * @c ONE_SHOT completes after one cycle.
    ///                       Set to 0 when the modulator owns its own
    ///                       periodicity (existing oscillator subclasses).
    ///
    /// Construct a looper with the default ctor or a one-shot with the
    /// @c float ctor. Subclasses whose internal modulators depend on the
    /// duration override @ref OnDurationChanged to rebuild them in lockstep.
    ///
    /// ## Callbacks
    ///
    /// - @ref OnComplete     - fired once when a @c ONE_SHOT finishes its
    ///                         cycle (Playing -> Stopped auto-transition).
    ///                         Never fires for loopers. Use to chain
    ///                         animations sequentially.
    /// - @ref OnStateChanged - fired on every state transition with (old, new).
    ///                         Composes cleanly for UI indicators that need
    ///                         to track all three states.
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
        // --- Construction ------------------------------------------------

        /// @brief Construct a looping animation with duration 0.
        /// @details Initial state is @c STOPPED and @c mHasRun is false, so
        ///          @ref Apply is a no-op until the caller calls @ref Play.
        ///          If you want the modulator's t=0 output baked into the
        ///          config immediately, call @ref Reset once at setup - it
        ///          writes @c ApplyAt(cfg, 0) without changing state.
        Animation() = default;

        /// @brief Construct a one-shot animation that ends after @p duration seconds.
        explicit Animation(float duration)
            : mMode(PlaybackMode::ONE_SHOT), mDuration(duration) {}

        virtual ~Animation() = default;

        // --- Control -----------------------------------------------------

        /// @brief Enter the @c PLAYING state.
        /// @details From @c STOPPED, elapsed is reset to 0 (so a completed
        ///          one-shot restarts from the beginning, and the settled
        ///          HOLD_* state parked at the stop-time value is dropped).
        ///          From @c PAUSED, elapsed continues from its frozen value
        ///          (there is no separate "Resume" method - @c Play from
        ///          @c PAUSED *is* resume). From @c PLAYING, this is a no-op
        ///          (no state change, no callback).
        virtual void Play()
        {
            if (mState == AnimationState::PLAYING)
            {
                return;
            }
            if (mState == AnimationState::STOPPED)
            {
                mElapsed = 0.0f;
            }
            transitionTo(AnimationState::PLAYING);
        }

        /// @brief Freeze elapsed at its current value; @ref Apply keeps writing it.
        /// @note Only valid from @c PLAYING; no-op otherwise.
        virtual void Pause()
        {
            if (mState != AnimationState::PLAYING)
            {
                return;
            }
            transitionTo(AnimationState::PAUSED);
        }

        /// @brief Enter the @c STOPPED state; @ref Apply then writes per @ref EndAction.
        /// @details Symmetric with the natural one-shot completion path in
        ///          @ref Update. @c mElapsed is preserved so @ref GetProgress
        ///          and @c HOLD_CURRENT read the stop-time value; @c mHasRun
        ///          is set so subsequent @ref Apply calls honour the end
        ///          action. No-op if already @c STOPPED.
        virtual void Stop()
        {
            if (mState == AnimationState::STOPPED)
            {
                return;
            }
            mHasRun = true;
            transitionTo(AnimationState::STOPPED);
        }

        /// @brief Zero elapsed and write the modulator's t=0 value into @p cfg.
        /// @details Works from any state; the state itself is unchanged.
        ///          Use as "rewind while playing" (Playing stays Playing,
        ///          the animation replays from the beginning) or as
        ///          "restore baseline while stopped" (Stopped stays Stopped
        ///          but the config field is put back to the modulator's
        ///          initial output). Clearing @c mHasRun matches the "has
        ///          never played" semantics so any @c HOLD_* / RESTORE end
        ///          action goes dormant until the next real @ref Play.
        virtual void Reset(Config &cfg)
        {
            mElapsed = 0.0f;
            mHasRun = false;
            ApplyAt(cfg, 0.0f);
        }

        // --- Drive -------------------------------------------------------

        /// @brief Advance elapsed by @p dt when Playing; may transition
        ///        Playing -> Stopped for a completed one-shot.
        /// @param dt Frame delta in seconds.
        /// @details No-op when Paused or Stopped (elapsed is frozen).
        ///          Speed is folded in: elapsed accumulates
        ///          @c dt * @ref GetSpeed.
        virtual void Update(float dt)
        {
            if (mState != AnimationState::PLAYING)
            {
                return;
            }
            mElapsed += dt * mSpeed;
            // Duration semantics: mDuration is the length of ONE animation
            // cycle, uniform across both modes.
            //   LOOP     - wrap mElapsed at mDuration so ApplyAt sees a
            //              periodic signal that resets every cycle.
            //   ONE_SHOT - complete after exactly one cycle, transitioning to
            //              Stopped and firing OnComplete once.
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
                    // Natural one-shot completion. Symmetric with Stop():
                    // clamp the elapsed overshoot to duration for a clean
                    // GetProgress = 1, mark mHasRun, and transition. Which
                    // value STOPPED-Apply writes is decided by mEndAction,
                    // not by mElapsed.
                    mElapsed = mDuration;
                    mHasRun = true;
                    transitionTo(AnimationState::STOPPED);
                    if (OnComplete)
                    {
                        OnComplete();
                    }
                }
            }
        }

        /// @brief Write the animation's current value into @p cfg.
        /// @details While PLAYING / PAUSED, forwards to @ref ApplyAt with the
        ///          current elapsed. While STOPPED, dispatches on
        ///          @ref EndAction (@c HOLD_CURRENT default) - but only if
        ///          the animation has actually run at least once; a
        ///          never-played animation is a no-op regardless of end
        ///          action, so freshly-attached hold animations don't snap
        ///          the field to a terminal value on attach.
        /// @note Virtual so composite animations (@ref AnimationGroup) can
        ///       bypass the Stopped dispatch and always forward to children;
        ///       the group's own state is a broadcast label, not a gate on
        ///       whether children execute.
        virtual void Apply(Config &cfg) const
        {
            if (mState != AnimationState::STOPPED)
            {
                ApplyAt(cfg, mElapsed);
                return;
            }
            if (!mHasRun)
            {
                return;
            }
            switch (mEndAction)
            {
            case EndAction::HOLD_CURRENT:
            {
                ApplyAt(cfg, mElapsed);
                return;
            }
            case EndAction::HOLD_END:
            {
                ApplyAt(cfg, mDuration);
                return;
            }
            case EndAction::HOLD_START:
            {
                ApplyAt(cfg, 0.0f);
                return;
            }
            case EndAction::RESTORE:
            {
                RestoreBaseline(cfg);
                return;
            }
            }
        }

        // --- Introspection -----------------------------------------------

        AnimationState GetState() const { return mState; }
        float GetElapsed() const { return mElapsed; }
        bool IsPlaying() const { return mState == AnimationState::PLAYING; }
        bool IsPaused() const { return mState == AnimationState::PAUSED; }
        bool IsStopped() const { return mState == AnimationState::STOPPED; }

        /// @brief Directly overwrite the elapsed accumulator.
        /// @details Normally elapsed is advanced by @ref Update. Explicit
        ///          setting is useful for scrubbing / testing and for
        ///          stateless callers that track elapsed themselves. Does
        ///          NOT change state or trigger @ref OnComplete on its own -
        ///          the next @ref Update tick will complete a one-shot whose
        ///          elapsed has crossed @ref GetDuration.
        void SetElapsed(float elapsed) { mElapsed = std::max(0.0f, elapsed); }

        /// @brief Normalised playback position in @c [0, 1].
        /// @details @c elapsed / @c duration - the reciprocal of a duration
        ///          slider for timeline UIs. Loop mode wraps elapsed at
        ///          duration in @ref Update so a playing looper lives in
        ///          @c [0, 1). A completed one-shot reports 1 (elapsed is
        ///          clamped to duration on completion). An explicitly
        ///          @ref Stop -ped animation reports the stop-time position.
        ///          A never-played animation reports 0.
        /// @returns 0 when @ref GetDuration is 0 - the animation's modulator
        ///          owns its own periodicity and there is no natural
        ///          normalisation.
        float GetProgress() const
        {
            const float d = GetDuration();
            return (d > 0.0f) ? (mElapsed / d) : 0.0f;
        }

        /// @brief Set the normalised playback position.
        /// @param progress Clamped to @c [0, 1] before scaling; values outside
        ///                 that range are silently pulled in.
        /// @details Same "does not change state" caveat as @ref SetElapsed -
        ///          a @c ONE_SHOT animation set past its end still needs an
        ///          @ref Update tick to fire @ref OnComplete. No-op when
        ///          @ref GetDuration is 0 (modulator owns its periodicity).
        void SetProgress(float progress)
        {
            const float d = GetDuration();
            if (d <= 0.0f)
            {
                return;
            }
            mElapsed = std::clamp(progress, 0.0f, 1.0f) * d;
        }

        // --- Identity ----------------------------------------------------
        //
        // Optional human-readable label - the animation carries it so UI code
        // (row headers) and callback lambdas (OnComplete / OnStateChanged
        // logging) don't need a parallel name vector keyed to attach order.
        // Empty by default; set once at construction and left alone.

        void SetName(std::string name) { mName = std::move(name); }
        const std::string &GetName() const { return mName; }

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
        ///          opts out - the modulator owns its own periodicity and
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
        ///          elapsed accumulation - semantically equivalent to Pause
        ///          for the value, but the state stays Playing. Use
        ///          @ref Pause when you want the state to reflect it.
        void SetSpeed(float speed) { mSpeed = std::max(0.0f, speed); }

        /// @brief Current playback rate multiplier. 1.0 = normal.
        float GetSpeed() const { return mSpeed; }

        // --- End-action policy -------------------------------------------

        /// @brief What to do with the target field once the animation stops.
        /// @details See @ref EndAction for the full menu. Symmetric across
        ///          natural one-shot completion and manual @ref Stop - both
        ///          paths route through the same STOPPED-Apply dispatch.
        ///          Takes effect the next frame; the current running value
        ///          is unaffected.
        void SetEndAction(EndAction action) { mEndAction = action; }

        /// @brief Current end-action policy.
        EndAction GetEndAction() const { return mEndAction; }

        // --- RESTORE snapshot (opt-in per subclass) ----------------------
        //
        // Base default is a no-op, so a subclass that doesn't override
        // CaptureBaseline / RestoreBaseline silently degrades
        // EndAction::RESTORE to "no-op after Stop" for that field.
        // Implementing them is straightforward for any subclass that knows
        // which config field(s) it writes - see the neon-animations.h
        // subclasses for examples.
        //
        // Callers must invoke CaptureBaseline(cfg) BEFORE Play so the
        // snapshot captures the pre-animation value. There's no auto-
        // snapshot because Play doesn't take a Config&. RestoreBaseline is
        // invoked automatically by Apply while STOPPED, so it stays
        // protected - no external caller needs to reach it.

        /// @brief Snapshot whatever fields RestoreBaseline needs, from cfg.
        ///        Base is a no-op; override in subclasses that use RESTORE.
        virtual void CaptureBaseline(const Config & /*cfg*/) {}

        // --- Callbacks (public data) -------------------------------------

        /// @brief Fired exactly once when a @c ONE_SHOT completes its cycle.
        /// @note Never fires for @c LOOP animations. Prefer for the
        ///       common "chain B after A" case.
        std::function<void()> OnComplete;

        /// @brief Fired on every state transition, with (previous, current).
        /// @note Lets UI code observe all state changes (including the
        ///       auto Playing -> Stopped edge that also fires OnComplete)
        ///       with a single callback.
        std::function<void(AnimationState /*prev*/, AnimationState /*now*/)> OnStateChanged;

    protected:
        /// @brief Subclass hook - write the modulator@elapsed value into @p cfg.
        /// @details @ref Apply routes through here for PLAYING / PAUSED and
        ///          for the STOPPED @c HOLD_* dispatches; @ref Reset invokes
        ///          it with @c elapsed = 0. Subclasses should keep this pure
        ///          (no side effects other than writing @p cfg).
        virtual void ApplyAt(Config &cfg, float elapsed) const = 0;

        /// @brief Hook for subclasses to rebuild duration-dependent state.
        /// @details Called by @ref SetDuration when the duration actually
        ///          changes. Default is a no-op.
        virtual void OnDurationChanged(float /*newDuration*/) {}

        /// @brief Write the previously-snapshotted field(s) back into cfg.
        /// @details Called from STOPPED-Apply when @ref GetEndAction ==
        ///          @c RESTORE. Base is a no-op. Protected because only the
        ///          base's own @ref Apply invokes it - subclasses override
        ///          to write their saved field(s) back.
        virtual void RestoreBaseline(Config & /*cfg*/) const {}

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

    private:
        // Default state is STOPPED - a newly-constructed animation does
        // nothing on Apply (mHasRun is false too) until the caller
        // explicitly Play()s it. Callers that want the modulator's t=0
        // baseline written into the config (typical "seed the field so a
        // Stopped animation still shows its initial value") should call
        // @ref Reset(cfg) once at setup - that triggers @c ApplyAt(cfg, 0)
        // without changing state.
        AnimationState mState = AnimationState::STOPPED;
        float mElapsed = 0.0f;
        PlaybackMode mMode = PlaybackMode::LOOP;
        float mDuration = 0.0f;
        float mSpeed = 1.0f;
        EndAction mEndAction = EndAction::HOLD_CURRENT;
        bool mHasRun = false; ///< True once the animation has advanced past Play at least once.
        std::string mName;    ///< Optional label; see SetName / GetName.
    };

    /// @brief Shared owning reference to an Animation.
    using AnimationPtr = std::shared_ptr<Animation>;

    /// @brief Container of animations updated and applied in registration order.
    ///
    /// @details Behaviour:
    /// - @ref Add / @ref Remove work live - children can be added or removed
    ///   while the group's other children keep running.
    /// - @ref Play / @ref Pause / @ref Stop / @ref Reset are broadcast to
    ///   every child (convenience for "control everything at once"); each
    ///   child can also be controlled individually via its own methods.
    /// - @ref Update / @ref Apply are forwarded to every child.
    /// - Group state / duration / mode are derived aggregates:
    ///     * Playing if any child is Playing (else Paused > Stopped).
    ///     * Mode is LOOP if any child loops, else ONE_SHOT.
    ///     * Duration is the longest child duration (0 if any child loops).
    ///
    /// Later-added children write their fields on top of earlier ones - the
    /// natural "base → modulation" layering.  Prefer @ref OnComplete on
    /// individual children for sequencing (chain B to fire when A finishes).
    class AnimationGroup : public Animation
    {
    public:
        // --- Construction ------------------------------------------------

        AnimationGroup() = default;

        // --- Composition -------------------------------------------------

        /// @brief Append a child. Added children start in whatever state they
        ///        already carry (typically Stopped - call @c child->Play() or
        ///        @ref Play on the group to start them).
        void Add(AnimationPtr animation)
        {
            if (animation)
            {
                mAnimations.push_back(std::move(animation));
            }
        }

        /// @brief Detach a child (by shared_ptr identity). The child keeps
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
        // child. The base's per-animation state still tracks the group's own
        // state (so `IsPlaying()` on the group reflects the last broadcast
        // command), but child states can drift if a child is controlled
        // individually.

        void Play() override
        {
            Animation::Play();
            for (const auto &a : mAnimations)
            {
                a->Play();
            }
        }

        void Pause() override
        {
            Animation::Pause();
            for (const auto &a : mAnimations)
            {
                a->Pause();
            }
        }

        void Stop() override
        {
            Animation::Stop();
            for (const auto &a : mAnimations)
            {
                a->Stop();
            }
        }

        void Reset(Config &cfg) override
        {
            Animation::Reset(cfg);
            for (const auto &a : mAnimations)
            {
                a->Reset(cfg);
            }
        }

        // --- Drive -------------------------------------------------------

        void Update(float dt) override
        {
            Animation::Update(dt);
            for (const auto &a : mAnimations)
            {
                a->Update(dt);
            }
        }

        /// @brief Forward Apply to every child unconditionally.
        /// @details Overrides the base Apply's Stopped dispatch: the group's
        ///          own state is a broadcast convenience, not a gate on
        ///          children. A Playing child in a Stopped group must still
        ///          write to config, otherwise Play on the child does
        ///          nothing when the group defaults to Stopped. Each child's
        ///          own Apply performs its own state check.
        void Apply(Config &cfg) const override
        {
            for (const auto &a : mAnimations)
            {
                a->Apply(cfg);
            }
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
        ///          (Stopped children are handled inside Animation::Apply
        ///          per the child's own end action).
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
