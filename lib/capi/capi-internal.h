#ifndef _CAPI_INTERNAL_H_
#define _CAPI_INTERNAL_H_

#include "edge-lighting-capi.h"

#include <cmath>
#include <initializer_list>

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/neon-renderer.h"
#include "renderer/debug-renderer.h"
#include "renderer/droplets-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "renderer/spotlight-renderer.h"
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

static_assert(static_cast<int>(EdgeLighting::SpotlightField::POSITION_X) == EL_SPOTLIGHT_FIELD_POSITION_X);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::POSITION_Y) == EL_SPOTLIGHT_FIELD_POSITION_Y);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::ANGLE) == EL_SPOTLIGHT_FIELD_ANGLE);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::BEAM_ANGLE) == EL_SPOTLIGHT_FIELD_BEAM_ANGLE);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::THROW_LENGTH) == EL_SPOTLIGHT_FIELD_THROW_LENGTH);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::APERTURE_WIDTH) == EL_SPOTLIGHT_FIELD_APERTURE_WIDTH);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::SOFTNESS) == EL_SPOTLIGHT_FIELD_SOFTNESS);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::INTENSITY) == EL_SPOTLIGHT_FIELD_INTENSITY);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::BLOOM) == EL_SPOTLIGHT_FIELD_BLOOM);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::BLOOM_RADIUS) == EL_SPOTLIGHT_FIELD_BLOOM_RADIUS);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::COLOR_TEMP) == EL_SPOTLIGHT_FIELD_COLOR_TEMP);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::TINT_R) == EL_SPOTLIGHT_FIELD_TINT_R);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::TINT_G) == EL_SPOTLIGHT_FIELD_TINT_G);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::TINT_B) == EL_SPOTLIGHT_FIELD_TINT_B);
static_assert(static_cast<int>(EdgeLighting::SpotlightField::SPREAD_FALLOFF) == EL_SPOTLIGHT_FIELD_SPREAD_FALLOFF);

static_assert(static_cast<int>(EdgeLighting::ClipMode::KEEP_INSIDE) == EL_CLIP_KEEP_INSIDE);
static_assert(static_cast<int>(EdgeLighting::ClipMode::KEEP_OUTSIDE) == EL_CLIP_KEEP_OUTSIDE);

static_assert(static_cast<int>(EdgeLighting::ColorStopField::POSITION) == EL_STOP_FIELD_POSITION);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::R) == EL_STOP_FIELD_R);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::G) == EL_STOP_FIELD_G);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::B) == EL_STOP_FIELD_B);
static_assert(static_cast<int>(EdgeLighting::ColorStopField::A) == EL_STOP_FIELD_A);

// el_renderer_flags_e's layer bits ARE RendererLayer's values, so a flag
// converts to a layer by a cast once it is known to name exactly one layer.
static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::NEON) == EL_RENDERER_NEON);
static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::DROPLETS) == EL_RENDERER_DROPLETS);
static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::LENS_FLARE) == EL_RENDERER_LENS_FLARE);
static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::SPOTLIGHT) == EL_RENDERER_SPOTLIGHT);
static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::DEBUG) == EL_RENDERER_DEBUG);

// PlaybackMode / EndAction / AnimationState use dedicated to*/from* helpers,
// so their ABI decoupling is enforced at the switch site rather than by
// parity - no static_asserts needed. The helpers' switches name every
// enumerator of both sides (the default arm only catches foreign integers).

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

