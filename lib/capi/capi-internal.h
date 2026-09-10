#ifndef _CAPI_INTERNAL_H_
#define _CAPI_INTERNAL_H_

#include "edge-lighting-capi.h"

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/neon-renderer.h"
#include "renderer/debug-renderer.h"
#include "renderer/droplets-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/modulator.h"
#include "util/log-util.h"
#include "util/segment-utils.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <exception>
#include <utility>
#include <vector>

#if defined(PLATFORM_MACOS) || defined(PLATFORM_WINDOWS)
#undef LOG_D
#define LOG_D(fmt, ...) ((void)0)
#endif

// ==========================================================================
// Enum ABI parity - fires at compile time if a C++ enum reorder ever silently
// diverges from its el_* mirror. Keep asserts one-per-value; that way a broken
// build tells you exactly which enumerator moved.
// ==========================================================================
static_assert(static_cast<int>(EdgeLighting::Winding::CLOCKWISE) == EL_WINDING_CLOCKWISE);
static_assert(static_cast<int>(EdgeLighting::Winding::COUNTER_CLOCKWISE) == EL_WINDING_COUNTER_CLOCKWISE);

static_assert(static_cast<int>(EdgeLighting::GlowSide::BOTH) == EL_GLOW_SIDE_BOTH);
static_assert(static_cast<int>(EdgeLighting::GlowSide::INSIDE) == EL_GLOW_SIDE_INSIDE);
static_assert(static_cast<int>(EdgeLighting::GlowSide::OUTSIDE) == EL_GLOW_SIDE_OUTSIDE);

static_assert(static_cast<int>(EdgeLighting::BlendSpace::RGB) == EL_BLEND_SPACE_RGB);
static_assert(static_cast<int>(EdgeLighting::BlendSpace::HSV) == EL_BLEND_SPACE_HSV);
static_assert(static_cast<int>(EdgeLighting::BlendSpace::HSL) == EL_BLEND_SPACE_HSL);

static_assert(static_cast<int>(EdgeLighting::Waveform::SINE) == EL_WAVE_SINE);
static_assert(static_cast<int>(EdgeLighting::Waveform::TRIANGLE) == EL_WAVE_TRIANGLE);
static_assert(static_cast<int>(EdgeLighting::Waveform::SQUARE) == EL_WAVE_SQUARE);
static_assert(static_cast<int>(EdgeLighting::Waveform::SAWTOOTH) == EL_WAVE_SAWTOOTH);

static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_INTENSITY) == EL_FIELD_NEON_INTENSITY);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_LINE_WIDTH) == EL_FIELD_NEON_LINE_WIDTH);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_GLOW_RADIUS) == EL_FIELD_NEON_GLOW_RADIUS);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_BLOOM_STRENGTH) == EL_FIELD_NEON_BLOOM_STRENGTH);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_FILAMENT_FALLOFF) == EL_FIELD_NEON_FILAMENT_FALLOFF);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_GLOW_SIDE_SOFTNESS) == EL_FIELD_NEON_GLOW_SIDE_SOFTNESS);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::NEON_HUE_ROTATION_RATE) == EL_FIELD_NEON_HUE_ROTATION_RATE);

static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_PERIMETER_POSITION) == EL_FIELD_LENS_FLARE_PERIMETER_POSITION);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_PERIMETER_OFFSET) == EL_FIELD_LENS_FLARE_PERIMETER_OFFSET);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_SIZE) == EL_FIELD_LENS_FLARE_SIZE);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_INTENSITY) == EL_FIELD_LENS_FLARE_INTENSITY);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_SPREAD) == EL_FIELD_LENS_FLARE_SPREAD);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_GHOST_SPACING) == EL_FIELD_LENS_FLARE_GHOST_SPACING);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_GHOST_SIZE) == EL_FIELD_LENS_FLARE_GHOST_SIZE);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_GHOST_OFFSET) == EL_FIELD_LENS_FLARE_GHOST_OFFSET);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_GHOST_TINT) == EL_FIELD_LENS_FLARE_GHOST_TINT);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_RAY_DENSITY) == EL_FIELD_LENS_FLARE_RAY_DENSITY);
static_assert(static_cast<int>(EdgeLighting::AnimatableField::LENS_FLARE_ROTATION_RATE) == EL_FIELD_LENS_FLARE_ROTATION_RATE);

