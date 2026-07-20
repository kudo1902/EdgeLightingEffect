#include "edge-lighting-capi.h"

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/neon-optimized-renderer.h"
#include "renderer/droplets-renderer.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/modulator.h"
#include "util/log-util.h"

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
// Per-thread error state
// ==========================================================================
namespace
{

// ==========================================================================
// Validation helpers
// ==========================================================================
// Handles map to EL_ERROR_INVALID_HANDLE; out pointers (VALIDATE_OUT_PTR
// below) are non-handle args and map to EL_ERROR_INVALID_PARAMETER.
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

    /// Map a caught @c std::exception to the closest @c el_result_e. Allocation
    /// failures get @ref EL_ERROR_OUT_OF_MEMORY (both @c std::bad_alloc and
    /// @c std::bad_array_new_length derive from it); every other exception is a
    /// programmer bug at the ABI boundary and lands in @ref EL_ERROR_INVALID_PARAMETER.
    static inline el_result_e mapExceptionToResult(const std::exception &e)
    {
        if (dynamic_cast<const std::bad_alloc *>(&e) != nullptr)
        {
            return EL_ERROR_OUT_OF_MEMORY;
        }
        return EL_ERROR_INVALID_PARAMETER;
    }

/// Short-circuit setter that only logs + assigns when the incoming value
/// actually differs from what @c field already holds, then returns @c EL_SUCCESS.
/// Use inside the setter body AFTER @c VALIDATE_EFFECT_PTR. Cuts log spam and
/// downstream @c OnConfigChanged churn when a UI slider fires a setter every
/// frame with the same value.
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
    EdgeLighting::EasingFunction::Curve toEasing(el_easing_e e)
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

    EdgeLighting::Waveform toWaveform(el_waveform_e w)
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

    EdgeLighting::EndAction toEndAction(el_end_action_e a)
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

    el_end_action_e fromEndAction(EdgeLighting::EndAction a)
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

    EdgeLighting::PlaybackMode toPlaybackMode(el_playback_mode_e m)
    {
        return m == EL_PLAYBACK_ONE_SHOT
                   ? EdgeLighting::PlaybackMode::ONE_SHOT
                   : EdgeLighting::PlaybackMode::LOOP;
    }

    el_playback_mode_e fromPlaybackMode(EdgeLighting::PlaybackMode m)
    {
        return m == EdgeLighting::PlaybackMode::ONE_SHOT
                   ? EL_PLAYBACK_ONE_SHOT
                   : EL_PLAYBACK_LOOP;
    }

    EdgeLighting::AnimatableField toAnimatableField(el_config_field_e f)
    {
        return static_cast<EdgeLighting::AnimatableField>(f);
    }

} // anonymous namespace

