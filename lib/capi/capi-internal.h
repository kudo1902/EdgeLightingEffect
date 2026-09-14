#ifndef _CAPI_INTERNAL_H_
#define _CAPI_INTERNAL_H_

#include "edge-lighting-capi.h"
#include "adopted-lock.h"

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/neon-renderer.h"
#include "renderer/debug-renderer.h"
#include "renderer/droplets-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/field-access.h"
#include "animation/modulator.h"
#include "util/log-util.h"
#include "util/segment-utils.h"

#include <algorithm>
#include <atomic>
#include <cmath> // std::isfinite - VALIDATE_FINITE
#include <memory>
#include <mutex>
#include <thread>
#include <new>
#include <exception>

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

static_assert(static_cast<int>(EdgeLighting::OpaqueMode::NONE) == EL_OPAQUE_MODE_NONE);
static_assert(static_cast<int>(EdgeLighting::OpaqueMode::OUTSIDE) == EL_OPAQUE_MODE_OUTSIDE);
static_assert(static_cast<int>(EdgeLighting::OpaqueMode::INSIDE) == EL_OPAQUE_MODE_INSIDE);
static_assert(static_cast<int>(EdgeLighting::OpaqueMode::BOTH) == EL_OPAQUE_MODE_BOTH);
static_assert(static_cast<int>(EdgeLighting::OpaqueMode::ALL) == EL_OPAQUE_MODE_ALL);

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

// PlaybackMode / EndAction / AnimationState / easing curves are deliberately
// NOT parity-checked: they cross through the ConvertToCapi / ConvertFromCapi
// switches below, which decouple the two numberings entirely, so a reorder on
// either side is a compile error at the switch rather than a silent remap.
//
// That exemption is only worth anything while EVERY crossing goes through a
// switch. AnimationState once had one call site that did not - the
// OnStateChanged callback bridge cast raw - which left it with neither parity
// asserts nor decoupling. If you add a path for one of these enums, route it
// through the converter; do not cast.

// ==========================================================================
// Opaque handle definitions
// ==========================================================================
struct el_effect_handle_impl
{
    /// Guards everything on this handle EXCEPT the render path: @c config,
    /// @c impl (both the pointer and the effect's data side), and
    /// @c rendererMask. Taken through @c LOCK_EFFECT, which every effect entry
    /// point but one uses, immediately after @c VALIDATE_EFFECT_PTR.
    ///
    /// The one exception is @c el_effect_render, which simply does not call
    /// @c LOCK_EFFECT. That is the guarantee the whole threaded model rests on:
    /// a host's UI thread cannot stall a frame, whatever it does with this
    /// handle.
    ///
    /// RECURSIVE, for exactly one reason, and it is worth being precise about
    /// which - a recursive mutex is a real cost (holding it no longer implies
    /// the invariants hold, because you may be re-entering mid-mutation), so it
    /// should not be kept alive by reasons that turn out to be false.
    ///
    /// The reason: **host callbacks run with this held.** An animation's
    /// @c OnComplete / @c OnStateChanged fires synchronously from inside
    /// @c impl->Update, which is inside @c el_effect_update, which holds this.
    /// Anything the host calls from there re-locks on the same thread. That is
    /// not a hypothetical shape to tolerate but one the library deliberately
    /// supports: @c AnimationManager::Update walks a copy specifically so that
    /// "detach me when I finish" is safe to write in a callback, and the header
    /// docs tell hosts it is legal. A plain mutex would turn that documented,
    /// supported pattern into a hang.
    ///
    /// What is NOT a reason, checked rather than assumed, because both look
    /// like re-entry and neither is: @c el_effect_init delegates to
    /// @c el_effect_init_with_renderers WITHOUT locking first, and the
    /// el-deprecated.cpp shims that delegate to a modern setter do not lock
    /// either (the four that lock do their own work instead). The ABI does not
    /// re-enter itself anywhere. If the callback problem below is ever solved,
    /// nothing else here needs recursion.
    ///
    /// The better end state is to collect callbacks and fire them after
    /// unlocking, so a callback observes settled state rather than a
    /// half-applied frame. That needs a change to @c Animation's dispatch,
    /// which is why this is the contained choice for now and not the final one.
    ///
    /// HEAP-ALLOCATED AND SHARED, not a plain member, because an attached
    /// animation handle adopts it (see @c el_animation_handle_impl) and there
    /// is no way to hand it back on every path. @c el_effect_detach_all_animations
    /// detaches at the manager level and never sees the @c el_animation_handle_t
    /// values at all, so a handle can outlive its attachment still pointing
    /// here. As a member that would be a use-after-free on the next
    /// @c el_animation_* call after @c el_effect_destroy; as a shared_ptr the
    /// mutex simply outlives the effect until the last handle lets go. The cost
    /// is one allocation per effect and one indirection per lock.
    std::shared_ptr<std::recursive_mutex> dataMutex = std::make_shared<std::recursive_mutex>();