static_assert(static_cast<int>(EdgeLighting::SegmentField::POSITION) == EL_SEGMENT_FIELD_POSITION);
static_assert(static_cast<int>(EdgeLighting::SegmentField::LENGTH) == EL_SEGMENT_FIELD_LENGTH);
static_assert(static_cast<int>(EdgeLighting::SegmentField::BOOST) == EL_SEGMENT_FIELD_BOOST);

static_assert(static_cast<int>(EdgeLighting::ArcField::START) == EL_ARC_FIELD_START);
static_assert(static_cast<int>(EdgeLighting::ArcField::LENGTH) == EL_ARC_FIELD_LENGTH);
static_assert(static_cast<int>(EdgeLighting::ArcField::INTENSITY) == EL_ARC_FIELD_INTENSITY);

static_assert(static_cast<int>(EdgeLighting::ColorStopField::POSITION) == EL_STOP_FIELD_POSITION);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::R) == EL_STOP_FIELD_R);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::G) == EL_STOP_FIELD_G);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::B) == EL_STOP_FIELD_B);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::A) == EL_STOP_FIELD_A);

// PlaybackMode / EndAction / AnimationState use dedicated to*/from* helpers,
// so their ABI decoupling is enforced at the switch site rather than by
// parity - no static_asserts needed.

// ==========================================================================
// EL_THREADING_SPLIT machinery
//
// None of this is allocated in EL_THREADING_SINGLE mode - el_effect_handle_impl
// holds it behind a unique_ptr that stays null, so the historical path takes no
// mutex and reserves no queue. `docs/capi-threading-design.md` is the design;
// the short version is that el_effect_update is a GL phase (it uploads LUTs
// and VBOs through OnConfigChanged), so the boundary is NOT update/render. It
// is "who owns the staging Config" versus "who owns everything behind impl".
// ==========================================================================

namespace EdgeLighting
{
    namespace Capi
    {
        /// Per-animation state the render thread republishes each frame for
        /// the data thread to poll.
        ///
        /// The three fields are independent atomics, so a poll can see this
        /// frame's @c state against last frame's @c elapsed. That is deliberate
        /// and harmless for the progress-bar readout these exist to serve; a
        /// seqlock here would buy consistency nobody can perceive at the cost
        /// of a lock on the render thread's per-frame path.
        typedef struct AnimMirror
        {
            std::atomic<int32_t> state{EL_ANIM_STATE_STOPPED};
            std::atomic<float> elapsed{0.0f};
            std::atomic<float> progress{0.0f};

            /// Whether the render thread still owns this animation.
            ///
            /// This is an ACKNOWLEDGEMENT, and it exists because detach is
            /// asynchronous. @c el_effect_detach_animation only queues a
            /// DETACH: until the render thread drains it, the manager is
            /// still ticking the animation and every command queued ahead of
            /// the DETACH is still in flight. Treating the animation as the
            /// data thread's again the instant detach returns is a real data
            /// race - ThreadSanitizer catches it on the first inline Play
            /// that lands next to a draining SetElapsed.
            ///
            /// So: the DATA thread sets it true when it queues an ATTACH -
            /// covering the window before the ATTACH runs - and only the
            /// RENDER thread clears it, when it executes the matching DETACH
            /// or DETACH_ALL. Because the queue is FIFO, everything queued
            /// before that DETACH has already run by then. @ref splitOwner is
            /// the predicate that reads it.
            std::atomic<bool> attached{false};
        } AnimMirror;

        /// Every mutation that crosses from the data thread to a live
        /// @c Animation or to the effect's @c Clock / @c AnimationManager.
        typedef enum class CommandOp
        {
            PLAY,
            PAUSE,
            STOP,
            RESET,
            CAPTURE_BASELINE,
            SET_ELAPSED,
            SET_PROGRESS,
            SET_SPEED,
            SET_DURATION,
            SET_END_ACTION,
            SET_PLAYBACK_MODE,
            ATTACH,
            DETACH,
            DETACH_ALL,
            CLOCK_PLAY,
            CLOCK_PAUSE
        } CommandOp;

        /// One queued mutation.
        ///
        /// @c anim is an @c AnimationPtr and NOT an @c el_animation_handle_t,
        /// which is what makes @c el_animation_destroy safe to call from the
        /// data thread with commands still in flight: destroying the handle
        /// frees @c el_animation_handle_impl, but the @c Animation itself
        /// survives as long as any queued command references it.
        typedef struct Command
        {
            CommandOp op = CommandOp::PLAY;
            AnimationPtr anim;                  ///< Null for the effect-level ops.
            std::shared_ptr<AnimMirror> mirror; ///< ATTACH only.
            float f = 0.0f;
            int32_t i = 0;
        } Command;

