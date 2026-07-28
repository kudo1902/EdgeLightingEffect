#ifndef _CAPI_INTERNAL_H_
#define _CAPI_INTERNAL_H_

#include "edge-lighting-capi.h"

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/neon-optimized-renderer.h"
#include "renderer/droplets-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/modulator.h"
#include "util/log-util.h"
#include "util/segment-utils.h"

#include <algorithm>
#include <memory>
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
// Opaque handle definitions
// ==========================================================================
struct el_effect_handle_impl
{
    EdgeLighting::Config config;
    std::unique_ptr<EdgeLighting::EdgeLightingEffect> impl;
};

struct el_animation_handle_impl
{
    EdgeLighting::AnimationPtr ptr;
};

struct el_modulator_handle_impl
{
    EdgeLighting::ModulatorPtr ptr;
};

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
namespace capi
{

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

} // namespace capi

#endif // _CAPI_INTERNAL_H_