    EdgeLighting::Config config;
    std::unique_ptr<EdgeLighting::EdgeLightingEffect> impl;

    /// The mask the FIRST el_effect_init_with_renderers was given, kept so a
    /// later call can tell a GL rebuild (same mask, re-initialise in place)
    /// from a request to change the layer set (which needs a new effect and is
    /// refused). Only meaningful once @c impl exists.
    uint32_t rendererMask = 0;

    /// Which thread currently holds an open batch, or a default-constructed id
    /// when there is none.
    ///
    /// ATOMIC because @c el_effect_end_batch has to answer "do I own this
    /// scope?" BEFORE it can safely unlock, and at that moment it may own
    /// nothing at all - a host calling end without begin, or the wrong thread
    /// calling end. Reading @c batchDepth to find out would be the very race
    /// the answer is needed to avoid, and unlocking a @c recursive_mutex this
    /// thread does not hold is undefined behaviour, not an error code.
    /// @c std::thread::id is trivially copyable, so this is well formed, and it
    /// is lock-free on every target this ships to.
    std::atomic<std::thread::id> batchOwner{std::thread::id{}};

    /// Nesting depth of the open batch. Plain @c int, not atomic, and that is
    /// correct: it is only ever touched by a thread that has already proved it
    /// holds the lock, so the lock is its synchronisation.
    int batchDepth = 0;
};

struct el_animation_handle_impl
{
    EdgeLighting::AnimationPtr ptr;

    /// The effect lock this animation has ADOPTED, or null while detached.
    ///
    /// An attached animation is walked every frame by the effect's data side:
    /// @c AnimationManager::Update reads and writes its elapsed, state and
    /// playback mode, and @c Apply reads its bindings. Every @c el_animation_*
    /// call therefore has to exclude @c el_effect_update - and the only way to
    /// do that is to take the SAME mutex, not one of its own. Adoption is what
    /// makes that possible without the animation knowing what an effect is.
    ///
    /// One mutex, never two, which is the property that keeps this free of
    /// lock-ordering hazards: an @c el_animation_* call from a host callback
    /// fired inside @c el_effect_update re-enters the one lock already held,
    /// which is exactly the case @c dataMutex is recursive for.
    ///
    /// Set by @c el_effect_attach_animation and cleared by
    /// @c el_effect_detach_animation, both under that same lock. Two gaps the
    /// host has to respect, neither closable from here in C++17:
    ///   - @c el_effect_detach_all_animations cannot clear it (it never sees
    ///     handles), so a detached animation may keep locking a mutex nothing
    ///     else contends. Harmless, and the shared_ptr is what makes it so.
    ///   - writing this pointer is not atomic, so attach/detach on a handle
    ///     must not run concurrently with another call on that SAME handle.
    ///     Attach from the thread that owns the handle.
    std::shared_ptr<std::recursive_mutex> dataMutex;
};

struct el_modulator_handle_impl
{
    EdgeLighting::ModulatorPtr ptr;
};

// ==========================================================================
// Entry-point prologue: VALIDATION
//
// Every one of these can RETURN from the enclosing function, which is the whole
// point of the VALIDATE_ prefix in this file - reading one at a call site means
// "check this or bail with an error code".
//
// The prologue of an effect entry point is three steps in a fixed order, and
// the order is load-bearing rather than stylistic:
//
//     VALIDATE_EFFECT_PTR(effect, fn);   // 1. the handle is not null
//     LOCK_EFFECT(effect);               // 2. take the lock (see below)
//     VALIDATE_EFFECT_READY(effect, fn); // 3. ...and only now read impl
//
// Step 2 dereferences the handle, so it cannot precede step 1; step 3 reads
// `impl`, which the lock protects, so it cannot precede step 2. Checks that
// touch nothing on the handle (VALIDATE_OUT_PTR and friends) may go anywhere.
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