        /// One host callback the render thread deferred to the data thread.
        /// Exactly one of the two function pointers is set.
        typedef struct PendingCallback
        {
            el_animation_on_completed_callback complete = nullptr;
            el_animation_on_state_changed_callback stateChanged = nullptr;
            void *userData = nullptr;
            el_animation_state_e previous = EL_ANIM_STATE_STOPPED;
            el_animation_state_e current = EL_ANIM_STATE_STOPPED;
        } PendingCallback;

        /// Everything shared between the two threads, plus the bookkeeping each
        /// keeps privately. Field comments name the owner; the ones with no
        /// owner named are the shared ones and are only touched under @c mutex.
        typedef struct ThreadedState
        {
            ThreadedState()
            {
                commands.reserve(EL_THREADED_QUEUE_CAPACITY);
                commandDrain.reserve(EL_THREADED_QUEUE_CAPACITY);
                callbacks.reserve(EL_THREADED_QUEUE_CAPACITY);
                callbackDrain.reserve(EL_THREADED_QUEUE_CAPACITY);
            }

            std::mutex mutex;

            // --- Config mailbox (data -> render) ---
            //
            // Two slots, a mutex, and a swap. NOT a lock-free triple buffer:
            // Config owns vectors, so a torn read is UB rather than a glitch,
            // and the lock is what makes the copy legal. The render thread
            // never holds the lock for a copy - only for the swap, which is
            // six vector pointer swaps - so it cannot be made to wait on the
            // data thread's work. Same reasoning, and the same idiom, as the
            // std::swap(mActiveConfig, mScratchConfig) in refreshActiveConfig.
            Config pending;      ///< Written by the data thread under the lock.
            Config consumed;     ///< RENDER THREAD, outside the lock, between swaps.
            bool hasPending = false;

            // --- Animation commands (data -> render) ---
            std::vector<Command> commands;
            std::vector<Command> commandDrain; ///< RENDER THREAD between swaps.

            // --- Deferred host callbacks (render -> data) ---
            std::vector<PendingCallback> callbacks;
            std::vector<PendingCallback> callbackDrain; ///< DATA THREAD between swaps.

            /// Set by el_effect_shutdown. Past it every publish and enqueue
            /// succeeds and does nothing, so a data thread racing the render
            /// thread's exit cannot wedge on a queue nobody will drain again.
            bool closed = false;

            // --- Mirrors published by the render thread ---
            std::atomic<bool> clockPlaying{false};

            /// The attached (animation, mirror) pairs, in attach order.
            /// RENDER THREAD ONLY - built by the ATTACH / DETACH commands.
            std::vector<std::pair<AnimationPtr, std::shared_ptr<AnimMirror>>> attached;

            /// Throwaway config for the RESET command. RENDER THREAD ONLY.
            ///
            /// Animation::Reset takes a mutable Config& and writes the
            /// modulator's t=0 value into it, which on the data thread means
            /// the caller's staging config. There is no mutable base config on
            /// this side, and for an ATTACHED animation that write is redundant
            /// anyway: the manager recomposites base + overlay every frame, so
            /// the t=0 value arrives through Apply regardless. What Reset is
            /// actually here for - zeroing elapsed and clearing mHasRun - lands
            /// on the animation itself either way.
            ///
            /// A member rather than a local so the assignment reuses the vector
            /// capacity already in it, the same reason EdgeLightingEffect keeps
            /// mScratchConfig.
            Config resetScratch;

            // --- Data-thread bookkeeping ---
            //
            // The data thread issued every attach command, so it can answer
            // el_effect_get_animation_count and el_effect_contains_animation
            // out of its own records rather than reaching into the manager.
            //
            // stagedAttached holds AnimationPtrs and outlives handle
            // destruction, so the count stays right when a host destroys a
            // handle without detaching (the manager keeps ticking it, exactly
            // as in single mode). stagedHandles holds only LIVE handles, so
            // DETACH_ALL can clear their stagedOwner back-pointers.
            std::vector<AnimationPtr> stagedAttached;
            std::vector<el_animation_handle_t> stagedHandles;
        } ThreadedState;

        /// The effect whose render-thread drain + Update is currently running,
        /// or null on any other thread and outside that window.
        ///
        /// This is how a host callback finds its way back to the data thread
        /// without the Animation needing to know which effect owns it. The
        /// lambdas installed by el_animation_set_on_*_callback read it when
        /// they fire: non-null means "we are inside the render thread's frame,
        /// defer"; null means "call directly", which is both the single-mode
        /// path and a manual el_animation_update on a detached animation.
        extern thread_local el_effect_handle_t gDispatchTarget;

    } // namespace Capi
} // namespace EdgeLighting