// ==========================================================================
// Library-level
// ==========================================================================
extern "C"
{

    // ==========================================================================
    // Effect - config setters
    // ==========================================================================

    // --- Geometry ---

    el_result_e el_effect_set_geometry(el_effect_handle_t effect,
                                       float width, float height, float posX, float posY, float cornerRadius)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_geometry");
        auto &g = effect->config.geometry;
        glm::vec2 pos(posX, posY);
        if (g.width == width && g.height == height && g.position == pos && g.cornerRadius == cornerRadius)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, width=%f, height=%f, posX=%f, posY=%f, cornerRadius=%f", (void *)effect, width, height, posX, posY, cornerRadius);
        g.width = width;
        g.height = height;
        g.position = pos;
        g.cornerRadius = cornerRadius;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_geometry(el_effect_handle_t effect,
                                       float *outWidth, float *outHeight, float *outPosX, float *outPosY,
                                       float *outCornerRadius)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outWidth, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outHeight, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outPosX, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outPosY, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outCornerRadius, "el_effect_get_geometry");
        *outWidth = effect->config.geometry.width;
        *outHeight = effect->config.geometry.height;
        *outPosX = effect->config.geometry.position.x;
        *outPosY = effect->config.geometry.position.y;
        *outCornerRadius = effect->config.geometry.cornerRadius;
        LOG_D("effect=%p, width=%f, height=%f, posX=%f, posY=%f, cornerRadius=%f", (void *)effect, *outWidth, *outHeight, *outPosX, *outPosY, *outCornerRadius);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_winding(el_effect_handle_t effect, el_winding_e winding)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_winding");
        SET_AND_LOG(effect->config.geometry.winding, static_cast<EdgeLighting::Winding>(winding),
                    "effect=%p, winding=%d", (void *)effect, (int)winding);
    }

    el_result_e el_effect_get_winding(el_effect_handle_t effect, el_winding_e *outWinding)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_winding");
        VALIDATE_OUT_PTR(outWinding, "el_effect_get_winding");
        *outWinding = static_cast<el_winding_e>(effect->config.geometry.winding);
        LOG_D("effect=%p, winding=%d", (void *)effect, (int)*outWinding);
        return EL_SUCCESS;
    }

    // --- Neon scalars ---

    el_result_e el_effect_set_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_neon_renderer_enabled");
        SET_AND_LOG(effect->config.neon.enable, enabled != 0,
                    "effect=%p, enabled=%d", (void *)effect, enabled);
    }

    el_result_e el_effect_get_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_neon_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_neon_renderer_enabled");
        *outEnabled = effect->config.neon.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_show_gradient_lut");
        SET_AND_LOG(effect->config.neon.showGradientLUT, show != 0,
                    "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_show_gradient_lut");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_show_gradient_lut");
        *outShow = effect->config.neon.showGradientLUT ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_show_color_stops(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_show_color_stops");
        SET_AND_LOG(effect->config.neon.showColorStops, show != 0,
                    "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_show_color_stops");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_show_color_stops");
        *outShow = effect->config.neon.showColorStops ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_opaque(el_effect_handle_t effect, el_bool_t opaque)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_opaque");
        SET_AND_LOG(effect->config.neon.opaque, opaque != 0,
                    "effect=%p, opaque=%d", (void *)effect, opaque);
    }

    el_result_e el_effect_get_opaque(el_effect_handle_t effect, el_bool_t *outOpaque)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_opaque");
        VALIDATE_OUT_PTR(outOpaque, "el_effect_get_opaque");
        *outOpaque = effect->config.neon.opaque ? 1 : 0;
        LOG_D("effect=%p, opaque=%d", (void *)effect, *outOpaque);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_opaque_color(el_effect_handle_t effect,
                                           float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_opaque_color");
        SET_AND_LOG(effect->config.neon.opaqueColor, glm::vec4(r, g, b, a),
                    "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }

    el_result_e el_effect_get_opaque_color(el_effect_handle_t effect,
                                           float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_opaque_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_opaque_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_opaque_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_opaque_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_opaque_color");
        *outR = effect->config.neon.opaqueColor.r;
        *outG = effect->config.neon.opaqueColor.g;
        *outB = effect->config.neon.opaqueColor.b;
        *outA = effect->config.neon.opaqueColor.a;
        LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_line_width(el_effect_handle_t effect, float width)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_line_width");
        SET_AND_LOG(effect->config.neon.lineWidth, width,
                    "effect=%p, width=%f", (void *)effect, width);
    }

    el_result_e el_effect_get_line_width(el_effect_handle_t effect, float *outWidth)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_line_width");
        VALIDATE_OUT_PTR(outWidth, "el_effect_get_line_width");
        *outWidth = effect->config.neon.lineWidth;
        LOG_D("effect=%p, width=%f", (void *)effect, *outWidth);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_filament_falloff(el_effect_handle_t effect, float falloff)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_filament_falloff");
        SET_AND_LOG(effect->config.neon.filamentFalloff, falloff,
                    "effect=%p, falloff=%f", (void *)effect, falloff);
    }

    el_result_e el_effect_get_filament_falloff(el_effect_handle_t effect, float *outFalloff)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_filament_falloff");
        VALIDATE_OUT_PTR(outFalloff, "el_effect_get_filament_falloff");
        *outFalloff = effect->config.neon.filamentFalloff;
        LOG_D("effect=%p, falloff=%f", (void *)effect, *outFalloff);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_intensity(el_effect_handle_t effect, float intensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_intensity");
        SET_AND_LOG(effect->config.neon.intensity, intensity,
                    "effect=%p, intensity=%f", (void *)effect, intensity);
    }

    el_result_e el_effect_get_intensity(el_effect_handle_t effect, float *outIntensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_intensity");
        VALIDATE_OUT_PTR(outIntensity, "el_effect_get_intensity");
        *outIntensity = effect->config.neon.intensity;
        LOG_D("effect=%p, intensity=%f", (void *)effect, *outIntensity);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_glow_radius(el_effect_handle_t effect, float radius)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_glow_radius");
        SET_AND_LOG(effect->config.neon.glowRadius, radius,
                    "effect=%p, radius=%f", (void *)effect, radius);
    }

    el_result_e el_effect_get_glow_radius(el_effect_handle_t effect, float *outRadius)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_glow_radius");
        VALIDATE_OUT_PTR(outRadius, "el_effect_get_glow_radius");
        *outRadius = effect->config.neon.glowRadius;
        LOG_D("effect=%p, radius=%f", (void *)effect, *outRadius);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_bloom_strength(el_effect_handle_t effect, float strength)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_bloom_strength");
        SET_AND_LOG(effect->config.neon.bloomStrength, strength,
                    "effect=%p, strength=%f", (void *)effect, strength);
    }

    el_result_e el_effect_get_bloom_strength(el_effect_handle_t effect, float *outStrength)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_bloom_strength");
        VALIDATE_OUT_PTR(outStrength, "el_effect_get_bloom_strength");
        *outStrength = effect->config.neon.bloomStrength;
        LOG_D("effect=%p, strength=%f", (void *)effect, *outStrength);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_glow_side(el_effect_handle_t effect, el_glow_side_e side)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_glow_side");
        SET_AND_LOG(effect->config.neon.glowSide, static_cast<EdgeLighting::GlowSide>(side),
                    "effect=%p, side=%d", (void *)effect, (int)side);
    }

    el_result_e el_effect_get_glow_side(el_effect_handle_t effect, el_glow_side_e *outSide)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_glow_side");
        VALIDATE_OUT_PTR(outSide, "el_effect_get_glow_side");
        *outSide = static_cast<el_glow_side_e>(effect->config.neon.glowSide);
        LOG_D("effect=%p, side=%d", (void *)effect, (int)*outSide);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_glow_side_softness(el_effect_handle_t effect, float softness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_glow_side_softness");
        SET_AND_LOG(effect->config.neon.glowSideSoftness, softness,
                    "effect=%p, softness=%f", (void *)effect, softness);
    }

    el_result_e el_effect_get_glow_side_softness(el_effect_handle_t effect, float *outSoftness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_glow_side_softness");
        VALIDATE_OUT_PTR(outSoftness, "el_effect_get_glow_side_softness");
        *outSoftness = effect->config.neon.glowSideSoftness;
        LOG_D("effect=%p, softness=%f", (void *)effect, *outSoftness);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_blend_space(el_effect_handle_t effect, el_blend_space_e space)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_blend_space");
        SET_AND_LOG(effect->config.neon.blendSpace, static_cast<EdgeLighting::BlendSpace>(space),
                    "effect=%p, space=%d", (void *)effect, (int)space);
    }

    el_result_e el_effect_get_blend_space(el_effect_handle_t effect, el_blend_space_e *outSpace)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_blend_space");
        VALIDATE_OUT_PTR(outSpace, "el_effect_get_blend_space");
        *outSpace = static_cast<el_blend_space_e>(effect->config.neon.blendSpace);
        LOG_D("effect=%p, space=%d", (void *)effect, (int)*outSpace);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_hue_rotation_rate(el_effect_handle_t effect, float rate)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_hue_rotation_rate");
        SET_AND_LOG(effect->config.neon.hueRotationRate, rate,
                    "effect=%p, rate=%f", (void *)effect, rate);
    }

    el_result_e el_effect_get_hue_rotation_rate(el_effect_handle_t effect, float *outRate)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_hue_rotation_rate");
        VALIDATE_OUT_PTR(outRate, "el_effect_get_hue_rotation_rate");
        *outRate = effect->config.neon.hueRotationRate;
        LOG_D("effect=%p, rate=%f", (void *)effect, *outRate);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_color_transition_duration(el_effect_handle_t effect, float seconds)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_color_transition_duration");
        SET_AND_LOG(effect->config.neon.colorTransitionDuration, seconds,
                    "effect=%p, seconds=%f", (void *)effect, seconds);
    }

    el_result_e el_effect_get_color_transition_duration(el_effect_handle_t effect, float *outSeconds)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_color_transition_duration");
        VALIDATE_OUT_PTR(outSeconds, "el_effect_get_color_transition_duration");
        *outSeconds = effect->config.neon.colorTransitionDuration;
        LOG_D("effect=%p, seconds=%f", (void *)effect, *outSeconds);
        return EL_SUCCESS;
    }

    // --- Neon colour stops ---

    el_result_e el_effect_set_color_stop_count(el_effect_handle_t effect, int32_t count)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_color_stop_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_color_stop_count: negative count");
            return EL_ERROR_INVALID_PARAMETER;
        }
        size_t newSize = static_cast<size_t>(count);
        if (effect->config.neon.colorStops.size() == newSize)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, count=%d", (void *)effect, count);
        effect->config.neon.colorStops.resize(newSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_color_stop_count(el_effect_handle_t effect, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_color_stop_count");
        *outCount = static_cast<int32_t>(effect->config.neon.colorStops.size());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_color_stop(el_effect_handle_t effect, int32_t index,
                                         float position, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_color_stop");
        if (index < 0)
        {
            LOG_E("el_effect_set_color_stop: negative index");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &stops = effect->config.neon.colorStops;
        size_t idx = static_cast<size_t>(index);
        EdgeLighting::ColorStop newStop{position, glm::vec4(r, g, b, a)};
        if (idx < stops.size() && stops[idx] == newStop)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, index=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)effect, index, position, r, g, b, a);
        if (idx >= stops.size())
        {
            LOG_E("el_effect_set_color_stop: index %d out of range (size=%zu)", index, stops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        stops[idx] = newStop;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_color_stop(el_effect_handle_t effect, int32_t index,
                                         float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_color_stop");
        if (index < 0 || static_cast<size_t>(index) >= effect->config.neon.colorStops.size())
        {
            LOG_E("el_effect_get_color_stop: index %d out of range (size=%zu)", index, effect->config.neon.colorStops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &s = effect->config.neon.colorStops[static_cast<size_t>(index)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        LOG_D("effect=%p, index=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)effect, index, *outPosition, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_color_stops(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_color_stops");
        if (effect->config.neon.colorStops.empty())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p", (void *)effect);
        effect->config.neon.colorStops.clear();
        return EL_SUCCESS;
    }

    // --- Neon segment boosts ---

    el_result_e el_effect_set_segment_boost_count(el_effect_handle_t effect, int32_t count)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_segment_boost_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_segment_boost_count: negative count");
            return EL_ERROR_INVALID_PARAMETER;
        }
        size_t newSize = static_cast<size_t>(count);
        if (effect->config.neon.segmentBoosts.size() == newSize)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, count=%d", (void *)effect, count);
        effect->config.neon.segmentBoosts.resize(newSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_segment_boost_count(el_effect_handle_t effect, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_segment_boost_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_segment_boost_count");
        *outCount = static_cast<int32_t>(effect->config.neon.segmentBoosts.size());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_segment_boost(el_effect_handle_t effect, int32_t index,
                                            float position, float length, float boost)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_segment_boost");
        if (index < 0)
        {
            LOG_E("el_effect_set_segment_boost: negative index");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &boosts = effect->config.neon.segmentBoosts;
        size_t idx = static_cast<size_t>(index);
        if (idx < boosts.size())
        {
            auto &b = boosts[idx];
            if (b.position == position && b.length == length && b.boost == boost)
            {
                return EL_SUCCESS;
            }
        }
        LOG_I("effect=%p, index=%d, position=%f, length=%f, boost=%f", (void *)effect, index, position, length, boost);
        if (idx >= boosts.size())
        {
            LOG_E("el_effect_set_segment_boost: index %d out of range (size=%zu)", index, boosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        boosts[idx].position = position;
        boosts[idx].length = length;
        boosts[idx].boost = boost;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_segment_boost(el_effect_handle_t effect, int32_t index,
                                            float *outPosition, float *outLength, float *outBoost)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_segment_boost");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_segment_boost");
        VALIDATE_OUT_PTR(outLength, "el_effect_get_segment_boost");
        VALIDATE_OUT_PTR(outBoost, "el_effect_get_segment_boost");
        if (index < 0 || static_cast<size_t>(index) >= effect->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_segment_boost: index %d out of range (size=%zu)", index, effect->config.neon.segmentBoosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &b = effect->config.neon.segmentBoosts[static_cast<size_t>(index)];
        *outPosition = b.position;
        *outLength = b.length;
        *outBoost = b.boost;
        LOG_D("effect=%p, index=%d, position=%f, length=%f, boost=%f", (void *)effect, index, *outPosition, *outLength, *outBoost);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_segment_boosts(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_segment_boosts");
        if (effect->config.neon.segmentBoosts.empty())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p", (void *)effect);
        effect->config.neon.segmentBoosts.clear();
        return EL_SUCCESS;
    }

    // --- Neon segment blend space + colour stops ---
    // Each SegmentBoost carries its own gradient (colorStops laid head-to-tail
    // across the segment's visible span) and a per-segment blendSpace.
    // Segments with no stops fall back to the base NeonConfig gradient.

    el_result_e el_effect_set_segment_blend_space(el_effect_handle_t effect,
                                                  int32_t segmentIndex, el_blend_space_e blendSpace)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_segment_blend_space");
        if (segmentIndex < 0)
        {
            LOG_E("el_effect_set_segment_blend_space: negative segmentIndex");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &boosts = effect->config.neon.segmentBoosts;
        size_t segIdx = static_cast<size_t>(segmentIndex);
        auto newVal = static_cast<EdgeLighting::BlendSpace>(blendSpace);
        if (segIdx < boosts.size() && boosts[segIdx].blendSpace == newVal)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, segmentIndex=%d, blendSpace=%d", (void *)effect, segmentIndex, (int)blendSpace);
        if (segIdx >= boosts.size())
        {
            LOG_E("el_effect_set_segment_blend_space: segmentIndex %d out of range (size=%zu)", segmentIndex, boosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        boosts[segIdx].blendSpace = newVal;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_segment_blend_space(el_effect_handle_t effect,
                                                  int32_t segmentIndex, el_blend_space_e *outBlendSpace)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_segment_blend_space");
        VALIDATE_OUT_PTR(outBlendSpace, "el_effect_get_segment_blend_space");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= effect->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_segment_blend_space: segmentIndex %d out of range (size=%zu)", segmentIndex, effect->config.neon.segmentBoosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        *outBlendSpace = static_cast<el_blend_space_e>(
            effect->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].blendSpace);
        LOG_D("effect=%p, segmentIndex=%d, blendSpace=%d", (void *)effect, segmentIndex, (int)*outBlendSpace);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_segment_color_stop_count(el_effect_handle_t effect,
                                                       int32_t segmentIndex, int32_t count)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_segment_color_stop_count");
        if (segmentIndex < 0)
        {
            LOG_E("el_effect_set_segment_color_stop_count: negative segmentIndex");
            return EL_ERROR_INVALID_PARAMETER;
        }
        if (count < 0)
        {
            LOG_E("el_effect_set_segment_color_stop_count: negative count");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &boosts = effect->config.neon.segmentBoosts;
        size_t segIdx = static_cast<size_t>(segmentIndex);
        size_t newSize = static_cast<size_t>(count);
        if (segIdx < boosts.size() && boosts[segIdx].colorStops.size() == newSize)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, segmentIndex=%d, count=%d", (void *)effect, segmentIndex, count);
        if (segIdx >= boosts.size())
        {
            LOG_E("el_effect_set_segment_color_stop_count: segmentIndex %d out of range (size=%zu)", segmentIndex, boosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        boosts[segIdx].colorStops.resize(newSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_segment_color_stop_count(el_effect_handle_t effect,
                                                       int32_t segmentIndex, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_segment_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_segment_color_stop_count");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= effect->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_segment_color_stop_count: segmentIndex %d out of range (size=%zu)", segmentIndex, effect->config.neon.segmentBoosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        *outCount = static_cast<int32_t>(
            effect->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].colorStops.size());
        LOG_D("effect=%p, segmentIndex=%d, count=%d", (void *)effect, segmentIndex, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_segment_color_stop(el_effect_handle_t effect,
                                                 int32_t segmentIndex, int32_t stopIndex,
                                                 float position, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_segment_color_stop");
        if (segmentIndex < 0 || stopIndex < 0)
        {
            LOG_E("el_effect_set_segment_color_stop: negative index");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &boosts = effect->config.neon.segmentBoosts;
        size_t segIdx = static_cast<size_t>(segmentIndex);
        size_t stopIdx = static_cast<size_t>(stopIndex);
        EdgeLighting::ColorStop newStop{position, glm::vec4(r, g, b, a)};
        if (segIdx < boosts.size() && stopIdx < boosts[segIdx].colorStops.size() &&
            boosts[segIdx].colorStops[stopIdx] == newStop)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, segmentIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f",
              (void *)effect, segmentIndex, stopIndex, position, r, g, b, a);
        if (segIdx >= boosts.size())
        {
            LOG_E("el_effect_set_segment_color_stop: segmentIndex %d out of range (size=%zu)", segmentIndex, boosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &stops = boosts[segIdx].colorStops;
        if (stopIdx >= stops.size())
        {
            LOG_E("el_effect_set_segment_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, stops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        stops[stopIdx] = newStop;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_segment_color_stop(el_effect_handle_t effect,
                                                 int32_t segmentIndex, int32_t stopIndex,
                                                 float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_segment_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_segment_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_segment_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_segment_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_segment_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_segment_color_stop");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= effect->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_segment_color_stop: segmentIndex %d out of range (size=%zu)", segmentIndex, effect->config.neon.segmentBoosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &seg = effect->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)];
        if (stopIndex < 0 || static_cast<size_t>(stopIndex) >= seg.colorStops.size())
        {
            LOG_E("el_effect_get_segment_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, seg.colorStops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &s = seg.colorStops[static_cast<size_t>(stopIndex)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        LOG_D("effect=%p, segmentIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f",
              (void *)effect, segmentIndex, stopIndex, *outPosition, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_segment_color_stops(el_effect_handle_t effect, int32_t segmentIndex)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_segment_color_stops");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= effect->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_clear_segment_color_stops: segmentIndex %d out of range (size=%zu)", segmentIndex, effect->config.neon.segmentBoosts.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &stops = effect->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].colorStops;
        if (stops.empty())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, segmentIndex=%d", (void *)effect, segmentIndex);
        stops.clear();
        return EL_SUCCESS;
    }

    // --- Neon arcs ---

    el_result_e el_effect_set_arc_count(el_effect_handle_t effect, int32_t count)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_arc_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_arc_count: negative count");
            return EL_ERROR_INVALID_PARAMETER;
        }
        size_t newSize = static_cast<size_t>(count);
        if (effect->config.neon.arcs.size() == newSize)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, count=%d", (void *)effect, count);
        effect->config.neon.arcs.resize(newSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_arc_count(el_effect_handle_t effect, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_arc_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_arc_count");
        *outCount = static_cast<int32_t>(effect->config.neon.arcs.size());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_arc(el_effect_handle_t effect, int32_t index,
                                  float start, float length, float intensity, el_blend_space_e blendSpace)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_arc");
        if (index < 0)
        {
            LOG_E("el_effect_set_arc: negative index");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &arcs = effect->config.neon.arcs;
        size_t idx = static_cast<size_t>(index);
        auto newBlend = static_cast<EdgeLighting::BlendSpace>(blendSpace);
        if (idx < arcs.size())
        {
            auto &a = arcs[idx];
            if (a.start == start && a.length == length && a.intensity == intensity && a.blendSpace == newBlend)
            {
                return EL_SUCCESS;
            }
        }
        LOG_I("effect=%p, index=%d, start=%f, length=%f, intensity=%f, blendSpace=%d", (void *)effect, index, start, length, intensity, (int)blendSpace);
        if (idx >= arcs.size())
        {
            LOG_E("el_effect_set_arc: index %d out of range (size=%zu)", index, arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        arcs[idx].start = start;
        arcs[idx].length = length;
        arcs[idx].intensity = intensity;
        arcs[idx].blendSpace = newBlend;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_arc(el_effect_handle_t effect, int32_t index,
                                  float *outStart, float *outLength, float *outIntensity,
                                  el_blend_space_e *outBlendSpace)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_arc");
        VALIDATE_OUT_PTR(outStart, "el_effect_get_arc");
        VALIDATE_OUT_PTR(outLength, "el_effect_get_arc");
        VALIDATE_OUT_PTR(outIntensity, "el_effect_get_arc");
        VALIDATE_OUT_PTR(outBlendSpace, "el_effect_get_arc");
        if (index < 0 || static_cast<size_t>(index) >= effect->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_arc: index %d out of range (size=%zu)", index, effect->config.neon.arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &a = effect->config.neon.arcs[static_cast<size_t>(index)];
        *outStart = a.start;
        *outLength = a.length;
        *outIntensity = a.intensity;
        *outBlendSpace = static_cast<el_blend_space_e>(a.blendSpace);
        LOG_D("effect=%p, index=%d, start=%f, length=%f, intensity=%f, blendSpace=%d", (void *)effect, index, *outStart, *outLength, *outIntensity, (int)*outBlendSpace);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_arcs(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_arcs");
        if (effect->config.neon.arcs.empty())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p", (void *)effect);
        effect->config.neon.arcs.clear();
        return EL_SUCCESS;
    }

    // --- Neon arc colour stops ---

    el_result_e el_effect_set_arc_color_stop_count(el_effect_handle_t effect,
                                                   int32_t arcIndex, int32_t count)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_arc_color_stop_count");
        if (arcIndex < 0)
        {
            LOG_E("el_effect_set_arc_color_stop_count: negative arcIndex");
            return EL_ERROR_INVALID_PARAMETER;
        }
        if (count < 0)
        {
            LOG_E("el_effect_set_arc_color_stop_count: negative count");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &arcs = effect->config.neon.arcs;
        size_t arcIdx = static_cast<size_t>(arcIndex);
        size_t newSize = static_cast<size_t>(count);
        if (arcIdx < arcs.size() && arcs[arcIdx].colorStops.size() == newSize)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, arcIndex=%d, count=%d", (void *)effect, arcIndex, count);
        if (arcIdx >= arcs.size())
        {
            LOG_E("el_effect_set_arc_color_stop_count: arcIndex %d out of range (size=%zu)", arcIndex, arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        arcs[arcIdx].colorStops.resize(newSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_arc_color_stop_count(el_effect_handle_t effect,
                                                   int32_t arcIndex, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_arc_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_arc_color_stop_count");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= effect->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_arc_color_stop_count: arcIndex %d out of range (size=%zu)", arcIndex, effect->config.neon.arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        *outCount = static_cast<int32_t>(
            effect->config.neon.arcs[static_cast<size_t>(arcIndex)].colorStops.size());
        LOG_D("effect=%p, arcIndex=%d, count=%d", (void *)effect, arcIndex, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_arc_color_stop(el_effect_handle_t effect,
                                             int32_t arcIndex, int32_t stopIndex,
                                             float position, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_arc_color_stop");
        if (arcIndex < 0 || stopIndex < 0)
        {
            LOG_E("el_effect_set_arc_color_stop: negative index");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &arcs = effect->config.neon.arcs;
        size_t arcIdx = static_cast<size_t>(arcIndex);
        size_t stopIdx = static_cast<size_t>(stopIndex);
        EdgeLighting::ColorStop newStop{position, glm::vec4(r, g, b, a)};
        if (arcIdx < arcs.size() && stopIdx < arcs[arcIdx].colorStops.size() &&
            arcs[arcIdx].colorStops[stopIdx] == newStop)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, arcIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)effect, arcIndex, stopIndex, position, r, g, b, a);
        if (arcIdx >= arcs.size())
        {
            LOG_E("el_effect_set_arc_color_stop: arcIndex %d out of range (size=%zu)", arcIndex, arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &stops = arcs[arcIdx].colorStops;
        if (stopIdx >= stops.size())
        {
            LOG_E("el_effect_set_arc_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, stops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        stops[stopIdx] = newStop;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_arc_color_stop(el_effect_handle_t effect,
                                             int32_t arcIndex, int32_t stopIndex,
                                             float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_arc_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_arc_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_arc_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_arc_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_arc_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_arc_color_stop");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= effect->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_arc_color_stop: arcIndex %d out of range (size=%zu)", arcIndex, effect->config.neon.arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &arc = effect->config.neon.arcs[static_cast<size_t>(arcIndex)];
        if (stopIndex < 0 || static_cast<size_t>(stopIndex) >= arc.colorStops.size())
        {
            LOG_E("el_effect_get_arc_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, arc.colorStops.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        const auto &s = arc.colorStops[static_cast<size_t>(stopIndex)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        LOG_D("effect=%p, arcIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)effect, arcIndex, stopIndex, *outPosition, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_arc_color_stops(el_effect_handle_t effect, int32_t arcIndex)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_arc_color_stops");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= effect->config.neon.arcs.size())
        {
            LOG_E("el_effect_clear_arc_color_stops: arcIndex %d out of range (size=%zu)", arcIndex, effect->config.neon.arcs.size());
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto &stops = effect->config.neon.arcs[static_cast<size_t>(arcIndex)].colorStops;
        if (stops.empty())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, arcIndex=%d", (void *)effect, arcIndex);
        stops.clear();
        return EL_SUCCESS;
    }

    // --- Optimized neon ---

    el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_renderer_enabled");
        SET_AND_LOG(effect->config.optimizedNeon.enable, enabled != 0,
                    "effect=%p, enabled=%d", (void *)effect, enabled);
    }

    el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_optimized_renderer_enabled");
        *outEnabled = effect->config.optimizedNeon.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t effect, float scale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_resolution_scale");
        SET_AND_LOG(effect->config.optimizedNeon.resolutionScale, scale,
                    "effect=%p, scale=%f", (void *)effect, scale);
    }

    el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t effect, float *outScale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_resolution_scale");
        VALIDATE_OUT_PTR(outScale, "el_effect_get_optimized_resolution_scale");
        *outScale = effect->config.optimizedNeon.resolutionScale;
        LOG_D("effect=%p, scale=%f", (void *)effect, *outScale);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t effect, int32_t samples)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_num_samples");
        SET_AND_LOG(effect->config.optimizedNeon.numSamples, samples,
                    "effect=%p, samples=%d", (void *)effect, samples);
    }

    el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t effect, int32_t *outSamples)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_num_samples");
        VALIDATE_OUT_PTR(outSamples, "el_effect_get_optimized_num_samples");
        *outSamples = effect->config.optimizedNeon.numSamples;
        LOG_D("effect=%p, samples=%d", (void *)effect, *outSamples);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t size)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_gradient_lut_size");
        SET_AND_LOG(effect->config.optimizedNeon.gradientLutSize, size,
                    "effect=%p, size=%d", (void *)effect, size);
    }

    el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_gradient_lut_size");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_optimized_gradient_lut_size");
        *outSize = effect->config.optimizedNeon.gradientLutSize;
        LOG_D("effect=%p, size=%d", (void *)effect, *outSize);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_show_half_res(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_show_half_res");
        SET_AND_LOG(effect->config.optimizedNeon.showHalfRes, show != 0,
                    "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_optimized_show_half_res(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_show_half_res");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_optimized_show_half_res");
        *outShow = effect->config.optimizedNeon.showHalfRes ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    // --- Droplets ---

    el_result_e el_effect_set_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_renderer_enabled");
        SET_AND_LOG(effect->config.droplets.enable, enabled != 0,
                    "effect=%p, enabled=%d", (void *)effect, enabled);
    }

    el_result_e el_effect_get_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_droplets_renderer_enabled");
        *outEnabled = effect->config.droplets.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_amount(el_effect_handle_t effect, float amount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_amount");
        SET_AND_LOG(effect->config.droplets.amount, amount,
                    "effect=%p, amount=%f", (void *)effect, amount);
    }

    el_result_e el_effect_get_droplets_amount(el_effect_handle_t effect, float *outAmount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_amount");
        VALIDATE_OUT_PTR(outAmount, "el_effect_get_droplets_amount");
        *outAmount = effect->config.droplets.amount;
        LOG_D("effect=%p, amount=%f", (void *)effect, *outAmount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_speed(el_effect_handle_t effect, float speed)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_speed");
        SET_AND_LOG(effect->config.droplets.speed, speed,
                    "effect=%p, speed=%f", (void *)effect, speed);
    }

    el_result_e el_effect_get_droplets_speed(el_effect_handle_t effect, float *outSpeed)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_speed");
        VALIDATE_OUT_PTR(outSpeed, "el_effect_get_droplets_speed");
        *outSpeed = effect->config.droplets.speed;
        LOG_D("effect=%p, speed=%f", (void *)effect, *outSpeed);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_lanes(el_effect_handle_t effect, int lanes)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_lanes");
        SET_AND_LOG(effect->config.droplets.lanes, lanes,
                    "effect=%p, lanes=%d", (void *)effect, lanes);
    }

    el_result_e el_effect_get_droplets_lanes(el_effect_handle_t effect, int *outLanes)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_lanes");
        VALIDATE_OUT_PTR(outLanes, "el_effect_get_droplets_lanes");
        *outLanes = effect->config.droplets.lanes;
        LOG_D("effect=%p, lanes=%d", (void *)effect, *outLanes);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_band_width(el_effect_handle_t effect, float bandWidth)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_band_width");
        SET_AND_LOG(effect->config.droplets.bandWidth, bandWidth,
                    "effect=%p, bandWidth=%f", (void *)effect, bandWidth);
    }

    el_result_e el_effect_get_droplets_band_width(el_effect_handle_t effect, float *outBandWidth)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_band_width");
        VALIDATE_OUT_PTR(outBandWidth, "el_effect_get_droplets_band_width");
        *outBandWidth = effect->config.droplets.bandWidth;
        LOG_D("effect=%p, bandWidth=%f", (void *)effect, *outBandWidth);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_band_offset(el_effect_handle_t effect, float bandOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_band_offset");
        SET_AND_LOG(effect->config.droplets.bandOffset, bandOffset,
                    "effect=%p, bandOffset=%f", (void *)effect, bandOffset);
    }

    el_result_e el_effect_get_droplets_band_offset(el_effect_handle_t effect, float *outBandOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_band_offset");
        VALIDATE_OUT_PTR(outBandOffset, "el_effect_get_droplets_band_offset");
        *outBandOffset = effect->config.droplets.bandOffset;
        LOG_D("effect=%p, bandOffset=%f", (void *)effect, *outBandOffset);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_tint(el_effect_handle_t effect,
                                            float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_tint");
        SET_AND_LOG(effect->config.droplets.tint, glm::vec4(r, g, b, a),
                    "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }

    el_result_e el_effect_get_droplets_tint(el_effect_handle_t effect,
                                            float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_tint");
        VALIDATE_OUT_PTR(outR, "el_effect_get_droplets_tint");
        VALIDATE_OUT_PTR(outG, "el_effect_get_droplets_tint");
        VALIDATE_OUT_PTR(outB, "el_effect_get_droplets_tint");
        VALIDATE_OUT_PTR(outA, "el_effect_get_droplets_tint");
        *outR = effect->config.droplets.tint.r;
        *outG = effect->config.droplets.tint.g;
        *outB = effect->config.droplets.tint.b;
        *outA = effect->config.droplets.tint.a;
        LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    // --- Wireframe ---

    el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_wireframe_renderer_enabled");
        SET_AND_LOG(effect->config.wireframe.enable, enabled != 0,
                    "effect=%p, enabled=%d", (void *)effect, enabled);
    }

    el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_wireframe_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_wireframe_renderer_enabled");
        *outEnabled = effect->config.wireframe.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_wireframe_color(el_effect_handle_t effect,
                                              float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_wireframe_color");
        SET_AND_LOG(effect->config.wireframe.color, glm::vec4(r, g, b, a),
                    "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }

    el_result_e el_effect_get_wireframe_color(el_effect_handle_t effect,
                                              float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_wireframe_color");
        *outR = effect->config.wireframe.color.r;
        *outG = effect->config.wireframe.color.g;
        *outB = effect->config.wireframe.color.b;
        *outA = effect->config.wireframe.color.a;
        LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    // ==========================================================================
    // Effect lifecycle
    // ==========================================================================

    el_effect_handle_t el_effect_create(void)
    {
        try
        {
            auto *fx = new el_effect_handle_impl();
            LOG_I("effect=%p", (void *)fx);
            return fx;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_effect_destroy(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        if (!effect)
        {
            return EL_SUCCESS;
        }
        delete effect;
        return EL_SUCCESS;
    }

    el_result_e el_effect_init(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_init");
        try
        {
            effect->impl = std::make_unique<EdgeLighting::EdgeLightingEffect>();
            effect->impl->AddRenderer(std::make_shared<EdgeLighting::WireframeRenderer>());
            effect->impl->AddRenderer(std::make_shared<EdgeLighting::NeonRenderer>());
            effect->impl->AddRenderer(std::make_shared<EdgeLighting::NeonOptimizedRenderer>());
            // Registered last: droplets snapshot the framebuffer at render
            // time, so the neon layers must already be drawn for the pane to
            // refract them.
            effect->impl->AddRenderer(std::make_shared<EdgeLighting::DropletsRenderer>());
            if (!effect->impl->Initialize())
            {
                LOG_E("el_effect_init: renderer initialisation failed");
                return EL_ERROR_INIT_FAILED;
            }
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_capture(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_capture");
        try
        {
            effect->config = effect->impl->GetConfig();
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_update(el_effect_handle_t effect, float deltaTime)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_update");
        try
        {
            effect->impl->SetConfig(effect->config);
            effect->impl->Update(deltaTime);
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_render(el_effect_handle_t effect,
                                 int32_t viewportWidth, int32_t viewportHeight)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_render");
        try
        {
            effect->impl->Render(viewportWidth, viewportHeight);
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_clock_play(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clock_play");
        if (effect->impl->GetClock().IsPlaying())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p", (void *)effect);
        effect->impl->GetClock().Play();
        return EL_SUCCESS;
    }

    el_result_e el_effect_clock_pause(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clock_pause");
        if (!effect->impl->GetClock().IsPlaying())
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p", (void *)effect);
        effect->impl->GetClock().Pause();
        return EL_SUCCESS;
    }

    el_result_e el_effect_clock_is_playing(el_effect_handle_t effect, el_bool_t *outPlaying)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clock_is_playing");
        VALIDATE_OUT_PTR(outPlaying, "el_effect_clock_is_playing");
        *outPlaying = effect->impl->GetClock().IsPlaying() ? 1 : 0;
        LOG_D("effect=%p, playing=%d", (void *)effect, *outPlaying);
        return EL_SUCCESS;
    }

    // --- Animation attachment ---

    el_result_e el_effect_attach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
    {
        LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
        VALIDATE_EFFECT_PTR(effect, "el_effect_attach_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_attach_animation");
        if (anim->ptr)
        {
            effect->impl->Attach(anim->ptr);
        }
        return EL_SUCCESS;
    }

    el_result_e el_effect_detach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
    {
        LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
        VALIDATE_EFFECT_PTR(effect, "el_effect_detach_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_detach_animation");
        if (anim->ptr)
        {
            effect->impl->Detach(anim->ptr);
        }
        return EL_SUCCESS;
    }

    el_result_e el_effect_detach_all_animations(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_detach_all_animations");
        effect->impl->GetAnimationManager().DetachAll();
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_animation_count(el_effect_handle_t effect, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_animation_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_animation_count");
        *outCount = static_cast<int32_t>(effect->impl->GetAnimationManager().GetCount());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_contains_animation(el_effect_handle_t effect,
                                             el_animation_handle_t anim, el_bool_t *outContains)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_contains_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_contains_animation");
        VALIDATE_OUT_PTR(outContains, "el_effect_contains_animation");
        *outContains = (anim->ptr && effect->impl->GetAnimationManager().Contains(anim->ptr)) ? 1 : 0;
        LOG_D("effect=%p, anim=%p, contains=%d", (void *)effect, (void *)anim, *outContains);
        return EL_SUCCESS;
    }

    // ==========================================================================
    // Animation lifecycle
    // ==========================================================================

    el_animation_handle_t el_animation_create(el_animation_preset_e preset)
    {
        using namespace EdgeLighting;
        AnimationPtr a;
        switch (preset)
        {
        case EL_ANIM_NONE:
        {
            LOG_E("el_animation_create: EL_ANIM_NONE is not a valid preset");
            return nullptr;
        }
        case EL_ANIM_BREATHING:
        {
            a = std::make_shared<IntensityPulse>(1.0f / 0.6f, 0.4f, 1.0f);
            break;
        }
        case EL_ANIM_STROBE:
        {
            a = std::make_shared<IntensityStrobe>(1.0f / 6.0f, 0.0f, 1.0f);
            break;
        }
        case EL_ANIM_HEARTBEAT:
        {
            auto seq = std::make_shared<Sequence>();
            seq->Append(std::make_shared<Ease>(0.30f, 1.00f, 0.08f, EasingFunction::OutCubic), 0.08f);
            seq->Append(std::make_shared<Ease>(1.00f, 0.45f, 0.10f, EasingFunction::InCubic), 0.10f);
            seq->Append(std::make_shared<Ease>(0.45f, 1.00f, 0.08f, EasingFunction::OutCubic), 0.08f);
            seq->Append(std::make_shared<Ease>(1.00f, 0.30f, 0.20f, EasingFunction::InCubic), 0.20f);
            seq->Append(std::make_shared<Constant>(0.30f), 0.54f);
            seq->SetLoop(true);
            a = std::make_shared<IntensityCurve>(seq);
            break;
        }
        case EL_ANIM_SHIMMER:
        {
            auto group = std::make_shared<AnimationGroup>();
            group->Add(std::make_shared<IntensityPulse>(0.5f, 0.65f, 1.0f));
            group->Add(std::make_shared<GlowRadiusBreath>(0.5f, 5.0f, 10.0f));
            a = group;
            break;
        }
        case EL_ANIM_AURORA:
        {
            auto group = std::make_shared<AnimationGroup>();
            group->Add(std::make_shared<IntensityPulse>(10.0f, 0.75f, 1.00f));
            group->Add(std::make_shared<GlowRadiusBreath>(1.0f / 0.15f, 8.0f, 24.0f));
            group->Add(std::make_shared<BloomPulse>(5.0f, 0.20f, 0.70f));
            a = group;
            break;
        }
        case EL_ANIM_REVERSE_SWEEP:
        {
            a = std::make_shared<HueRotationEaseReverse>(0.8f, 6.0f);
            break;
        }
        case EL_ANIM_FADE_IN:
        {
            a = std::make_shared<IntensityFadeIn>(1.0f, 1.5f, EasingFunction::OutCubic);
            break;
        }
        case EL_ANIM_SEGMENT_TRAVEL:
        {
            a = std::make_shared<SegmentTravel>(3.0f, 0.15f, 4.0f);
            break;
        }
        case EL_ANIM_SEGMENT_BOUNCE:
        {
            a = std::make_shared<SegmentBounce>(4.0f, 0.20f, 3.5f);
            break;
        }
        case EL_ANIM_COMET:
        {
            a = std::make_shared<SegmentTravel>(0.6f, 0.05f, 6.0f);
            break;
        }
        case EL_ANIM_OUTLINE_TRACER:
        {
            a = std::make_shared<OutlineTracer>(2.0f, EasingFunction::OutCubic);
            break;
        }
        case EL_ANIM_FADE_OUT:
        {
            a = std::make_shared<IntensityFadeOut>(1.0f, 2.0f, EasingFunction::InCubic);
            break;
        }
        case EL_ANIM_HUE_REVERSE:
        {
            a = std::make_shared<HueRotationReverse>(0.4f, 6.0f);
            break;
        }
        case EL_ANIM_ARC_WIPE:
        {
            a = std::make_shared<ArcWipe>(3.0f, 0.1f, 0.1f, 0.5f, EasingFunction::Linear);
            break;
        }
        default:
        {
            LOG_E("el_animation_create: unknown preset");
            return nullptr;
        }
        }
        try
        {
            auto *handle = new el_animation_handle_impl{std::move(a)};
            LOG_I("anim=%p, preset=%d", (void *)handle, (int)preset);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_animation_destroy(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        if (!anim)
        {
            return EL_SUCCESS;
        }
        delete anim;
        return EL_SUCCESS;
    }

    // --- Parametric factories ---

    el_animation_handle_t el_animation_create_intensity_pulse(float duration,
                                                              float minIntensity, float maxIntensity)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::IntensityPulse>(duration, minIntensity, maxIntensity)};
            LOG_I("anim=%p, duration=%f, minIntensity=%f, maxIntensity=%f", (void *)handle, duration, minIntensity, maxIntensity);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_intensity_strobe(float duration,
                                                               float offIntensity, float onIntensity)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::IntensityStrobe>(duration, offIntensity, onIntensity)};
            LOG_I("anim=%p, duration=%f, offIntensity=%f, onIntensity=%f", (void *)handle, duration, offIntensity, onIntensity);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_intensity_fade_in(float targetIntensity,
                                                                float duration, el_easing_e easing)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::IntensityFadeIn>(targetIntensity, duration, toEasing(easing))};
            LOG_I("anim=%p, targetIntensity=%f, duration=%f, easing=%d", (void *)handle, targetIntensity, duration, (int)easing);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_intensity_fade_out(float startIntensity,
                                                                 float duration, el_easing_e easing)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::IntensityFadeOut>(startIntensity, duration, toEasing(easing))};
            LOG_I("anim=%p, startIntensity=%f, duration=%f, easing=%d", (void *)handle, startIntensity, duration, (int)easing);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_glow_radius_breath(float duration,
                                                                 float minRadius, float maxRadius)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::GlowRadiusBreath>(duration, minRadius, maxRadius)};
            LOG_I("anim=%p, duration=%f, minRadius=%f, maxRadius=%f", (void *)handle, duration, minRadius, maxRadius);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_bloom_pulse(float duration,
                                                          float minStrength, float maxStrength)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::BloomPulse>(duration, minStrength, maxStrength)};
            LOG_I("anim=%p, duration=%f, minStrength=%f, maxStrength=%f", (void *)handle, duration, minStrength, maxStrength);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_hue_rotation_reverse(float peakRate,
                                                                   float duration)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::HueRotationReverse>(peakRate, duration)};
            LOG_I("anim=%p, peakRate=%f, duration=%f", (void *)handle, peakRate, duration);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_hue_rotation_ease_reverse(float peakRate,
                                                                        float duration)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::HueRotationEaseReverse>(peakRate, duration)};
            LOG_I("anim=%p, peakRate=%f, duration=%f", (void *)handle, peakRate, duration);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_segment_travel(float duration,
                                                             float length, float boost)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::SegmentTravel>(duration, length, boost)};
            LOG_I("anim=%p, duration=%f, length=%f, boost=%f", (void *)handle, duration, length, boost);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_segment_bounce(float duration,
                                                             float length, float boost)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::SegmentBounce>(duration, length, boost)};
            LOG_I("anim=%p, duration=%f, length=%f, boost=%f", (void *)handle, duration, length, boost);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_outline_tracer(float duration,
                                                             el_easing_e easing)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::OutlineTracer>(duration, toEasing(easing))};
            LOG_I("anim=%p, duration=%f, easing=%d", (void *)handle, duration, (int)easing);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_arc_wipe(float duration,
                                                       float startPosition, float endPosition, float maxLength,
                                                       el_easing_e easing)
    {
        LOG_I("duration=%f, startPosition=%f, endPosition=%f, maxLength=%f, easing=%d", duration, startPosition, endPosition, maxLength, (int)easing);
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::ArcWipe>(
                duration, startPosition, endPosition, maxLength, toEasing(easing))};
            LOG_I("anim=%p, duration=%f, startPosition=%f, endPosition=%f, maxLength=%f, easing=%d", (void *)handle, duration, startPosition, endPosition, maxLength, (int)easing);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    // --- Stateful lifecycle ---

    el_result_e el_animation_play(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANIM_PTR(anim, "el_animation_play");
        if (anim->ptr)
        {
            anim->ptr->Play();
        }
        return EL_SUCCESS;
    }

    el_result_e el_animation_pause(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANIM_PTR(anim, "el_animation_pause");
        if (anim->ptr)
        {
            anim->ptr->Pause();
        }
        return EL_SUCCESS;
    }

    el_result_e el_animation_stop(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANIM_PTR(anim, "el_animation_stop");
        if (anim->ptr)
        {
            anim->ptr->Stop();
        }
        return EL_SUCCESS;
    }

    el_result_e el_animation_reset(el_animation_handle_t anim, el_effect_handle_t effect)
    {
        LOG_I("anim=%p, effect=%p", (void *)anim, (void *)effect);
        VALIDATE_ANIM_PTR(anim, "el_animation_reset");
        VALIDATE_EFFECT_PTR(effect, "el_animation_reset");
        try
        {
            anim->ptr->Reset(effect->config);
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_animation_update(el_animation_handle_t anim, float dt)
    {
        LOG_I("anim=%p, dt=%f", (void *)anim, dt);
        VALIDATE_ANIM_PTR(anim, "el_animation_update");
        if (anim->ptr)
        {
            anim->ptr->Update(dt);
        }
        return EL_SUCCESS;
    }

    el_result_e el_animation_apply(el_animation_handle_t anim, el_effect_handle_t effect)
    {
        LOG_I("anim=%p, effect=%p", (void *)anim, (void *)effect);
        VALIDATE_ANIM_PTR(anim, "el_animation_apply");
        VALIDATE_EFFECT_PTR(effect, "el_animation_apply");
        try
        {
            anim->ptr->Apply(effect->config);
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    // --- Elapsed / state ---

    el_result_e el_animation_get_state(el_animation_handle_t anim, el_animation_state_e *outState)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_state");
        VALIDATE_OUT_PTR(outState, "el_animation_get_state");
        if (!anim->ptr)
        {
            *outState = EL_ANIM_STATE_STOPPED;
        }
        else
        {
            using ES = EdgeLighting::AnimationState;
            switch (anim->ptr->GetState())
            {
            case ES::PLAYING:
                *outState = EL_ANIM_STATE_PLAYING;
                break;
            case ES::PAUSED:
                *outState = EL_ANIM_STATE_PAUSED;
                break;
            case ES::STOPPED:
            default:
                *outState = EL_ANIM_STATE_STOPPED;
                break;
            }
        }
        LOG_D("anim=%p, state=%d", (void *)anim, (int)*outState);
        return EL_SUCCESS;
    }

    el_result_e el_animation_get_elapsed(el_animation_handle_t anim, float *outElapsed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_elapsed");
        VALIDATE_OUT_PTR(outElapsed, "el_animation_get_elapsed");
        *outElapsed = anim->ptr ? anim->ptr->GetElapsed() : 0.0f;
        LOG_D("anim=%p, elapsed=%f", (void *)anim, *outElapsed);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_elapsed(el_animation_handle_t anim, float elapsed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_elapsed");
        if (!anim->ptr || anim->ptr->GetElapsed() == elapsed)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, elapsed=%f", (void *)anim, elapsed);
        anim->ptr->SetElapsed(elapsed);
        return EL_SUCCESS;
    }

    el_result_e el_animation_get_progress(el_animation_handle_t anim, float *outProgress)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_progress");
        VALIDATE_OUT_PTR(outProgress, "el_animation_get_progress");
        *outProgress = anim->ptr ? anim->ptr->GetProgress() : 0.0f;
        LOG_D("anim=%p, progress=%f", (void *)anim, *outProgress);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_progress(el_animation_handle_t anim, float progress)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_progress");
        if (!anim->ptr || anim->ptr->GetProgress() == progress)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, progress=%f", (void *)anim, progress);
        anim->ptr->SetProgress(progress);
        return EL_SUCCESS;
    }

    // --- End action ---

    el_result_e el_animation_get_end_action(el_animation_handle_t anim, el_end_action_e *outAction)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_end_action");
        VALIDATE_OUT_PTR(outAction, "el_animation_get_end_action");
        *outAction = anim->ptr ? fromEndAction(anim->ptr->GetEndAction()) : EL_END_ACTION_HOLD_CURRENT;
        LOG_D("anim=%p, action=%d", (void *)anim, (int)*outAction);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_end_action(el_animation_handle_t anim, el_end_action_e action)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_end_action");
        auto newVal = toEndAction(action);
        if (!anim->ptr || anim->ptr->GetEndAction() == newVal)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, action=%d", (void *)anim, (int)action);
        anim->ptr->SetEndAction(newVal);
        return EL_SUCCESS;
    }

    el_result_e el_animation_capture_baseline(el_animation_handle_t anim, el_effect_handle_t effect)
    {
        LOG_I("anim=%p, effect=%p", (void *)anim, (void *)effect);
        VALIDATE_ANIM_PTR(anim, "el_animation_capture_baseline");
        VALIDATE_EFFECT_PTR(effect, "el_animation_capture_baseline");
        try
        {
            if (anim->ptr)
            {
                anim->ptr->CaptureBaseline(effect->config);
            }
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    // --- Playback mode ---

    el_result_e el_animation_get_playback_mode(el_animation_handle_t anim, el_playback_mode_e *outMode)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_playback_mode");
        VALIDATE_OUT_PTR(outMode, "el_animation_get_playback_mode");
        *outMode = anim->ptr ? fromPlaybackMode(anim->ptr->GetPlaybackMode()) : EL_PLAYBACK_LOOP;
        LOG_D("anim=%p, mode=%d", (void *)anim, (int)*outMode);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_playback_mode(el_animation_handle_t anim, el_playback_mode_e mode)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_playback_mode");
        auto newVal = toPlaybackMode(mode);
        if (!anim->ptr || anim->ptr->GetPlaybackMode() == newVal)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, mode=%d", (void *)anim, (int)mode);
        anim->ptr->SetPlaybackMode(newVal);
        return EL_SUCCESS;
    }

    // --- Duration ---

    el_result_e el_animation_get_duration(el_animation_handle_t anim, float *outSeconds)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_duration");
        VALIDATE_OUT_PTR(outSeconds, "el_animation_get_duration");
        *outSeconds = anim->ptr ? anim->ptr->GetDuration() : 0.0f;
        LOG_D("anim=%p, seconds=%f", (void *)anim, *outSeconds);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_duration(el_animation_handle_t anim, float seconds)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_duration");
        if (!anim->ptr || anim->ptr->GetDuration() == seconds)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, seconds=%f", (void *)anim, seconds);
        anim->ptr->SetDuration(seconds);
        return EL_SUCCESS;
    }

    // --- Speed ---

    el_result_e el_animation_get_speed(el_animation_handle_t anim, float *outSpeed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_speed");
        VALIDATE_OUT_PTR(outSpeed, "el_animation_get_speed");
        *outSpeed = anim->ptr ? anim->ptr->GetSpeed() : 1.0f;
        LOG_D("anim=%p, speed=%f", (void *)anim, *outSpeed);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_speed(el_animation_handle_t anim, float speed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_speed");
        if (!anim->ptr || anim->ptr->GetSpeed() == speed)
        {
            return EL_SUCCESS;
        }
        LOG_I("anim=%p, speed=%f", (void *)anim, speed);
        anim->ptr->SetSpeed(speed);
        return EL_SUCCESS;
    }

    // --- Callbacks ---

    el_result_e el_animation_set_on_complete_callback(el_animation_handle_t anim,
                                                      el_animation_on_completed_callback callback, void *userData)
    {
        LOG_I("anim=%p, callback=%p, userData=%p", (void *)anim, (void *)callback, userData);
        VALIDATE_ANIM_PTR(anim, "el_animation_set_on_complete_callback");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        if (!callback)
        {
            anim->ptr->OnComplete = nullptr;
            return EL_SUCCESS;
        }
        anim->ptr->OnComplete = [callback, userData]()
        {
            callback(userData);
        };
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_on_state_changed_callback(el_animation_handle_t anim,
                                                           el_animation_on_state_changed_callback callback,
                                                           void *userData)
    {
        LOG_I("anim=%p, callback=%p, userData=%p", (void *)anim, (void *)callback, userData);
        VALIDATE_ANIM_PTR(anim, "el_animation_set_on_state_changed_callback");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        if (!callback)
        {
            anim->ptr->OnStateChanged = nullptr;
            return EL_SUCCESS;
        }
        anim->ptr->OnStateChanged = [callback, userData](EdgeLighting::AnimationState prev,
                                                         EdgeLighting::AnimationState now)
        {
            callback(static_cast<el_animation_state_e>(prev),
                     static_cast<el_animation_state_e>(now),
                     userData);
        };
        return EL_SUCCESS;
    }

    // ==========================================================================
    // Field-bound animation
    // ==========================================================================

    el_animation_handle_t el_animation_from_modulator(el_config_field_e field,
                                                      el_modulator_handle_t mod)
    {
        LOG_I("field=%d, mod=%p", (int)field, (void *)mod);
        if (!mod)
        {
            LOG_E("el_animation_from_modulator: mod is null");
            return nullptr;
        }
        try
        {
            auto a = std::make_shared<EdgeLighting::FieldBoundAnimation>(
                toAnimatableField(field), mod->ptr);
            auto *handle = new el_animation_handle_impl{std::move(a)};
            LOG_I("anim=%p", (void *)handle);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t el_animation_create_field_bound(void)
    {
        try
        {
            auto a = std::make_shared<EdgeLighting::FieldBoundAnimation>();
            auto *handle = new el_animation_handle_impl{std::move(a)};
            LOG_I("anim=%p", (void *)handle);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_animation_add_field(el_animation_handle_t anim,
                                       el_config_field_e field, el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, field=%d, mod=%p", (void *)anim, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddField(toAnimatableField(field), mod->ptr);
        return EL_SUCCESS;
    }

    el_result_e el_animation_add_segment_field(el_animation_handle_t anim,
                                               int32_t index, el_segment_field_e field, el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, index=%d, field=%d, mod=%p", (void *)anim, index, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_segment_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_segment_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_segment_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddSegmentField(static_cast<size_t>(index),
                            static_cast<EdgeLighting::SegmentField>(field), mod->ptr);
        return EL_SUCCESS;
    }

    el_result_e el_animation_add_arc_field(el_animation_handle_t anim,
                                           int32_t index, el_arc_field_e field, el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, index=%d, field=%d, mod=%p", (void *)anim, index, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_arc_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_arc_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_arc_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddArcField(static_cast<size_t>(index),
                        static_cast<EdgeLighting::ArcField>(field), mod->ptr);
        return EL_SUCCESS;
    }

    el_result_e el_animation_add_arc_stop_field(el_animation_handle_t anim,
                                                int32_t arcIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, arcIndex=%d, stopIndex=%d, field=%d, mod=%p", (void *)anim, arcIndex, stopIndex, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_arc_stop_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_arc_stop_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_arc_stop_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddArcStopField(static_cast<size_t>(arcIndex), static_cast<size_t>(stopIndex),
                            static_cast<EdgeLighting::ColorStopField>(field), mod->ptr);
        return EL_SUCCESS;
    }

    el_result_e el_animation_add_segment_stop_field(el_animation_handle_t anim,
                                                    int32_t segmentIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                    el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, segmentIndex=%d, stopIndex=%d, field=%d, mod=%p", (void *)anim, segmentIndex, stopIndex, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_segment_stop_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_segment_stop_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_segment_stop_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddStopField(static_cast<size_t>(segmentIndex), static_cast<size_t>(stopIndex),
                         static_cast<EdgeLighting::ColorStopField>(field), mod->ptr);
        return EL_SUCCESS;
    }

    // ==========================================================================
    // Modulator factories
    // ==========================================================================

    el_modulator_handle_t el_modulator_create_constant(float value)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Constant>(value)};
            LOG_I("mod=%p, value=%f", (void *)handle, value);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_oscillator(float frequency,
                                                         float minValue, float maxValue, float phase, el_waveform_e waveform)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Oscillator>(
                frequency, minValue, maxValue, phase, toWaveform(waveform))};
            LOG_I("mod=%p, frequency=%f, minValue=%f, maxValue=%f, phase=%f, waveform=%d", (void *)handle, frequency, minValue, maxValue, phase, (int)waveform);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_ease(float from, float to,
                                                   float duration, el_easing_e easing, el_bool_t loop)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Ease>(
                from, to, duration, toEasing(easing), loop != 0)};
            LOG_I("mod=%p, from=%f, to=%f, duration=%f, easing=%d, loop=%d", (void *)handle, from, to, duration, (int)easing, loop);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_sequence(el_bool_t loop)
    {
        try
        {
            auto seq = std::make_shared<EdgeLighting::Sequence>();
            seq->SetLoop(loop != 0);
            auto *handle = new el_modulator_handle_impl{std::move(seq)};
            LOG_I("mod=%p, loop=%d", (void *)handle, loop);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_sequence_append(el_modulator_handle_t seq,
                                             el_modulator_handle_t stage, float duration)
    {
        LOG_I("seq=%p, stage=%p, duration=%f", (void *)seq, (void *)stage, duration);
        VALIDATE_MOD_PTR(seq, "el_modulator_sequence_append");
        VALIDATE_MOD_PTR(stage, "el_modulator_sequence_append");
        auto *s = dynamic_cast<EdgeLighting::Sequence *>(seq->ptr.get());
        if (!s)
        {
            LOG_E("el_modulator_sequence_append: seq is not a Sequence");
            return EL_ERROR_INVALID_PARAMETER;
        }
        s->Append(stage->ptr, duration);
        return EL_SUCCESS;
    }

    el_modulator_handle_t el_modulator_create_multiplier(el_modulator_handle_t a,
                                                         el_modulator_handle_t b)
    {
        if (!a || !b)
        {
            LOG_E("el_modulator_create_multiplier: null arg");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Multiplier>(a->ptr, b->ptr)};
            LOG_I("mod=%p, a=%p, b=%p", (void *)handle, (void *)a, (void *)b);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_adder(el_modulator_handle_t a,
                                                    el_modulator_handle_t b)
    {
        if (!a || !b)
        {
            LOG_E("el_modulator_create_adder: null arg");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Adder>(a->ptr, b->ptr)};
            LOG_I("mod=%p, a=%p, b=%p", (void *)handle, (void *)a, (void *)b);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_remap(el_modulator_handle_t inner,
                                                    float outMin, float outMax)
    {
        if (!inner)
        {
            LOG_E("el_modulator_create_remap: inner is null");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Remap>(inner->ptr, outMin, outMax)};
            LOG_I("mod=%p, inner=%p, outMin=%f, outMax=%f", (void *)handle, (void *)inner, outMin, outMax);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_destroy(el_modulator_handle_t mod)
    {
        LOG_I("mod=%p", (void *)mod);
        if (!mod)
        {
            return EL_SUCCESS;
        }
        delete mod;
        return EL_SUCCESS;
    }

    el_result_e el_modulator_evaluate(el_modulator_handle_t mod, float time, float *outValue)
    {
        VALIDATE_MOD_PTR(mod, "el_modulator_evaluate");
        VALIDATE_OUT_PTR(outValue, "el_modulator_evaluate");
        *outValue = mod->ptr ? mod->ptr->Evaluate(time) : 0.0f;
        LOG_D("mod=%p, time=%f, value=%f", (void *)mod, time, *outValue);
        return EL_SUCCESS;
    }

} // extern "C"

// Undo the local LOG_D override so nothing downstream inherits it.
#if defined(PLATFORM_MACOS) || defined(PLATFORM_WINDOWS)
#undef LOG_D
#endif