/// Reject a live handle that has not been through @c el_effect_init yet.
///
/// Use after @c LOCK_EFFECT in any entry point that dereferences @c impl - the
/// lock is what makes reading @c impl sound in the first place. The
/// effect object does not exist until init builds it, and every one of these
/// calls used to dereference the null @c unique_ptr and crash the host -
/// update, render, capture, all four clock calls and all five animation calls.
/// The @c el_effect_read_* family was already careful here, through
/// @c ResolveConfigSource; this is the same guarantee for everyone else.
///
/// EL_ERROR_INVALID_HANDLE rather than EL_ERROR_INVALID_PARAMETER: no argument
/// is at fault, the handle is simply not in a state where the call means
/// anything. Nothing depended on the old behaviour, which was a crash.
#define VALIDATE_EFFECT_READY(effect, fn)              \
    do                                                 \
    {                                                  \
        if (!(effect)->impl)                           \
        {                                              \
            LOG_E("%s: effect not initialised - call " \
                  "el_effect_init first",              \
                  fn);                                 \
            return EL_ERROR_INVALID_HANDLE;            \
        }                                              \
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

/// Reject a NaN or an infinity before it can reach a @c Config field.
///
/// Every float this ABI accepts goes through here, and the reason is not that
/// a bad float draws badly - it is that a NaN is not a LOCAL problem. Once one
/// is in the config, @c NaN @c != @c NaN makes that config unequal to ITSELF
/// forever, so @c EdgeLightingEffect::refreshActiveConfig reports a change on
/// every frame for the rest of the process: measured, 120 of 120 settled
/// frames against 0 of 120 healthy. Every renderer's @c OnConfigChanged then
/// fires per frame, which among other things sets @c mEmissionDirty and
/// re-bakes the neon emission table every frame - the exact per-frame cost
/// docs/emission-prepass.md exists to remove. One bad float silently undoes it,
/// with nothing in the log. See review-findings I40.
///
/// Infinities are rejected alongside NaN even though @c inf @c == @c inf and so
/// does NOT break the comparison. They are refused because nothing downstream
/// has a meaning for them - an infinite glow radius or corner radius is not a
/// value the shaders can do anything sane with - and because a host that
/// computed one has a bug it is better off hearing about. It also keeps the
/// rule to one word a caller can remember: finite, or an error.
///
/// Deliberately NOT a clamp. There is no defensible finite value to substitute
/// (what is a "reasonable" replacement for a NaN line width?), and silently
/// swapping one in is how a host's arithmetic bug becomes a rendering mystery
/// instead of a return code at the call site that produced it.
#define VALIDATE_FINITE(v, fn)                                    \
    do                                                            \
    {                                                             \
        if (!std::isfinite(v))                                    \
        {                                                         \
            LOG_E("%s: non-finite value %f - rejected", fn, (v)); \
            return EL_ERROR_INVALID_PARAMETER;                    \
        }                                                         \
    } while (0)

/// @ref VALIDATE_FINITE for the colour and vector setters, so a four-component
/// call does not need four lines of prologue.
#define VALIDATE_FINITE2(a, b, fn) \
    do                             \
    {                              \
        VALIDATE_FINITE(a, fn);    \
        VALIDATE_FINITE(b, fn);    \
    } while (0)

#define VALIDATE_FINITE3(a, b, c, fn) \
    do                                \
    {                                 \
        VALIDATE_FINITE(a, fn);       \
        VALIDATE_FINITE(b, fn);       \
        VALIDATE_FINITE(c, fn);       \
    } while (0)

#define VALIDATE_FINITE4(a, b, c, d, fn) \
    do                                   \
    {                                    \
        VALIDATE_FINITE(a, fn);          \
        VALIDATE_FINITE(b, fn);          \
        VALIDATE_FINITE(c, fn);          \
        VALIDATE_FINITE(d, fn);          \
    } while (0)

/// Refuse a negative value on a parameter documented as a positive DISTANCE.
///
/// Narrower than @ref VALIDATE_FINITE by design: it guards the few floats a
/// negative silently corrupts GEOMETRY with, not every float in the ABI. The
/// cutoff sizes are those - they reach @c NeonRenderer::setupFillGeometry's
/// margins and land in vertex data, where a negative inverts the ring (its
/// outer edge ends up inside its own hole) while the shader's own boundary
/// test flips sign and returns coverage 0. The fill then vanishes with no
/// error anywhere, which is the worst of both: wrong picture, silent API.
///
/// Note what does NOT need this and why, so nobody adds it there: the
/// SOFTNESS fields are already floored downstream - neon.frag takes
/// max(softness, softFloor) and renderOpaqueFill takes max(opaqueSoftness,
/// SIDE_SOFT_EPSILON) - so a negative is absorbed before it can reach
/// geometry. Guarding them too would only turn a harmless value into an
/// error.
///
/// Rejects rather than clamps, for @ref VALIDATE_FINITE's reason plus one of
/// its own: every el_effect_set_* here round-trips through its getter, and a
/// clamp would hand the host back a value it never set.
#define VALIDATE_NON_NEGATIVE(v, fn)                                     \
    do                                                                   \
    {                                                                    \
        if ((v) < 0.0f)                                                  \
        {                                                                \
            LOG_E("%s: negative distance %f - rejected", fn, (v));       \
            return EL_ERROR_INVALID_PARAMETER;                           \
        }                                                                \
    } while (0)

/// @ref VALIDATE_FINITE for the entry points that return a HANDLE rather than
/// an @c el_result_e - the animation and modulator factories. Same rule, same
/// reasoning; only the failure value differs, because @c nullptr is the only
/// thing those signatures can say "no" with.
///
/// A rejected factory therefore looks exactly like an allocation failure to the
/// caller, which the factories already document: they return @c nullptr and log
/// the reason. The log line is what tells the two apart.
#define VALIDATE_FINITE_H(v, fn)                                  \
    do                                                            \
    {                                                             \
        if (!std::isfinite(v))                                    \
        {                                                         \
            LOG_E("%s: non-finite value %f - rejected", fn, (v)); \
            return nullptr;                                       \
        }                                                         \
    } while (0)

#define VALIDATE_FINITE_H2(a, b, fn) \
    do                               \
    {                                \
        VALIDATE_FINITE_H(a, fn);    \
        VALIDATE_FINITE_H(b, fn);    \
    } while (0)

#define VALIDATE_FINITE_H3(a, b, c, fn) \
    do                                  \
    {                                   \
        VALIDATE_FINITE_H(a, fn);       \
        VALIDATE_FINITE_H(b, fn);       \
        VALIDATE_FINITE_H(c, fn);       \
    } while (0)

#define VALIDATE_FINITE_H4(a, b, c, d, fn) \
    do                                     \
    {                                      \
        VALIDATE_FINITE_H(a, fn);          \
        VALIDATE_FINITE_H(b, fn);          \
        VALIDATE_FINITE_H(c, fn);          \
        VALIDATE_FINITE_H(d, fn);          \
    } while (0)

/// Companion to @c ResolveConfigSource: bail out when it could not resolve.
/// Covers both of its failure modes, which are the same class of caller error -
/// an enum value the ABI does not define, or BASE/ACTIVE asked of a handle that
/// has no effect behind it yet.
#define VALIDATE_SOURCE(cfgPtr, source, fn)                                      \
    do                                                                           \
    {                                                                            \
        if (!(cfgPtr))                                                           \
        {                                                                        \
            LOG_E("%s: unknown config source %d, or effect not initialised", fn, \
                  (int)(source));                                                \
            return EL_ERROR_INVALID_PARAMETER;                                   \
        }                                                                        \
    } while (0)

// ==========================================================================
// Entry-point prologue: LOCKING
//
// Kept apart from the validators above because they are a different kind of
// thing: a validator RETURNS, a lock DECLARES a guard that lives to the closing
// brace. Neither is a single statement, so both belong at the top of a function
// body and never under an unbraced `if`.
//
// The animation prologue mirrors the effect one:
//
//     VALIDATE_ANIM_PTR(anim, fn);       // 1. the handle is not null
//     LOCK_ANIMATION(anim);              // 2. take whatever lock it adopted
//                                        // 3. ...and only now read anim->ptr
//
// Two entry points take no lock and both omissions are deliberate:
// el_effect_render, which is the design, and el_effect_destroy, which cannot
// lock a mutex it is about to destroy. See LOCK_EFFECT.
// ==========================================================================
/// Hold the handle's data lock for the rest of the enclosing scope.
///
/// Two entry points deliberately omit it:
///   - @c el_effect_render, which is the whole design rather than an oversight.
///     The render path touches only the renderers and one atomic snapshot cell,
///     so no host thread can ever stall a frame.
///   - @c el_effect_destroy, for the unrelated reason that it cannot lock a
///     mutex it is about to destroy. The host must quiesce its data thread
///     first, and no lock here could help with that.
#define LOCK_EFFECT(effect) \
    std::lock_guard<std::recursive_mutex> elDataLock(*(effect)->dataMutex)

/// Take the effect lock this animation has ADOPTED, for the rest of the scope.
///
/// A no-op while the animation is detached, which is the documented ownership
/// model: a detached animation belongs to whoever built it. See
/// @c el_animation_handle_impl for why it is the effect's lock and not one of
/// the animation's own, and @c AdoptedLock for why this needs a guard of its
/// own rather than @c std::lock_guard.
#define LOCK_ANIMATION(anim) \
    AdoptedLock elAnimLock((anim)->dataMutex)

// ==========================================================================
// Setter helper
// ==========================================================================
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
//
// Everything at this seam exists twice: once as an ABI enum and once as the
// library's own. @c ConvertFromCapi / @c ConvertToCapi are the two directions,
// overloaded on the argument type rather than spelled out per type - the axis
// is named once instead of seven times, and the direction is absolute rather
// than relative to which type the name happens to mention.
//
// The overloads are grouped by TYPE below, not by direction, so a pair sits
// together and it is obvious at a glance which types round-trip and which are
// one-way. Adding a type means adding its overload(s) beside the others.
//
// Overload resolution is safe here because every argument type is distinct:
// the C side are separate unscoped enums (which convert to int, never to each
// other) and the C++ side are all @c enum @c class. An untyped literal would be
// ambiguous, and that is a compile error rather than a wrong pick.
//
// @c MapExceptionToResult is deliberately NOT part of this: it is a genuine
// many-to-one classification, not a change of representation, so it keeps its
// own verb.
// ==========================================================================
inline el_result_e MapExceptionToResult(const std::exception &e)
{
    if (dynamic_cast<const std::bad_alloc *>(&e) != nullptr)
    {
        return EL_ERROR_OUT_OF_MEMORY;
    }
    return EL_ERROR_INVALID_PARAMETER;
}

inline EdgeLighting::EasingFunction::Curve ConvertFromCapi(el_easing_e e)
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

inline EdgeLighting::Waveform ConvertFromCapi(el_waveform_e w)
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

inline EdgeLighting::EndAction ConvertFromCapi(el_end_action_e a)
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
    case EL_END_ACTION_HOLD_NONE:
        return EndAction::HOLD_NONE;
    case EL_END_ACTION_HOLD_CURRENT:
    default:
        return EndAction::HOLD_CURRENT;
    }
}