// ==========================================================================
// Opaque handle definitions
// ==========================================================================
struct el_effect_handle_impl
{
    EdgeLighting::Config config;
    std::unique_ptr<EdgeLighting::EdgeLightingEffect> impl;
    el_threading_mode_e mode = EL_THREADING_SINGLE;
    /// Non-null exactly when mode is EL_THREADING_SPLIT. Allocated by
    /// el_effect_set_threading_mode, which is why that call has to precede
    /// el_effect_init and is refused afterwards.
    std::unique_ptr<EdgeLighting::Capi::ThreadedState> threaded;
};

struct el_animation_handle_impl
{
    EdgeLighting::AnimationPtr ptr;
    /// Allocated on the first attach to a split-mode effect and shared with
    /// that effect's render-thread attach list, so the render thread can keep
    /// writing it after this handle is destroyed.
    std::shared_ptr<EdgeLighting::Capi::AnimMirror> mirror;
    /// The split-mode effect this handle was attached to, or null. DATA
    /// THREAD ONLY.
    ///
    /// Read it through @ref splitOwner, never directly: this field survives a
    /// detach on purpose, and it is that predicate which decides - against the
    /// render thread's acknowledgement - whether the animation is still the
    /// render thread's to touch.
    el_effect_handle_t stagedOwner = nullptr;

    /// Data-thread shadows of the four values the data thread is the only
    /// writer of. Seeded at attach - while the animation is provably not yet
    /// on the render thread's list - and updated by the setters that queue.
    ///
    /// Their reason to exist is that the render thread WRITES these when it
    /// drains a SET_SPEED / SET_DURATION / SET_END_ACTION / SET_PLAYBACK_MODE
    /// command, so reading them back off the live Animation would be a data
    /// race on a non-atomic float. The three genuinely live values (state,
    /// elapsed, progress) go the other way, through @ref AnimMirror.
    float shadowSpeed = 1.0f;
    float shadowDuration = 0.0f;
    int32_t shadowEndAction = EL_END_ACTION_HOLD_CURRENT;
    int32_t shadowPlaybackMode = EL_PLAYBACK_LOOP;
};

struct el_modulator_handle_impl
{
    EdgeLighting::ModulatorPtr ptr;
};

// ==========================================================================
// Split-mode helpers
// ==========================================================================

/// True when @p effect is running EL_THREADING_SPLIT. Asked through the
/// allocation rather than the enum so the two cannot disagree.
inline bool isSplit(el_effect_handle_t effect)
{
    return effect->threaded != nullptr;
}

/// Queue one mutation for the render thread to run at the top of its next
/// el_effect_update.
/// @returns EL_ERROR_OUT_OF_MEMORY when the queue is full - see
///          EL_THREADED_QUEUE_CAPACITY for why that is not a block.
inline el_result_e enqueueCommand(el_effect_handle_t effect,
                                  EdgeLighting::Capi::Command &&cmd)
{
    auto &st = *effect->threaded;
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.closed)
    {
        return EL_SUCCESS;
    }
    if (st.commands.size() >= EL_THREADED_QUEUE_CAPACITY)
    {
        LOG_E("command queue full (%d) - command dropped", EL_THREADED_QUEUE_CAPACITY);
        return EL_ERROR_OUT_OF_MEMORY;
    }
    st.commands.push_back(std::move(cmd));
    return EL_SUCCESS;
}

/// The effect whose queue this animation's mutations must still route
/// through, or null when the animation is genuinely the data thread's again.
///
/// Not simply @c anim->stagedOwner: that field is set at attach and survives
/// the detach, because detach is asynchronous (see @ref AnimMirror::attached).
/// It is cleared HERE, lazily, on the first call after the render thread has
/// acknowledged the detach - which is also the moment the animation becomes
/// safe to touch inline again.
///
/// DATA THREAD only, and non-const because of that lazy clear.
inline el_effect_handle_t splitOwner(el_animation_handle_t anim)
{
    if (!anim->stagedOwner)
    {
        return nullptr;
    }
    if (anim->mirror && anim->mirror->attached.load(std::memory_order_acquire))
    {
        return anim->stagedOwner;
    }
    anim->stagedOwner = nullptr;
    return nullptr;
}