/// Rejects a foreign integer that names no enumerator of @c type (one of the
/// isValid* predicates below). A C caller can pass any int through an enum
/// parameter, and a static_cast to the C++ enum would store it unchecked.
#define VALIDATE_ENUM(type, value, fn)                                 \
    do                                                                 \
    {                                                                  \
        if (!isValid(static_cast<type>(value)))                        \
        {                                                              \
            LOG_E("%s: invalid %s value %d", fn, #type, (int)(value)); \
            return EL_ERROR_INVALID_PARAMETER;                         \
        }                                                              \
    } while (0)

/// Rejects a negative index before it is cast to size_t.
#define VALIDATE_INDEX(index, fn)                 \
    do                                            \
    {                                             \
        if ((index) < 0)                          \
        {                                         \
            LOG_E("%s: negative %s", fn, #index); \
            return EL_ERROR_INVALID_PARAMETER;    \
        }                                         \
    } while (0)

/// Whether every value in @p values is finite. One NaN in the config makes
/// Config::operator== false for good, so the effect notifies every renderer on
/// every frame and every skip they keep is defeated (I80).
inline bool AllFinite(std::initializer_list<double> values)
{
    for (const double v : values)
    {
        if (!std::isfinite(v))
        {
            return false;
        }
    }
    return true;
}

/// Rejects a call any of whose float arguments is NaN or infinite, returning
/// @p failure (EL_ERROR_INVALID_PARAMETER, or nullptr from a factory).
#define REQUIRE_FINITE(fn, failure, ...)                             \
    do                                                               \
    {                                                                \
        if (!AllFinite({__VA_ARGS__}))                               \
        {                                                            \
            LOG_E("%s: non-finite argument (%s)", fn, #__VA_ARGS__); \
            return failure;                                          \
        }                                                            \
    } while (0)

/// Rejects a duration below @p minimum, or equal to it when @p strict.
#define REQUIRE_DURATION(fn, failure, duration, strict)                   \
    do                                                                    \
    {                                                                     \
        if ((strict) ? !((duration) > 0.0f) : !((duration) >= 0.0f))      \
        {                                                                 \
            LOG_E("%s: duration %f must be %s 0", fn, (double)(duration), \
                  (strict) ? "above" : "at least");                       \
            return failure;                                               \
        }                                                                 \
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
// Enum range predicates
//
// One per mirrored enum a setter or binder stores through static_cast. Each
// switch names every enumerator and has no default, so -Wswitch flags a new
// value that was not added here; the parity asserts above pin the numbers.
// ==========================================================================
inline bool isValid(el_winding_e v)
{
    switch (v)
    {
    case EL_WINDING_CLOCKWISE:
    case EL_WINDING_COUNTER_CLOCKWISE:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_glow_side_e v)
{
    switch (v)
    {
    case EL_GLOW_SIDE_BOTH:
    case EL_GLOW_SIDE_INSIDE:
    case EL_GLOW_SIDE_OUTSIDE:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_opaque_mode_e v)
{
    switch (v)
    {
    case EL_OPAQUE_MODE_NONE:
    case EL_OPAQUE_MODE_OUTSIDE:
    case EL_OPAQUE_MODE_INSIDE:
    case EL_OPAQUE_MODE_BOTH:
    case EL_OPAQUE_MODE_ALL:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_blend_space_e v)
{
    switch (v)
    {
    case EL_BLEND_SPACE_RGB:
    case EL_BLEND_SPACE_HSV:
    case EL_BLEND_SPACE_HSL:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_clip_mode_e v)
{
    switch (v)
    {
    case EL_CLIP_KEEP_INSIDE:
    case EL_CLIP_KEEP_OUTSIDE:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_config_field_e v)
{
    switch (v)
    {
    case EL_FIELD_NEON_INTENSITY:
    case EL_FIELD_NEON_LINE_WIDTH:
    case EL_FIELD_NEON_GLOW_RADIUS:
    case EL_FIELD_NEON_BLOOM_STRENGTH:
    case EL_FIELD_NEON_FILAMENT_FALLOFF:
    case EL_FIELD_NEON_GLOW_SIDE_SOFTNESS:
    case EL_FIELD_NEON_HUE_ROTATION_RATE:
    case EL_FIELD_LENS_FLARE_PERIMETER_POSITION:
    case EL_FIELD_LENS_FLARE_PERIMETER_OFFSET:
    case EL_FIELD_LENS_FLARE_SIZE:
    case EL_FIELD_LENS_FLARE_INTENSITY:
    case EL_FIELD_LENS_FLARE_SPREAD:
    case EL_FIELD_LENS_FLARE_GHOST_SPACING:
    case EL_FIELD_LENS_FLARE_GHOST_SIZE:
    case EL_FIELD_LENS_FLARE_GHOST_OFFSET:
    case EL_FIELD_LENS_FLARE_GHOST_TINT:
    case EL_FIELD_LENS_FLARE_RAY_DENSITY:
    case EL_FIELD_LENS_FLARE_ROTATION_RATE:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_segment_field_e v)
{
    switch (v)
    {
    case EL_SEGMENT_FIELD_POSITION:
    case EL_SEGMENT_FIELD_LENGTH:
    case EL_SEGMENT_FIELD_BOOST:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_arc_field_e v)
{
    switch (v)
    {
    case EL_ARC_FIELD_START:
    case EL_ARC_FIELD_LENGTH:
    case EL_ARC_FIELD_INTENSITY:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_spotlight_field_e v)
{
    switch (v)
    {
    case EL_SPOTLIGHT_FIELD_POSITION_X:
    case EL_SPOTLIGHT_FIELD_POSITION_Y:
    case EL_SPOTLIGHT_FIELD_ANGLE:
    case EL_SPOTLIGHT_FIELD_BEAM_ANGLE:
    case EL_SPOTLIGHT_FIELD_THROW_LENGTH:
    case EL_SPOTLIGHT_FIELD_APERTURE_WIDTH:
    case EL_SPOTLIGHT_FIELD_SOFTNESS:
    case EL_SPOTLIGHT_FIELD_INTENSITY:
    case EL_SPOTLIGHT_FIELD_BLOOM:
    case EL_SPOTLIGHT_FIELD_BLOOM_RADIUS:
    case EL_SPOTLIGHT_FIELD_COLOR_TEMP:
    case EL_SPOTLIGHT_FIELD_TINT_R:
    case EL_SPOTLIGHT_FIELD_TINT_G:
    case EL_SPOTLIGHT_FIELD_TINT_B:
    case EL_SPOTLIGHT_FIELD_SPREAD_FALLOFF:
    {
        return true;
    }
    }
    return false;
}

inline bool isValid(el_color_stop_field_e v)
{
    switch (v)
    {
    case EL_STOP_FIELD_POSITION:
    case EL_STOP_FIELD_R:
    case EL_STOP_FIELD_G:
    case EL_STOP_FIELD_B:
    case EL_STOP_FIELD_A:
    {
        return true;
    }
    }
    return false;
}

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

#endif // _CAPI_INTERNAL_H_