inline el_end_action_e ConvertToCapi(EdgeLighting::EndAction a)
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
    case EndAction::HOLD_NONE:
        return EL_END_ACTION_HOLD_NONE;
    case EndAction::HOLD_CURRENT:
    default:
        return EL_END_ACTION_HOLD_CURRENT;
    }
}

inline EdgeLighting::PlaybackMode ConvertFromCapi(el_playback_mode_e m)
{
    return m == EL_PLAYBACK_ONE_SHOT
               ? EdgeLighting::PlaybackMode::ONE_SHOT
               : EdgeLighting::PlaybackMode::LOOP;
}

inline el_animation_state_e ConvertToCapi(EdgeLighting::AnimationState s)
{
    switch (s)
    {
    case EdgeLighting::AnimationState::PLAYING:
    {
        return EL_ANIM_STATE_PLAYING;
    }
    case EdgeLighting::AnimationState::PAUSED:
    {
        return EL_ANIM_STATE_PAUSED;
    }
    case EdgeLighting::AnimationState::STOPPED:
    default:
    {
        return EL_ANIM_STATE_STOPPED;
    }
    }
}

inline el_playback_mode_e ConvertToCapi(EdgeLighting::PlaybackMode m)
{
    return m == EdgeLighting::PlaybackMode::ONE_SHOT
               ? EL_PLAYBACK_ONE_SHOT
               : EL_PLAYBACK_LOOP;
}

inline EdgeLighting::AnimatableField ConvertFromCapi(el_config_field_e f)
{
    return static_cast<EdgeLighting::AnimatableField>(f);
}

/// The one place that knows which storage each @ref el_config_source_e names.
/// Returns nullptr for an unknown source, and for BASE / ACTIVE on a handle
/// that has not been through @c el_effect_init - those two live in the effect,
/// which does not exist yet. STAGING is always available: it is the handle's
/// own member, filled with defaults from the moment @c el_effect_create
/// returns.
inline const EdgeLighting::Config *ResolveConfigSource(el_effect_handle_t effect,
                                                       el_config_source_e source)
{
    switch (source)
    {
    case EL_CONFIG_SOURCE_STAGING:
    {
        return &effect->config;
    }
    case EL_CONFIG_SOURCE_BASE:
    {
        return effect->impl ? &effect->impl->GetConfig() : nullptr;
    }
    case EL_CONFIG_SOURCE_ACTIVE:
    {
        return effect->impl ? &effect->impl->GetActiveConfig() : nullptr;
    }
    }
    return nullptr;
}

#endif // _CAPI_INTERNAL_H_