/// Route an animation mutation to the render thread and RETURN, when the
/// animation is attached to a split-mode effect. Falls through to the caller's
/// inline path otherwise.
///
/// Place this before any idempotence guard the caller has: those guards read
/// the live Animation, which in split mode is the render thread's to touch.
#define QUEUE_IF_SPLIT(anim, opName, floatArg, intArg)                       \
    do                                                                       \
    {                                                                        \
        el_effect_handle_t routeTo = splitOwner(anim);                       \
        if (routeTo)                                                         \
        {                                                                    \
            EdgeLighting::Capi::Command queued;                              \
            queued.op = EdgeLighting::Capi::CommandOp::opName;               \
            queued.anim = (anim)->ptr;                                       \
            queued.f = (floatArg);                                           \
            queued.i = (intArg);                                             \
            return enqueueCommand(routeTo, std::move(queued));               \
        }                                                                    \
    } while (0)

/// QUEUE_IF_SPLIT for the four values the data thread shadows, where the
/// idempotence guard has to read the shadow rather than the live Animation.
///
/// The shadow only advances once the command is accepted, so a full queue
/// cannot leave the data thread reporting a value the render thread never got.
#define QUEUE_SHADOWED(anim, opName, floatArg, intArg, shadowField, shadowValue) \
    do                                                                           \
    {                                                                            \
        el_effect_handle_t routeTo = splitOwner(anim);                           \
        if (routeTo)                                                             \
        {                                                                        \
            if ((anim)->shadowField == (shadowValue))                            \
            {                                                                    \
                return EL_SUCCESS;                                               \
            }                                                                    \
            EdgeLighting::Capi::Command queued;                                  \
            queued.op = EdgeLighting::Capi::CommandOp::opName;                   \
            queued.anim = (anim)->ptr;                                           \
            queued.f = (floatArg);                                               \
            queued.i = (intArg);                                                 \
            const el_result_e queueResult =                                      \
                enqueueCommand(routeTo, std::move(queued));                      \
            if (queueResult == EL_SUCCESS)                                       \
            {                                                                    \
                (anim)->shadowField = (shadowValue);                             \
            }                                                                    \
            return queueResult;                                                  \
        }                                                                        \
    } while (0)

/// Queue one host callback for the data thread to run in its next
/// el_effect_poll_callbacks. Dropped silently when full: losing a completion
/// notification is bad, but running it on the GL thread - which is what the
/// alternative would be - is worse and is the trap this whole path exists to
/// close.
inline void enqueueCallback(el_effect_handle_t effect,
                            const EdgeLighting::Capi::PendingCallback &cb)
{
    auto &st = *effect->threaded;
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.closed || st.callbacks.size() >= EL_THREADED_QUEUE_CAPACITY)
    {
        return;
    }
    st.callbacks.push_back(cb);
}

// ==========================================================================
// Validation helpers
// ==========================================================================
#define VALIDATE_EFFECT_PTR(effect, fn)      \
    do                                       \
    {                                        \
        if (!(effect))                       \
        {                                    \
            LOG_E("%s: effect is null", fn); \
            return EL_ERROR_INVALID_HANDLE;  \
        }                                    \
    } while (0)

#define VALIDATE_ANIM_PTR(anim, fn)         \
    do                                      \
    {                                       \
        if (!(anim))                        \
        {                                   \
            LOG_E("%s: anim is null", fn);  \
            return EL_ERROR_INVALID_HANDLE; \
        }                                   \
    } while (0)

#define VALIDATE_MOD_PTR(mod, fn)           \
    do                                      \
    {                                       \
        if (!(mod))                         \
        {                                   \
            LOG_E("%s: mod is null", fn);   \
            return EL_ERROR_INVALID_HANDLE; \
        }                                   \
    } while (0)

#define VALIDATE_OUT_PTR(ptr, fn)                 \
    do                                            \
    {                                             \
        if (!(ptr))                               \
        {                                         \
            LOG_E("%s: out pointer is null", fn); \
            return EL_ERROR_INVALID_PARAMETER;    \
        }                                         \
    } while (0)

/// Short-circuit setter that only logs + assigns when the incoming value
/// actually differs from what @c field already holds, then returns @c EL_SUCCESS.
#define SET_AND_LOG(field, newVal, ...) \
    do                                  \
    {                                   \
        if ((field) == (newVal))        \
        {                               \
            return EL_SUCCESS;          \
        }                               \
        LOG_I(__VA_ARGS__);             \
        (field) = (newVal);             \
        return EL_SUCCESS;              \
    } while (0)

// ==========================================================================
// Enum conversion helpers
// ==========================================================================
inline el_result_e mapExceptionToResult(const std::exception &e)
{
    if (dynamic_cast<const std::bad_alloc *>(&e) != nullptr)
    {
        return EL_ERROR_OUT_OF_MEMORY;
    }
    return EL_ERROR_INVALID_PARAMETER;
}

inline EdgeLighting::EasingFunction::Curve toEasing(el_easing_e e)
{
    using namespace EdgeLighting;
    switch (e)
    {
    case EL_EASE_LINEAR:
        return EasingFunction::Linear;
    case EL_EASE_IN_QUAD:
        return EasingFunction::InQuad;
    case EL_EASE_OUT_QUAD:
        return EasingFunction::OutQuad;
    case EL_EASE_INOUT_QUAD:
        return EasingFunction::InOutQuad;
    case EL_EASE_IN_CUBIC:
        return EasingFunction::InCubic;
    case EL_EASE_OUT_CUBIC:
        return EasingFunction::OutCubic;
    case EL_EASE_INOUT_CUBIC:
        return EasingFunction::InOutCubic;
    case EL_EASE_IN_SINE:
        return EasingFunction::InSine;
    case EL_EASE_OUT_SINE:
        return EasingFunction::OutSine;
    case EL_EASE_INOUT_SINE:
        return EasingFunction::InOutSine;
    case EL_EASE_IN_EXPO:
        return EasingFunction::InExpo;
    case EL_EASE_OUT_EXPO:
        return EasingFunction::OutExpo;
    case EL_EASE_INOUT_EXPO:
        return EasingFunction::InOutExpo;
    default:
        return EasingFunction::Linear;
    }
}

inline EdgeLighting::Waveform toWaveform(el_waveform_e w)
{
    using namespace EdgeLighting;
    switch (w)
    {
    case EL_WAVE_TRIANGLE:
        return Waveform::TRIANGLE;
    case EL_WAVE_SQUARE:
        return Waveform::SQUARE;
    case EL_WAVE_SAWTOOTH:
        return Waveform::SAWTOOTH;
    case EL_WAVE_SINE:
    default:
        return Waveform::SINE;
    }
}

inline EdgeLighting::EndAction toEndAction(el_end_action_e a)
{
    using namespace EdgeLighting;
    switch (a)
    {
    case EL_END_ACTION_HOLD_END:
        return EndAction::HOLD_END;
    case EL_END_ACTION_HOLD_START:
        return EndAction::HOLD_START;
    case EL_END_ACTION_RESTORE:
        return EndAction::RESTORE;
    case EL_END_ACTION_HOLD_CURRENT:
    default:
        return EndAction::HOLD_CURRENT;
    }
}

inline el_end_action_e fromEndAction(EdgeLighting::EndAction a)
{
    using namespace EdgeLighting;
    switch (a)
    {
    case EndAction::HOLD_END:
        return EL_END_ACTION_HOLD_END;
    case EndAction::HOLD_START:
        return EL_END_ACTION_HOLD_START;
    case EndAction::RESTORE:
        return EL_END_ACTION_RESTORE;
    case EndAction::HOLD_CURRENT:
    default:
        return EL_END_ACTION_HOLD_CURRENT;
    }
}

inline EdgeLighting::PlaybackMode toPlaybackMode(el_playback_mode_e m)
{
    return m == EL_PLAYBACK_ONE_SHOT
               ? EdgeLighting::PlaybackMode::ONE_SHOT
               : EdgeLighting::PlaybackMode::LOOP;
}

inline el_playback_mode_e fromPlaybackMode(EdgeLighting::PlaybackMode m)
{
    return m == EdgeLighting::PlaybackMode::ONE_SHOT
               ? EL_PLAYBACK_ONE_SHOT
               : EL_PLAYBACK_LOOP;
}

inline EdgeLighting::AnimatableField toAnimatableField(el_config_field_e f)
{
    return static_cast<EdgeLighting::AnimatableField>(f);
}

inline el_animation_state_e fromAnimationState(EdgeLighting::AnimationState s)
{
    using ES = EdgeLighting::AnimationState;
    switch (s)
    {
    case ES::PLAYING:
        return EL_ANIM_STATE_PLAYING;
    case ES::PAUSED:
        return EL_ANIM_STATE_PAUSED;
    case ES::STOPPED:
    default:
        return EL_ANIM_STATE_STOPPED;
    }
}

#endif // _CAPI_INTERNAL_H_
