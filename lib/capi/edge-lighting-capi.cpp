#include "edge-lighting-capi.h"

#include "core/edge-lighting.h"
#include "animation/animation-manager.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/neon-optimized-renderer.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/modulator.h"
#include "util/log-util.h"

#include <algorithm>
#include <memory>

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
    EdgeLighting::EdgeLightingEffect effect;
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
#define VALIDATE_FX(fx, fn)              \
    do                                   \
    {                                    \
        if (!(fx))                       \
        {                                \
            LOG_E("%s: fx is null", fn); \
            return EL_ERR_NULL_ARG;      \
        }                                \
    } while (0)

#define VALIDATE_ANM(anim, fn)             \
    do                                     \
    {                                      \
        if (!(anim))                       \
        {                                  \
            LOG_E("%s: anim is null", fn); \
            return EL_ERR_NULL_ARG;        \
        }                                  \
    } while (0)

#define VALIDATE_MOD(mod, fn)             \
    do                                    \
    {                                     \
        if (!(mod))                       \
        {                                 \
            LOG_E("%s: mod is null", fn); \
            return EL_ERR_NULL_ARG;       \
        }                                 \
    } while (0)

#define VALIDATE_OUT_PTR(ptr, fn)                 \
    do                                            \
    {                                             \
        if (!(ptr))                               \
        {                                         \
            LOG_E("%s: out pointer is null", fn); \
            return EL_ERR_NULL_ARG;               \
        }                                         \
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

    el_result_e el_effect_set_geometry(el_effect_handle_t *fx,
                                       float width, float height, float posX, float posY, float cornerRadius)
    {
        LOG_I("fx=%p, width=%f, height=%f, posX=%f, posY=%f, cornerRadius=%f", (void *)fx, width, height, posX, posY, cornerRadius);
        VALIDATE_FX(fx, "el_effect_set_geometry");
        fx->config.geometry.width = width;
        fx->config.geometry.height = height;
        fx->config.geometry.position = glm::vec2(posX, posY);
        fx->config.geometry.cornerRadius = cornerRadius;
        return EL_OK;
    }

    el_result_e el_effect_get_geometry(const el_effect_handle_t *fx,
                                       float *outWidth, float *outHeight, float *outPosX, float *outPosY,
                                       float *outCornerRadius)
    {
        LOG_I("fx=%p, outWidth=%p, outHeight=%p, outPosX=%p, outPosY=%p, outCornerRadius=%p", (void *)fx, (void *)outWidth, (void *)outHeight, (void *)outPosX, (void *)outPosY, (void *)outCornerRadius);
        VALIDATE_FX(fx, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outWidth, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outHeight, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outPosX, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outPosY, "el_effect_get_geometry");
        VALIDATE_OUT_PTR(outCornerRadius, "el_effect_get_geometry");
        *outWidth = fx->config.geometry.width;
        *outHeight = fx->config.geometry.height;
        *outPosX = fx->config.geometry.position.x;
        *outPosY = fx->config.geometry.position.y;
        *outCornerRadius = fx->config.geometry.cornerRadius;
        return EL_OK;
    }

    el_result_e el_effect_set_winding(el_effect_handle_t *fx, el_winding_e winding)
    {
        LOG_I("fx=%p, winding=%d", (void *)fx, (int)winding);
        VALIDATE_FX(fx, "el_effect_set_winding");
        fx->config.geometry.winding = static_cast<EdgeLighting::Winding>(winding);
        return EL_OK;
    }

    el_result_e el_effect_get_winding(const el_effect_handle_t *fx, el_winding_e *outWinding)
    {
        LOG_I("fx=%p, outWinding=%p", (void *)fx, (void *)outWinding);
        VALIDATE_FX(fx, "el_effect_get_winding");
        VALIDATE_OUT_PTR(outWinding, "el_effect_get_winding");
        *outWinding = static_cast<el_winding_e>(fx->config.geometry.winding);
        return EL_OK;
    }

    // --- Neon scalars ---

    el_result_e el_effect_set_neon_enabled(el_effect_handle_t *fx, int enabled)
    {
        LOG_I("fx=%p, enabled=%d", (void *)fx, enabled);
        VALIDATE_FX(fx, "el_effect_set_neon_enabled");
        fx->config.neon.enable = enabled != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_enabled(const el_effect_handle_t *fx, int *outEnabled)
    {
        LOG_I("fx=%p, outEnabled=%p", (void *)fx, (void *)outEnabled);
        VALIDATE_FX(fx, "el_effect_get_neon_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_neon_enabled");
        *outEnabled = fx->config.neon.enable ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_show_gradient_lut(el_effect_handle_t *fx, int show)
    {
        LOG_I("fx=%p, show=%d", (void *)fx, show);
        VALIDATE_FX(fx, "el_effect_set_neon_show_gradient_lut");
        fx->config.neon.showGradientLUT = show != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_show_gradient_lut(const el_effect_handle_t *fx, int *outShow)
    {
        LOG_I("fx=%p, outShow=%p", (void *)fx, (void *)outShow);
        VALIDATE_FX(fx, "el_effect_get_neon_show_gradient_lut");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_neon_show_gradient_lut");
        *outShow = fx->config.neon.showGradientLUT ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_show_color_stops(el_effect_handle_t *fx, int show)
    {
        LOG_I("fx=%p, show=%d", (void *)fx, show);
        VALIDATE_FX(fx, "el_effect_set_neon_show_color_stops");
        fx->config.neon.showColorStops = show != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_show_color_stops(const el_effect_handle_t *fx, int *outShow)
    {
        LOG_I("fx=%p, outShow=%p", (void *)fx, (void *)outShow);
        VALIDATE_FX(fx, "el_effect_get_neon_show_color_stops");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_neon_show_color_stops");
        *outShow = fx->config.neon.showColorStops ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_opaque(el_effect_handle_t *fx, int opaque)
    {
        LOG_I("fx=%p, opaque=%d", (void *)fx, opaque);
        VALIDATE_FX(fx, "el_effect_set_neon_opaque");
        fx->config.neon.opaque = opaque != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_opaque(const el_effect_handle_t *fx, int *outOpaque)
    {
        LOG_I("fx=%p, outOpaque=%p", (void *)fx, (void *)outOpaque);
        VALIDATE_FX(fx, "el_effect_get_neon_opaque");
        VALIDATE_OUT_PTR(outOpaque, "el_effect_get_neon_opaque");
        *outOpaque = fx->config.neon.opaque ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_opaque_color(el_effect_handle_t *fx,
                                                float r, float g, float b, float a)
    {
        LOG_I("fx=%p, r=%f, g=%f, b=%f, a=%f", (void *)fx, r, g, b, a);
        VALIDATE_FX(fx, "el_effect_set_neon_opaque_color");
        fx->config.neon.opaqueColor = glm::vec4(r, g, b, a);
        return EL_OK;
    }

    el_result_e el_effect_get_neon_opaque_color(const el_effect_handle_t *fx,
                                                float *outR, float *outG, float *outB, float *outA)
    {
        LOG_I("fx=%p, outR=%p, outG=%p, outB=%p, outA=%p", (void *)fx, (void *)outR, (void *)outG, (void *)outB, (void *)outA);
        VALIDATE_FX(fx, "el_effect_get_neon_opaque_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_neon_opaque_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_neon_opaque_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_neon_opaque_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_neon_opaque_color");
        *outR = fx->config.neon.opaqueColor.r;
        *outG = fx->config.neon.opaqueColor.g;
        *outB = fx->config.neon.opaqueColor.b;
        *outA = fx->config.neon.opaqueColor.a;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_line_width(el_effect_handle_t *fx, float width)
    {
        LOG_I("fx=%p, width=%f", (void *)fx, width);
        VALIDATE_FX(fx, "el_effect_set_neon_line_width");
        fx->config.neon.lineWidth = width;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_line_width(const el_effect_handle_t *fx, float *outWidth)
    {
        LOG_I("fx=%p, outWidth=%p", (void *)fx, (void *)outWidth);
        VALIDATE_FX(fx, "el_effect_get_neon_line_width");
        VALIDATE_OUT_PTR(outWidth, "el_effect_get_neon_line_width");
        *outWidth = fx->config.neon.lineWidth;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_filament_falloff(el_effect_handle_t *fx, float falloff)
    {
        LOG_I("fx=%p, falloff=%f", (void *)fx, falloff);
        VALIDATE_FX(fx, "el_effect_set_neon_filament_falloff");
        fx->config.neon.filamentFalloff = falloff;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_filament_falloff(const el_effect_handle_t *fx, float *outFalloff)
    {
        LOG_I("fx=%p, outFalloff=%p", (void *)fx, (void *)outFalloff);
        VALIDATE_FX(fx, "el_effect_get_neon_filament_falloff");
        VALIDATE_OUT_PTR(outFalloff, "el_effect_get_neon_filament_falloff");
        *outFalloff = fx->config.neon.filamentFalloff;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_intensity(el_effect_handle_t *fx, float val)
    {
        LOG_I("fx=%p, val=%f", (void *)fx, val);
        VALIDATE_FX(fx, "el_effect_set_neon_intensity");
        fx->config.neon.intensity = val;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_intensity(const el_effect_handle_t *fx, float *outVal)
    {
        LOG_I("fx=%p, outVal=%p", (void *)fx, (void *)outVal);
        VALIDATE_FX(fx, "el_effect_get_neon_intensity");
        VALIDATE_OUT_PTR(outVal, "el_effect_get_neon_intensity");
        *outVal = fx->config.neon.intensity;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_glow_radius(el_effect_handle_t *fx, float radius)
    {
        LOG_I("fx=%p, radius=%f", (void *)fx, radius);
        VALIDATE_FX(fx, "el_effect_set_neon_glow_radius");
        fx->config.neon.glowRadius = radius;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_glow_radius(const el_effect_handle_t *fx, float *outRadius)
    {
        LOG_I("fx=%p, outRadius=%p", (void *)fx, (void *)outRadius);
        VALIDATE_FX(fx, "el_effect_get_neon_glow_radius");
        VALIDATE_OUT_PTR(outRadius, "el_effect_get_neon_glow_radius");
        *outRadius = fx->config.neon.glowRadius;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_bloom_strength(el_effect_handle_t *fx, float val)
    {
        LOG_I("fx=%p, val=%f", (void *)fx, val);
        VALIDATE_FX(fx, "el_effect_set_neon_bloom_strength");
        fx->config.neon.bloomStrength = val;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_bloom_strength(const el_effect_handle_t *fx, float *outVal)
    {
        LOG_I("fx=%p, outVal=%p", (void *)fx, (void *)outVal);
        VALIDATE_FX(fx, "el_effect_get_neon_bloom_strength");
        VALIDATE_OUT_PTR(outVal, "el_effect_get_neon_bloom_strength");
        *outVal = fx->config.neon.bloomStrength;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_glow_side(el_effect_handle_t *fx, el_glow_side_e side)
    {
        LOG_I("fx=%p, side=%d", (void *)fx, (int)side);
        VALIDATE_FX(fx, "el_effect_set_neon_glow_side");
        fx->config.neon.glowSide = static_cast<EdgeLighting::GlowSide>(side);
        return EL_OK;
    }

    el_result_e el_effect_get_neon_glow_side(const el_effect_handle_t *fx, el_glow_side_e *outSide)
    {
        LOG_I("fx=%p, outSide=%p", (void *)fx, (void *)outSide);
        VALIDATE_FX(fx, "el_effect_get_neon_glow_side");
        VALIDATE_OUT_PTR(outSide, "el_effect_get_neon_glow_side");
        *outSide = static_cast<el_glow_side_e>(fx->config.neon.glowSide);
        return EL_OK;
    }

    el_result_e el_effect_set_neon_glow_side_softness(el_effect_handle_t *fx, float val)
    {
        LOG_I("fx=%p, val=%f", (void *)fx, val);
        VALIDATE_FX(fx, "el_effect_set_neon_glow_side_softness");
        fx->config.neon.glowSideSoftness = val;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_glow_side_softness(const el_effect_handle_t *fx, float *outVal)
    {
        LOG_I("fx=%p, outVal=%p", (void *)fx, (void *)outVal);
        VALIDATE_FX(fx, "el_effect_get_neon_glow_side_softness");
        VALIDATE_OUT_PTR(outVal, "el_effect_get_neon_glow_side_softness");
        *outVal = fx->config.neon.glowSideSoftness;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_blend_space(el_effect_handle_t *fx, el_blend_space_e space)
    {
        LOG_I("fx=%p, space=%d", (void *)fx, (int)space);
        VALIDATE_FX(fx, "el_effect_set_neon_blend_space");
        fx->config.neon.blendSpace = static_cast<EdgeLighting::BlendSpace>(space);
        return EL_OK;
    }

    el_result_e el_effect_get_neon_blend_space(const el_effect_handle_t *fx, el_blend_space_e *outSpace)
    {
        LOG_I("fx=%p, outSpace=%p", (void *)fx, (void *)outSpace);
        VALIDATE_FX(fx, "el_effect_get_neon_blend_space");
        VALIDATE_OUT_PTR(outSpace, "el_effect_get_neon_blend_space");
        *outSpace = static_cast<el_blend_space_e>(fx->config.neon.blendSpace);
        return EL_OK;
    }

    el_result_e el_effect_set_neon_hue_rotation_rate(el_effect_handle_t *fx, float rate)
    {
        LOG_I("fx=%p, rate=%f", (void *)fx, rate);
        VALIDATE_FX(fx, "el_effect_set_neon_hue_rotation_rate");
        fx->config.neon.hueRotationRate = rate;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_hue_rotation_rate(const el_effect_handle_t *fx, float *outRate)
    {
        LOG_I("fx=%p, outRate=%p", (void *)fx, (void *)outRate);
        VALIDATE_FX(fx, "el_effect_get_neon_hue_rotation_rate");
        VALIDATE_OUT_PTR(outRate, "el_effect_get_neon_hue_rotation_rate");
        *outRate = fx->config.neon.hueRotationRate;
        return EL_OK;
    }

    el_result_e el_effect_set_neon_color_transition_duration(el_effect_handle_t *fx, float seconds)
    {
        LOG_I("fx=%p, seconds=%f", (void *)fx, seconds);
        VALIDATE_FX(fx, "el_effect_set_neon_color_transition_duration");
        fx->config.neon.colorTransitionDuration = seconds;
        return EL_OK;
    }

    el_result_e el_effect_get_neon_color_transition_duration(const el_effect_handle_t *fx, float *outSeconds)
    {
        LOG_I("fx=%p, outSeconds=%p", (void *)fx, (void *)outSeconds);
        VALIDATE_FX(fx, "el_effect_get_neon_color_transition_duration");
        VALIDATE_OUT_PTR(outSeconds, "el_effect_get_neon_color_transition_duration");
        *outSeconds = fx->config.neon.colorTransitionDuration;
        return EL_OK;
    }

    // --- Neon colour stops ---

    el_result_e el_effect_set_neon_color_stop_count(el_effect_handle_t *fx, int32_t count)
    {
        LOG_I("fx=%p, count=%d", (void *)fx, count);
        VALIDATE_FX(fx, "el_effect_set_neon_color_stop_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_neon_color_stop_count: negative count");
            return EL_ERR_INVALID_ARG;
        }
        fx->config.neon.colorStops.resize(static_cast<size_t>(count));
        return EL_OK;
    }

    el_result_e el_effect_get_neon_color_stop_count(const el_effect_handle_t *fx, int32_t *outCount)
    {
        LOG_I("fx=%p, outCount=%p", (void *)fx, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_neon_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_neon_color_stop_count");
        *outCount = static_cast<int32_t>(fx->config.neon.colorStops.size());
        return EL_OK;
    }

    el_result_e el_effect_set_neon_color_stop(el_effect_handle_t *fx, int32_t index,
                                              float position, float r, float g, float b, float a)
    {
        LOG_I("fx=%p, index=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)fx, index, position, r, g, b, a);
        VALIDATE_FX(fx, "el_effect_set_neon_color_stop");
        if (index < 0)
        {
            LOG_E("el_effect_set_neon_color_stop: negative index");
            return EL_ERR_INVALID_ARG;
        }
        auto &stops = fx->config.neon.colorStops;
        size_t uIndex = static_cast<size_t>(index);
        if (uIndex >= stops.size())
        {
            stops.resize(uIndex + 1);
        }
        stops[uIndex] = {position, glm::vec4(r, g, b, a)};
        return EL_OK;
    }

    el_result_e el_effect_get_neon_color_stop(const el_effect_handle_t *fx, int32_t index,
                                              float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        LOG_I("fx=%p, index=%d, outPosition=%p, outR=%p, outG=%p, outB=%p, outA=%p", (void *)fx, index, (void *)outPosition, (void *)outR, (void *)outG, (void *)outB, (void *)outA);
        VALIDATE_FX(fx, "el_effect_get_neon_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_neon_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_neon_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_neon_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_neon_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_neon_color_stop");
        if (index < 0 || static_cast<size_t>(index) >= fx->config.neon.colorStops.size())
        {
            LOG_E("el_effect_get_neon_color_stop: index out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &s = fx->config.neon.colorStops[static_cast<size_t>(index)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        return EL_OK;
    }

    el_result_e el_effect_clear_neon_color_stops(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_clear_neon_color_stops");
        fx->config.neon.colorStops.clear();
        return EL_OK;
    }

    // --- Neon segment boosts ---

    el_result_e el_effect_set_neon_segment_boost_count(el_effect_handle_t *fx, int32_t count)
    {
        LOG_I("fx=%p, count=%d", (void *)fx, count);
        VALIDATE_FX(fx, "el_effect_set_neon_segment_boost_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_neon_segment_boost_count: negative count");
            return EL_ERR_INVALID_ARG;
        }
        fx->config.neon.segmentBoosts.resize(static_cast<size_t>(count));
        return EL_OK;
    }

    el_result_e el_effect_get_neon_segment_boost_count(const el_effect_handle_t *fx, int32_t *outCount)
    {
        LOG_I("fx=%p, outCount=%p", (void *)fx, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_neon_segment_boost_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_neon_segment_boost_count");
        *outCount = static_cast<int32_t>(fx->config.neon.segmentBoosts.size());
        return EL_OK;
    }

    el_result_e el_effect_set_neon_segment_boost(el_effect_handle_t *fx, int32_t index,
                                                 float position, float length, float boost)
    {
        LOG_I("fx=%p, index=%d, position=%f, length=%f, boost=%f", (void *)fx, index, position, length, boost);
        VALIDATE_FX(fx, "el_effect_set_neon_segment_boost");
        if (index < 0)
        {
            LOG_E("el_effect_set_neon_segment_boost: negative index");
            return EL_ERR_INVALID_ARG;
        }
        auto &boosts = fx->config.neon.segmentBoosts;
        size_t uIndex = static_cast<size_t>(index);
        if (uIndex >= boosts.size())
        {
            boosts.resize(uIndex + 1);
        }
        boosts[uIndex] = {position, length, boost};
        return EL_OK;
    }

    el_result_e el_effect_get_neon_segment_boost(const el_effect_handle_t *fx, int32_t index,
                                                 float *outPosition, float *outLength, float *outBoost)
    {
        LOG_I("fx=%p, index=%d, outPosition=%p, outLength=%p, outBoost=%p", (void *)fx, index, (void *)outPosition, (void *)outLength, (void *)outBoost);
        VALIDATE_FX(fx, "el_effect_get_neon_segment_boost");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_neon_segment_boost");
        VALIDATE_OUT_PTR(outLength, "el_effect_get_neon_segment_boost");
        VALIDATE_OUT_PTR(outBoost, "el_effect_get_neon_segment_boost");
        if (index < 0 || static_cast<size_t>(index) >= fx->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_neon_segment_boost: index out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &b = fx->config.neon.segmentBoosts[static_cast<size_t>(index)];
        *outPosition = b.position;
        *outLength = b.length;
        *outBoost = b.boost;
        return EL_OK;
    }

    el_result_e el_effect_clear_neon_segment_boosts(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_clear_neon_segment_boosts");
        fx->config.neon.segmentBoosts.clear();
        return EL_OK;
    }

    // --- Neon segment blend space + colour stops ---
    // Each SegmentBoost carries its own gradient (colorStops laid head-to-tail
    // across the segment's visible span) and a per-segment blendSpace.
    // Segments with no stops fall back to the base NeonConfig gradient.

    el_result_e el_effect_set_neon_segment_blend_space(el_effect_handle_t *fx,
                                                       int32_t segmentIndex, el_blend_space_e blendSpace)
    {
        LOG_I("fx=%p, segmentIndex=%d, blendSpace=%d", (void *)fx, segmentIndex, (int)blendSpace);
        VALIDATE_FX(fx, "el_effect_set_neon_segment_blend_space");
        if (segmentIndex < 0)
        {
            LOG_E("el_effect_set_neon_segment_blend_space: negative segmentIndex");
            return EL_ERR_INVALID_ARG;
        }
        auto &boosts = fx->config.neon.segmentBoosts;
        size_t uSeg = static_cast<size_t>(segmentIndex);
        if (uSeg >= boosts.size())
        {
            boosts.resize(uSeg + 1);
        }
        boosts[uSeg].blendSpace = static_cast<EdgeLighting::BlendSpace>(blendSpace);
        return EL_OK;
    }

    el_result_e el_effect_get_neon_segment_blend_space(const el_effect_handle_t *fx,
                                                       int32_t segmentIndex, el_blend_space_e *outBlendSpace)
    {
        LOG_I("fx=%p, segmentIndex=%d, outBlendSpace=%p", (void *)fx, segmentIndex, (void *)outBlendSpace);
        VALIDATE_FX(fx, "el_effect_get_neon_segment_blend_space");
        VALIDATE_OUT_PTR(outBlendSpace, "el_effect_get_neon_segment_blend_space");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= fx->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_neon_segment_blend_space: segmentIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        *outBlendSpace = static_cast<el_blend_space_e>(
            fx->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].blendSpace);
        return EL_OK;
    }

    el_result_e el_effect_set_neon_segment_color_stop_count(el_effect_handle_t *fx,
                                                            int32_t segmentIndex, int32_t count)
    {
        LOG_I("fx=%p, segmentIndex=%d, count=%d", (void *)fx, segmentIndex, count);
        VALIDATE_FX(fx, "el_effect_set_neon_segment_color_stop_count");
        if (segmentIndex < 0)
        {
            LOG_E("el_effect_set_neon_segment_color_stop_count: negative segmentIndex");
            return EL_ERR_INVALID_ARG;
        }
        if (count < 0)
        {
            LOG_E("el_effect_set_neon_segment_color_stop_count: negative count");
            return EL_ERR_INVALID_ARG;
        }
        auto &boosts = fx->config.neon.segmentBoosts;
        size_t uSeg = static_cast<size_t>(segmentIndex);
        if (uSeg >= boosts.size())
        {
            boosts.resize(uSeg + 1);
        }
        boosts[uSeg].colorStops.resize(static_cast<size_t>(count));
        return EL_OK;
    }

    el_result_e el_effect_get_neon_segment_color_stop_count(const el_effect_handle_t *fx,
                                                            int32_t segmentIndex, int32_t *outCount)
    {
        LOG_I("fx=%p, segmentIndex=%d, outCount=%p", (void *)fx, segmentIndex, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_neon_segment_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_neon_segment_color_stop_count");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= fx->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_neon_segment_color_stop_count: segmentIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        *outCount = static_cast<int32_t>(
            fx->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].colorStops.size());
        return EL_OK;
    }

    el_result_e el_effect_set_neon_segment_color_stop(el_effect_handle_t *fx,
                                                      int32_t segmentIndex, int32_t stopIndex,
                                                      float position, float r, float g, float b, float a)
    {
        LOG_I("fx=%p, segmentIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f",
              (void *)fx, segmentIndex, stopIndex, position, r, g, b, a);
        VALIDATE_FX(fx, "el_effect_set_neon_segment_color_stop");
        if (segmentIndex < 0 || stopIndex < 0)
        {
            LOG_E("el_effect_set_neon_segment_color_stop: negative index");
            return EL_ERR_INVALID_ARG;
        }
        auto &boosts = fx->config.neon.segmentBoosts;
        size_t uSeg = static_cast<size_t>(segmentIndex);
        if (uSeg >= boosts.size())
        {
            boosts.resize(uSeg + 1);
        }
        auto &stops = boosts[uSeg].colorStops;
        size_t uStop = static_cast<size_t>(stopIndex);
        if (uStop >= stops.size())
        {
            stops.resize(uStop + 1);
        }
        stops[uStop] = {position, glm::vec4(r, g, b, a)};
        return EL_OK;
    }

    el_result_e el_effect_get_neon_segment_color_stop(const el_effect_handle_t *fx,
                                                      int32_t segmentIndex, int32_t stopIndex,
                                                      float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        LOG_I("fx=%p, segmentIndex=%d, stopIndex=%d, outPosition=%p, outR=%p, outG=%p, outB=%p, outA=%p",
              (void *)fx, segmentIndex, stopIndex, (void *)outPosition, (void *)outR, (void *)outG, (void *)outB, (void *)outA);
        VALIDATE_FX(fx, "el_effect_get_neon_segment_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_neon_segment_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_neon_segment_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_neon_segment_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_neon_segment_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_neon_segment_color_stop");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= fx->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_get_neon_segment_color_stop: segmentIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &seg = fx->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)];
        if (stopIndex < 0 || static_cast<size_t>(stopIndex) >= seg.colorStops.size())
        {
            LOG_E("el_effect_get_neon_segment_color_stop: stopIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &s = seg.colorStops[static_cast<size_t>(stopIndex)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        return EL_OK;
    }

    el_result_e el_effect_clear_neon_segment_color_stops(el_effect_handle_t *fx, int32_t segmentIndex)
    {
        LOG_I("fx=%p, segmentIndex=%d", (void *)fx, segmentIndex);
        VALIDATE_FX(fx, "el_effect_clear_neon_segment_color_stops");
        if (segmentIndex < 0 || static_cast<size_t>(segmentIndex) >= fx->config.neon.segmentBoosts.size())
        {
            LOG_E("el_effect_clear_neon_segment_color_stops: segmentIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        fx->config.neon.segmentBoosts[static_cast<size_t>(segmentIndex)].colorStops.clear();
        return EL_OK;
    }

    // --- Neon arcs ---

    el_result_e el_effect_set_neon_arc_count(el_effect_handle_t *fx, int32_t count)
    {
        LOG_I("fx=%p, count=%d", (void *)fx, count);
        VALIDATE_FX(fx, "el_effect_set_neon_arc_count");
        if (count < 0)
        {
            LOG_E("el_effect_set_neon_arc_count: negative count");
            return EL_ERR_INVALID_ARG;
        }
        fx->config.neon.arcs.resize(static_cast<size_t>(count));
        return EL_OK;
    }

    el_result_e el_effect_get_neon_arc_count(const el_effect_handle_t *fx, int32_t *outCount)
    {
        LOG_I("fx=%p, outCount=%p", (void *)fx, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_neon_arc_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_neon_arc_count");
        *outCount = static_cast<int32_t>(fx->config.neon.arcs.size());
        return EL_OK;
    }

    el_result_e el_effect_set_neon_arc(el_effect_handle_t *fx, int32_t index,
                                       float start, float length, float intensity, el_blend_space_e blendSpace)
    {
        LOG_I("fx=%p, index=%d, start=%f, length=%f, intensity=%f, blendSpace=%d", (void *)fx, index, start, length, intensity, (int)blendSpace);
        VALIDATE_FX(fx, "el_effect_set_neon_arc");
        if (index < 0)
        {
            LOG_E("el_effect_set_neon_arc: negative index");
            return EL_ERR_INVALID_ARG;
        }
        auto &arcs = fx->config.neon.arcs;
        size_t uIndex = static_cast<size_t>(index);
        if (uIndex >= arcs.size())
        {
            arcs.resize(uIndex + 1);
        }
        arcs[uIndex].start = start;
        arcs[uIndex].length = length;
        arcs[uIndex].intensity = intensity;
        arcs[uIndex].blendSpace = static_cast<EdgeLighting::BlendSpace>(blendSpace);
        return EL_OK;
    }

    el_result_e el_effect_get_neon_arc(const el_effect_handle_t *fx, int32_t index,
                                       float *outStart, float *outLength, float *outIntensity,
                                       el_blend_space_e *outBlendSpace)
    {
        LOG_I("fx=%p, index=%d, outStart=%p, outLength=%p, outIntensity=%p, outBlendSpace=%p", (void *)fx, index, (void *)outStart, (void *)outLength, (void *)outIntensity, (void *)outBlendSpace);
        VALIDATE_FX(fx, "el_effect_get_neon_arc");
        VALIDATE_OUT_PTR(outStart, "el_effect_get_neon_arc");
        VALIDATE_OUT_PTR(outLength, "el_effect_get_neon_arc");
        VALIDATE_OUT_PTR(outIntensity, "el_effect_get_neon_arc");
        VALIDATE_OUT_PTR(outBlendSpace, "el_effect_get_neon_arc");
        if (index < 0 || static_cast<size_t>(index) >= fx->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_neon_arc: index out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &a = fx->config.neon.arcs[static_cast<size_t>(index)];
        *outStart = a.start;
        *outLength = a.length;
        *outIntensity = a.intensity;
        *outBlendSpace = static_cast<el_blend_space_e>(a.blendSpace);
        return EL_OK;
    }

    el_result_e el_effect_clear_neon_arcs(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_clear_neon_arcs");
        fx->config.neon.arcs.clear();
        return EL_OK;
    }

    // --- Neon arc colour stops ---

    el_result_e el_effect_set_neon_arc_color_stop_count(el_effect_handle_t *fx,
                                                        int32_t arcIndex, int32_t count)
    {
        LOG_I("fx=%p, arcIndex=%d, count=%d", (void *)fx, arcIndex, count);
        VALIDATE_FX(fx, "el_effect_set_neon_arc_color_stop_count");
        if (arcIndex < 0)
        {
            LOG_E("el_effect_set_neon_arc_color_stop_count: negative arcIndex");
            return EL_ERR_INVALID_ARG;
        }
        if (count < 0)
        {
            LOG_E("el_effect_set_neon_arc_color_stop_count: negative count");
            return EL_ERR_INVALID_ARG;
        }
        auto &arcs = fx->config.neon.arcs;
        size_t uArc = static_cast<size_t>(arcIndex);
        if (uArc >= arcs.size())
        {
            arcs.resize(uArc + 1);
        }
        arcs[uArc].colorStops.resize(static_cast<size_t>(count));
        return EL_OK;
    }

    el_result_e el_effect_get_neon_arc_color_stop_count(const el_effect_handle_t *fx,
                                                        int32_t arcIndex, int32_t *outCount)
    {
        LOG_I("fx=%p, arcIndex=%d, outCount=%p", (void *)fx, arcIndex, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_neon_arc_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_neon_arc_color_stop_count");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= fx->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_neon_arc_color_stop_count: arcIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        *outCount = static_cast<int32_t>(
            fx->config.neon.arcs[static_cast<size_t>(arcIndex)].colorStops.size());
        return EL_OK;
    }

    el_result_e el_effect_set_neon_arc_color_stop(el_effect_handle_t *fx,
                                                  int32_t arcIndex, int32_t stopIndex,
                                                  float position, float r, float g, float b, float a)
    {
        LOG_I("fx=%p, arcIndex=%d, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f", (void *)fx, arcIndex, stopIndex, position, r, g, b, a);
        VALIDATE_FX(fx, "el_effect_set_neon_arc_color_stop");
        if (arcIndex < 0 || stopIndex < 0)
        {
            LOG_E("el_effect_set_neon_arc_color_stop: negative index");
            return EL_ERR_INVALID_ARG;
        }
        auto &arcs = fx->config.neon.arcs;
        size_t uArc = static_cast<size_t>(arcIndex);
        if (uArc >= arcs.size())
        {
            arcs.resize(uArc + 1);
        }
        auto &stops = arcs[uArc].colorStops;
        size_t uStop = static_cast<size_t>(stopIndex);
        if (uStop >= stops.size())
        {
            stops.resize(uStop + 1);
        }
        stops[uStop] = {position, glm::vec4(r, g, b, a)};
        return EL_OK;
    }

    el_result_e el_effect_get_neon_arc_color_stop(const el_effect_handle_t *fx,
                                                  int32_t arcIndex, int32_t stopIndex,
                                                  float *outPosition, float *outR, float *outG, float *outB, float *outA)
    {
        LOG_I("fx=%p, arcIndex=%d, stopIndex=%d, outPosition=%p, outR=%p, outG=%p, outB=%p, outA=%p", (void *)fx, arcIndex, stopIndex, (void *)outPosition, (void *)outR, (void *)outG, (void *)outB, (void *)outA);
        VALIDATE_FX(fx, "el_effect_get_neon_arc_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_neon_arc_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_neon_arc_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_neon_arc_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_neon_arc_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_neon_arc_color_stop");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= fx->config.neon.arcs.size())
        {
            LOG_E("el_effect_get_neon_arc_color_stop: arcIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &arc = fx->config.neon.arcs[static_cast<size_t>(arcIndex)];
        if (stopIndex < 0 || static_cast<size_t>(stopIndex) >= arc.colorStops.size())
        {
            LOG_E("el_effect_get_neon_arc_color_stop: stopIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        const auto &s = arc.colorStops[static_cast<size_t>(stopIndex)];
        *outPosition = s.position;
        *outR = s.color.r;
        *outG = s.color.g;
        *outB = s.color.b;
        *outA = s.color.a;
        return EL_OK;
    }

    el_result_e el_effect_clear_neon_arc_color_stops(el_effect_handle_t *fx, int32_t arcIndex)
    {
        LOG_I("fx=%p, arcIndex=%d", (void *)fx, arcIndex);
        VALIDATE_FX(fx, "el_effect_clear_neon_arc_color_stops");
        if (arcIndex < 0 || static_cast<size_t>(arcIndex) >= fx->config.neon.arcs.size())
        {
            LOG_E("el_effect_clear_neon_arc_color_stops: arcIndex out of range");
            return EL_ERR_INVALID_ARG;
        }
        fx->config.neon.arcs[static_cast<size_t>(arcIndex)].colorStops.clear();
        return EL_OK;
    }

    // --- Optimized neon ---

    el_result_e el_effect_set_optimized_neon_enabled(el_effect_handle_t *fx, int enabled)
    {
        LOG_I("fx=%p, enabled=%d", (void *)fx, enabled);
        VALIDATE_FX(fx, "el_effect_set_optimized_neon_enabled");
        fx->config.optimizedNeon.enable = enabled != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_optimized_neon_enabled(const el_effect_handle_t *fx, int *outEnabled)
    {
        LOG_I("fx=%p, outEnabled=%p", (void *)fx, (void *)outEnabled);
        VALIDATE_FX(fx, "el_effect_get_optimized_neon_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_optimized_neon_enabled");
        *outEnabled = fx->config.optimizedNeon.enable ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_optimized_neon_resolution_scale(el_effect_handle_t *fx, float scale)
    {
        LOG_I("fx=%p, scale=%f", (void *)fx, scale);
        VALIDATE_FX(fx, "el_effect_set_optimized_neon_resolution_scale");
        fx->config.optimizedNeon.resolutionScale = scale;
        return EL_OK;
    }

    el_result_e el_effect_get_optimized_neon_resolution_scale(const el_effect_handle_t *fx, float *outScale)
    {
        LOG_I("fx=%p, outScale=%p", (void *)fx, (void *)outScale);
        VALIDATE_FX(fx, "el_effect_get_optimized_neon_resolution_scale");
        VALIDATE_OUT_PTR(outScale, "el_effect_get_optimized_neon_resolution_scale");
        *outScale = fx->config.optimizedNeon.resolutionScale;
        return EL_OK;
    }

    el_result_e el_effect_set_optimized_neon_num_samples(el_effect_handle_t *fx, int32_t samples)
    {
        LOG_I("fx=%p, samples=%d", (void *)fx, samples);
        VALIDATE_FX(fx, "el_effect_set_optimized_neon_num_samples");
        fx->config.optimizedNeon.numSamples = samples;
        return EL_OK;
    }

    el_result_e el_effect_get_optimized_neon_num_samples(const el_effect_handle_t *fx, int32_t *outSamples)
    {
        LOG_I("fx=%p, outSamples=%p", (void *)fx, (void *)outSamples);
        VALIDATE_FX(fx, "el_effect_get_optimized_neon_num_samples");
        VALIDATE_OUT_PTR(outSamples, "el_effect_get_optimized_neon_num_samples");
        *outSamples = fx->config.optimizedNeon.numSamples;
        return EL_OK;
    }

    el_result_e el_effect_set_optimized_neon_gradient_lut_size(el_effect_handle_t *fx, int32_t size)
    {
        LOG_I("fx=%p, size=%d", (void *)fx, size);
        VALIDATE_FX(fx, "el_effect_set_optimized_neon_gradient_lut_size");
        fx->config.optimizedNeon.gradientLutSize = size;
        return EL_OK;
    }

    el_result_e el_effect_get_optimized_neon_gradient_lut_size(const el_effect_handle_t *fx, int32_t *outSize)
    {
        LOG_I("fx=%p, outSize=%p", (void *)fx, (void *)outSize);
        VALIDATE_FX(fx, "el_effect_get_optimized_neon_gradient_lut_size");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_optimized_neon_gradient_lut_size");
        *outSize = fx->config.optimizedNeon.gradientLutSize;
        return EL_OK;
    }

    el_result_e el_effect_set_optimized_neon_show_half_res(el_effect_handle_t *fx, int show)
    {
        LOG_I("fx=%p, show=%d", (void *)fx, show);
        VALIDATE_FX(fx, "el_effect_set_optimized_neon_show_half_res");
        fx->config.optimizedNeon.showHalfRes = show != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_optimized_neon_show_half_res(const el_effect_handle_t *fx, int *outShow)
    {
        LOG_I("fx=%p, outShow=%p", (void *)fx, (void *)outShow);
        VALIDATE_FX(fx, "el_effect_get_optimized_neon_show_half_res");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_optimized_neon_show_half_res");
        *outShow = fx->config.optimizedNeon.showHalfRes ? 1 : 0;
        return EL_OK;
    }

    // --- Wireframe ---

    el_result_e el_effect_set_wireframe_enabled(el_effect_handle_t *fx, int enabled)
    {
        LOG_I("fx=%p, enabled=%d", (void *)fx, enabled);
        VALIDATE_FX(fx, "el_effect_set_wireframe_enabled");
        fx->config.wireframe.enable = enabled != 0;
        return EL_OK;
    }

    el_result_e el_effect_get_wireframe_enabled(const el_effect_handle_t *fx, int *outEnabled)
    {
        LOG_I("fx=%p, outEnabled=%p", (void *)fx, (void *)outEnabled);
        VALIDATE_FX(fx, "el_effect_get_wireframe_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_wireframe_enabled");
        *outEnabled = fx->config.wireframe.enable ? 1 : 0;
        return EL_OK;
    }

    el_result_e el_effect_set_wireframe_color(el_effect_handle_t *fx,
                                              float r, float g, float b, float a)
    {
        LOG_I("fx=%p, r=%f, g=%f, b=%f, a=%f", (void *)fx, r, g, b, a);
        VALIDATE_FX(fx, "el_effect_set_wireframe_color");
        fx->config.wireframe.color = glm::vec4(r, g, b, a);
        return EL_OK;
    }

    el_result_e el_effect_get_wireframe_color(const el_effect_handle_t *fx,
                                              float *outR, float *outG, float *outB, float *outA)
    {
        LOG_I("fx=%p, outR=%p, outG=%p, outB=%p, outA=%p", (void *)fx, (void *)outR, (void *)outG, (void *)outB, (void *)outA);
        VALIDATE_FX(fx, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_wireframe_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_wireframe_color");
        *outR = fx->config.wireframe.color.r;
        *outG = fx->config.wireframe.color.g;
        *outB = fx->config.wireframe.color.b;
        *outA = fx->config.wireframe.color.a;
        return EL_OK;
    }

    // ==========================================================================
    // Effect lifecycle
    // ==========================================================================

    el_effect_handle_t *el_effect_create(void)
    {
        LOG_I("called");
        try
        {
            auto *fx = new el_effect_handle_impl();
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::WireframeRenderer>());
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::NeonRenderer>());
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::NeonOptimizedRenderer>());
            return fx;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_effect_destroy(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        if (!fx)
        {
            return EL_OK;
        }
        delete fx;
        return EL_OK;
    }

    el_result_e el_effect_initialize(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_initialize");
        try
        {
            if (!fx->effect.Initialize())
            {
                LOG_E("el_effect_initialize: renderer initialisation failed");
                return EL_ERR_INIT_FAILED;
            }
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    el_result_e el_effect_capture(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_capture");
        try
        {
            fx->config = fx->effect.GetConfig();
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    el_result_e el_effect_update(el_effect_handle_t *fx, float deltaTime)
    {
        LOG_I("fx=%p, deltaTime=%f", (void *)fx, deltaTime);
        VALIDATE_FX(fx, "el_effect_update");
        try
        {
            fx->effect.SetConfig(fx->config);
            fx->effect.Update(deltaTime);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    el_result_e el_effect_render(el_effect_handle_t *fx,
                                 int32_t viewportWidth, int32_t viewportHeight)
    {
        LOG_I("fx=%p, viewportWidth=%d, viewportHeight=%d", (void *)fx, viewportWidth, viewportHeight);
        VALIDATE_FX(fx, "el_effect_render");
        try
        {
            fx->effect.Render(viewportWidth, viewportHeight);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    // --- Animation attachment ---

    el_result_e el_effect_attach_animation(el_effect_handle_t *fx, el_animation_handle_t *anim)
    {
        LOG_I("fx=%p, anim=%p", (void *)fx, (void *)anim);
        VALIDATE_FX(fx, "el_effect_attach_animation");
        VALIDATE_ANM(anim, "el_effect_attach_animation");
        if (anim->ptr)
        {
            fx->effect.Attach(anim->ptr);
        }
        return EL_OK;
    }

    el_result_e el_effect_detach_animation(el_effect_handle_t *fx, el_animation_handle_t *anim)
    {
        LOG_I("fx=%p, anim=%p", (void *)fx, (void *)anim);
        VALIDATE_FX(fx, "el_effect_detach_animation");
        VALIDATE_ANM(anim, "el_effect_detach_animation");
        if (anim->ptr)
        {
            fx->effect.Detach(anim->ptr);
        }
        return EL_OK;
    }

    el_result_e el_effect_detach_all_animations(el_effect_handle_t *fx)
    {
        LOG_I("fx=%p", (void *)fx);
        VALIDATE_FX(fx, "el_effect_detach_all_animations");
        fx->effect.GetAnimationManager().DetachAll();
        return EL_OK;
    }

    el_result_e el_effect_get_animation_count(el_effect_handle_t *fx, int32_t *outCount)
    {
        LOG_I("fx=%p, outCount=%p", (void *)fx, (void *)outCount);
        VALIDATE_FX(fx, "el_effect_get_animation_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_animation_count");
        *outCount = static_cast<int32_t>(fx->effect.GetAnimationManager().GetCount());
        return EL_OK;
    }

    el_result_e el_effect_contains_animation(el_effect_handle_t *fx,
                                             el_animation_handle_t *anim, int *outContains)
    {
        LOG_I("fx=%p, anim=%p, outContains=%p", (void *)fx, (void *)anim, (void *)outContains);
        VALIDATE_FX(fx, "el_effect_contains_animation");
        VALIDATE_ANM(anim, "el_effect_contains_animation");
        VALIDATE_OUT_PTR(outContains, "el_effect_contains_animation");
        *outContains = (anim->ptr && fx->effect.GetAnimationManager().Contains(anim->ptr)) ? 1 : 0;
        return EL_OK;
    }

    // ==========================================================================
    // Animation lifecycle
    // ==========================================================================

    el_animation_handle_t *el_animation_create(el_animation_preset_e preset)
    {
        LOG_I("preset=%d", (int)preset);
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
            return new el_animation_handle_t{std::move(a)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_animation_destroy(el_animation_handle_t *anim)
    {
        LOG_I("anim=%p", (void *)anim);
        if (!anim)
        {
            return EL_OK;
        }
        delete anim;
        return EL_OK;
    }

    // --- Parametric factories ---

    el_animation_handle_t *el_animation_create_intensity_pulse(float duration,
                                                               float min, float max)
    {
        LOG_I("duration=%f, min=%f, max=%f", duration, min, max);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::IntensityPulse>(duration, min, max)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_intensity_strobe(float duration,
                                                                float offIntensity, float onIntensity)
    {
        LOG_I("duration=%f, offIntensity=%f, onIntensity=%f", duration, offIntensity, onIntensity);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::IntensityStrobe>(duration, offIntensity, onIntensity)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_intensity_fade_in(float target,
                                                                 float duration, el_easing_e easing)
    {
        LOG_I("target=%f, duration=%f, easing=%d", target, duration, (int)easing);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::IntensityFadeIn>(target, duration, toEasing(easing))};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_intensity_fade_out(float start,
                                                                  float duration, el_easing_e easing)
    {
        LOG_I("start=%f, duration=%f, easing=%d", start, duration, (int)easing);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::IntensityFadeOut>(start, duration, toEasing(easing))};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_glow_radius_breath(float duration,
                                                                  float minRadius, float maxRadius)
    {
        LOG_I("duration=%f, minRadius=%f, maxRadius=%f", duration, minRadius, maxRadius);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::GlowRadiusBreath>(duration, minRadius, maxRadius)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_bloom_pulse(float duration,
                                                           float min, float max)
    {
        LOG_I("duration=%f, min=%f, max=%f", duration, min, max);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::BloomPulse>(duration, min, max)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_hue_rotation_reverse(float baseRate,
                                                                    float duration)
    {
        LOG_I("baseRate=%f, duration=%f", baseRate, duration);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::HueRotationReverse>(baseRate, duration)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_hue_rotation_ease_reverse(float maxRate,
                                                                         float duration)
    {
        LOG_I("maxRate=%f, duration=%f", maxRate, duration);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::HueRotationEaseReverse>(maxRate, duration)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_segment_travel(float duration,
                                                              float length, float boost)
    {
        LOG_I("duration=%f, length=%f, boost=%f", duration, length, boost);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::SegmentTravel>(duration, length, boost)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_segment_bounce(float duration,
                                                              float length, float boost)
    {
        LOG_I("duration=%f, length=%f, boost=%f", duration, length, boost);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::SegmentBounce>(duration, length, boost)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_outline_tracer(float duration,
                                                              el_easing_e easing)
    {
        LOG_I("duration=%f, easing=%d", duration, (int)easing);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::OutlineTracer>(duration, toEasing(easing))};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_arc_wipe(float duration,
                                                        float startPos, float endPos, float maxLength,
                                                        el_easing_e easing)
    {
        LOG_I("duration=%f, startPos=%f, endPos=%f, maxLength=%f, easing=%d", duration, startPos, endPos, maxLength, (int)easing);
        try
        {
            return new el_animation_handle_t{std::make_shared<EdgeLighting::ArcWipe>(
                duration, startPos, endPos, maxLength, toEasing(easing))};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    // --- Stateful lifecycle ---

    el_result_e el_animation_play(el_animation_handle_t *anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANM(anim, "el_animation_play");
        if (anim->ptr)
        {
            anim->ptr->Play();
        }
        return EL_OK;
    }

    el_result_e el_animation_pause(el_animation_handle_t *anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANM(anim, "el_animation_pause");
        if (anim->ptr)
        {
            anim->ptr->Pause();
        }
        return EL_OK;
    }

    el_result_e el_animation_stop(el_animation_handle_t *anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANM(anim, "el_animation_stop");
        if (anim->ptr)
        {
            anim->ptr->Stop();
        }
        return EL_OK;
    }

    el_result_e el_animation_reset(el_animation_handle_t *anim, el_effect_handle_t *fx)
    {
        LOG_I("anim=%p, fx=%p", (void *)anim, (void *)fx);
        VALIDATE_ANM(anim, "el_animation_reset");
        VALIDATE_FX(fx, "el_animation_reset");
        try
        {
            anim->ptr->Reset(fx->config);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    el_result_e el_animation_update(el_animation_handle_t *anim, float dt)
    {
        LOG_I("anim=%p, dt=%f", (void *)anim, dt);
        VALIDATE_ANM(anim, "el_animation_update");
        if (anim->ptr)
        {
            anim->ptr->Update(dt);
        }
        return EL_OK;
    }

    el_result_e el_animation_apply(el_animation_handle_t *anim, el_effect_handle_t *fx)
    {
        LOG_I("anim=%p, fx=%p", (void *)anim, (void *)fx);
        VALIDATE_ANM(anim, "el_animation_apply");
        VALIDATE_FX(fx, "el_animation_apply");
        try
        {
            anim->ptr->Apply(fx->config);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    // --- Elapsed / state ---

    el_result_e el_animation_get_state(el_animation_handle_t *anim, el_animation_state_e *outState)
    {
        LOG_I("anim=%p, outState=%p", (void *)anim, (void *)outState);
        VALIDATE_ANM(anim, "el_animation_get_state");
        VALIDATE_OUT_PTR(outState, "el_animation_get_state");
        if (!anim->ptr)
        {
            *outState = EL_ANIM_STATE_STOPPED;
            return EL_OK;
        }
        using ES = EdgeLighting::AnimationState;
        switch (anim->ptr->GetState())
        {
        case ES::PLAYING:
            *outState = EL_ANIM_STATE_PLAYING;
            return EL_OK;
        case ES::PAUSED:
            *outState = EL_ANIM_STATE_PAUSED;
            return EL_OK;
        case ES::STOPPED:
        default:
            *outState = EL_ANIM_STATE_STOPPED;
            return EL_OK;
        }
    }

    el_result_e el_animation_get_elapsed(el_animation_handle_t *anim, float *outElapsed)
    {
        LOG_I("anim=%p, outElapsed=%p", (void *)anim, (void *)outElapsed);
        VALIDATE_ANM(anim, "el_animation_get_elapsed");
        VALIDATE_OUT_PTR(outElapsed, "el_animation_get_elapsed");
        *outElapsed = anim->ptr ? anim->ptr->GetElapsed() : 0.0f;
        return EL_OK;
    }

    el_result_e el_animation_set_elapsed(el_animation_handle_t *anim, float elapsed)
    {
        LOG_I("anim=%p, elapsed=%f", (void *)anim, elapsed);
        VALIDATE_ANM(anim, "el_animation_set_elapsed");
        if (anim->ptr)
        {
            anim->ptr->SetElapsed(elapsed);
        }
        return EL_OK;
    }

    // --- End action ---

    el_result_e el_animation_get_end_action(el_animation_handle_t *anim, el_end_action_e *outAction)
    {
        LOG_I("anim=%p, outAction=%p", (void *)anim, (void *)outAction);
        VALIDATE_ANM(anim, "el_animation_get_end_action");
        VALIDATE_OUT_PTR(outAction, "el_animation_get_end_action");
        *outAction = anim->ptr ? fromEndAction(anim->ptr->GetEndAction()) : EL_END_ACTION_HOLD_CURRENT;
        return EL_OK;
    }

    el_result_e el_animation_set_end_action(el_animation_handle_t *anim, el_end_action_e action)
    {
        LOG_I("anim=%p, action=%d", (void *)anim, (int)action);
        VALIDATE_ANM(anim, "el_animation_set_end_action");
        if (anim->ptr)
        {
            anim->ptr->SetEndAction(toEndAction(action));
        }
        return EL_OK;
    }

    el_result_e el_animation_capture_baseline(el_animation_handle_t *anim, const el_effect_handle_t *fx)
    {
        LOG_I("anim=%p, fx=%p", (void *)anim, (void *)fx);
        VALIDATE_ANM(anim, "el_animation_capture_baseline");
        VALIDATE_FX(fx, "el_animation_capture_baseline");
        try
        {
            if (anim->ptr)
            {
                anim->ptr->CaptureBaseline(fx->config);
            }
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    // --- Playback mode ---

    el_result_e el_animation_get_playback_mode(el_animation_handle_t *anim, el_playback_mode_e *outMode)
    {
        LOG_I("anim=%p, outMode=%p", (void *)anim, (void *)outMode);
        VALIDATE_ANM(anim, "el_animation_get_playback_mode");
        VALIDATE_OUT_PTR(outMode, "el_animation_get_playback_mode");
        *outMode = anim->ptr ? fromPlaybackMode(anim->ptr->GetPlaybackMode()) : EL_PLAYBACK_LOOP;
        return EL_OK;
    }

    el_result_e el_animation_set_playback_mode(el_animation_handle_t *anim, el_playback_mode_e mode)
    {
        LOG_I("anim=%p, mode=%d", (void *)anim, (int)mode);
        VALIDATE_ANM(anim, "el_animation_set_playback_mode");
        if (anim->ptr)
        {
            anim->ptr->SetPlaybackMode(toPlaybackMode(mode));
        }
        return EL_OK;
    }

    // --- Duration ---

    el_result_e el_animation_get_duration(el_animation_handle_t *anim, float *outSeconds)
    {
        LOG_I("anim=%p, outSeconds=%p", (void *)anim, (void *)outSeconds);
        VALIDATE_ANM(anim, "el_animation_get_duration");
        VALIDATE_OUT_PTR(outSeconds, "el_animation_get_duration");
        *outSeconds = anim->ptr ? anim->ptr->GetDuration() : 0.0f;
        return EL_OK;
    }

    el_result_e el_animation_set_duration(el_animation_handle_t *anim, float seconds)
    {
        LOG_I("anim=%p, seconds=%f", (void *)anim, seconds);
        VALIDATE_ANM(anim, "el_animation_set_duration");
        if (anim->ptr)
        {
            anim->ptr->SetDuration(seconds);
        }
        return EL_OK;
    }

    // --- Speed ---

    el_result_e el_animation_get_speed(el_animation_handle_t *anim, float *outSpeed)
    {
        LOG_I("anim=%p, outSpeed=%p", (void *)anim, (void *)outSpeed);
        VALIDATE_ANM(anim, "el_animation_get_speed");
        VALIDATE_OUT_PTR(outSpeed, "el_animation_get_speed");
        *outSpeed = anim->ptr ? anim->ptr->GetSpeed() : 1.0f;
        return EL_OK;
    }

    el_result_e el_animation_set_speed(el_animation_handle_t *anim, float speed)
    {
        LOG_I("anim=%p, speed=%f", (void *)anim, speed);
        VALIDATE_ANM(anim, "el_animation_set_speed");
        if (anim->ptr)
        {
            anim->ptr->SetSpeed(speed);
        }
        return EL_OK;
    }

    // --- Callbacks ---

    el_result_e el_animation_set_on_complete_callback(el_animation_handle_t *anim,
                                                      el_animation_on_completed_callback callback, void *userData)
    {
        LOG_I("anim=%p, callback=%s, userData=%p", (void *)anim, callback, userData);
        VALIDATE_ANM(anim, "el_animation_set_on_complete_callback");
        if (!anim->ptr)
        {
            return EL_OK;
        }
        if (!callback)
        {
            anim->ptr->OnComplete = nullptr;
            return EL_OK;
        }
        anim->ptr->OnComplete = [callback, userData]()
        {
            callback(userData);
        };
        return EL_OK;
    }

    el_result_e el_animation_set_on_state_changed_callback(el_animation_handle_t *anim,
                                                           el_animation_on_state_changed_callback callback,
                                                           void *userData)
    {
        LOG_I("anim=%p, callback=%s, userData=%p", (void *)anim, callback, userData);
        VALIDATE_ANM(anim, "el_animation_set_on_state_changed_callback");
        if (!anim->ptr)
        {
            return EL_OK;
        }
        if (!callback)
        {
            anim->ptr->OnStateChanged = nullptr;
            return EL_OK;
        }
        anim->ptr->OnStateChanged = [callback, userData](EdgeLighting::AnimationState prev,
                                                         EdgeLighting::AnimationState now)
        {
            callback(static_cast<int>(prev), static_cast<int>(now), userData);
        };
        return EL_OK;
    }

    // ==========================================================================
    // Field-bound animation
    // ==========================================================================

    el_animation_handle_t *el_animation_from_modulator(el_config_field_e field,
                                                       el_modulator_handle_t *mod)
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
            return new el_animation_handle_t{std::move(a)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_animation_handle_t *el_animation_create_field_bound(void)
    {
        LOG_I("called");
        try
        {
            auto a = std::make_shared<EdgeLighting::FieldBoundAnimation>();
            return new el_animation_handle_t{std::move(a)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_animation_add_field(el_animation_handle_t *anim,
                                       el_config_field_e field, el_modulator_handle_t *mod)
    {
        LOG_I("anim=%p, field=%d, mod=%p", (void *)anim, (int)field, (void *)mod);
        VALIDATE_ANM(anim, "el_animation_add_field");
        VALIDATE_MOD(mod, "el_animation_add_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_field: animation is not a FieldBoundAnimation");
            return EL_ERR_INVALID_ARG;
        }
        fb->AddField(toAnimatableField(field), mod->ptr);
        return EL_OK;
    }

    el_result_e el_animation_add_segment_field(el_animation_handle_t *anim,
                                               int32_t index, el_segment_field_e field, el_modulator_handle_t *mod)
    {
        LOG_I("anim=%p, index=%d, field=%d, mod=%p", (void *)anim, index, (int)field, (void *)mod);
        VALIDATE_ANM(anim, "el_animation_add_segment_field");
        VALIDATE_MOD(mod, "el_animation_add_segment_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_segment_field: animation is not a FieldBoundAnimation");
            return EL_ERR_INVALID_ARG;
        }
        fb->AddSegmentField(static_cast<size_t>(index),
                            static_cast<EdgeLighting::SegmentField>(field), mod->ptr);
        return EL_OK;
    }

    el_result_e el_animation_add_arc_field(el_animation_handle_t *anim,
                                           int32_t index, el_arc_field_e field, el_modulator_handle_t *mod)
    {
        LOG_I("anim=%p, index=%d, field=%d, mod=%p", (void *)anim, index, (int)field, (void *)mod);
        VALIDATE_ANM(anim, "el_animation_add_arc_field");
        VALIDATE_MOD(mod, "el_animation_add_arc_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_arc_field: animation is not a FieldBoundAnimation");
            return EL_ERR_INVALID_ARG;
        }
        fb->AddArcField(static_cast<size_t>(index),
                        static_cast<EdgeLighting::ArcField>(field), mod->ptr);
        return EL_OK;
    }

    el_result_e el_animation_add_arc_stop_field(el_animation_handle_t *anim,
                                                int32_t arcIdx, int32_t stopIdx, el_color_stop_field_e field,
                                                el_modulator_handle_t *mod)
    {
        LOG_I("anim=%p, arcIdx=%d, stopIdx=%d, field=%d, mod=%p", (void *)anim, arcIdx, stopIdx, (int)field, (void *)mod);
        VALIDATE_ANM(anim, "el_animation_add_arc_stop_field");
        VALIDATE_MOD(mod, "el_animation_add_arc_stop_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_arc_stop_field: animation is not a FieldBoundAnimation");
            return EL_ERR_INVALID_ARG;
        }
        fb->AddArcStopField(static_cast<size_t>(arcIdx), static_cast<size_t>(stopIdx),
                            static_cast<EdgeLighting::ColorStopField>(field), mod->ptr);
        return EL_OK;
    }

    el_result_e el_animation_add_segment_stop_field(el_animation_handle_t *anim,
                                                    int32_t segIdx, int32_t stopIdx, el_color_stop_field_e field,
                                                    el_modulator_handle_t *mod)
    {
        LOG_I("anim=%p, segIdx=%d, stopIdx=%d, field=%d, mod=%p", (void *)anim, segIdx, stopIdx, (int)field, (void *)mod);
        VALIDATE_ANM(anim, "el_animation_add_segment_stop_field");
        VALIDATE_MOD(mod, "el_animation_add_segment_stop_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_segment_stop_field: animation is not a FieldBoundAnimation");
            return EL_ERR_INVALID_ARG;
        }
        fb->AddStopField(static_cast<size_t>(segIdx), static_cast<size_t>(stopIdx),
                         static_cast<EdgeLighting::ColorStopField>(field), mod->ptr);
        return EL_OK;
    }

    // ==========================================================================
    // Modulator factories
    // ==========================================================================

    el_modulator_handle_t *el_modulator_create_constant(float value)
    {
        LOG_I("value=%f", value);
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Constant>(value)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t *el_modulator_create_oscillator(float frequency,
                                                          float min, float max, float phase, el_waveform_e waveform)
    {
        LOG_I("frequency=%f, min=%f, max=%f, phase=%f, waveform=%d", frequency, min, max, phase, (int)waveform);
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Oscillator>(
                frequency, min, max, phase, toWaveform(waveform))};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t *el_modulator_create_ease(float from, float to,
                                                    float duration, el_easing_e easing, int loop)
    {
        LOG_I("from=%f, to=%f, duration=%f, easing=%d, loop=%d", from, to, duration, (int)easing, loop);
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Ease>(
                from, to, duration, toEasing(easing), loop != 0)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t *el_modulator_create_sequence(int loop)
    {
        LOG_I("loop=%d", loop);
        try
        {
            auto seq = std::make_shared<EdgeLighting::Sequence>();
            seq->SetLoop(loop != 0);
            return new el_modulator_handle_t{std::move(seq)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_sequence_append(el_modulator_handle_t *seq,
                                             el_modulator_handle_t *segment, float duration)
    {
        LOG_I("seq=%p, segment=%p, duration=%f", (void *)seq, (void *)segment, duration);
        VALIDATE_MOD(seq, "el_modulator_sequence_append");
        VALIDATE_MOD(segment, "el_modulator_sequence_append");
        auto *s = dynamic_cast<EdgeLighting::Sequence *>(seq->ptr.get());
        if (!s)
        {
            LOG_E("el_modulator_sequence_append: seq is not a Sequence");
            return EL_ERR_INVALID_ARG;
        }
        s->Append(segment->ptr, duration);
        return EL_OK;
    }

    el_modulator_handle_t *el_modulator_create_multiplier(el_modulator_handle_t *a,
                                                          el_modulator_handle_t *b)
    {
        LOG_I("a=%p, b=%p", (void *)a, (void *)b);
        if (!a || !b)
        {
            LOG_E("el_modulator_create_multiplier: null arg");
            return nullptr;
        }
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Multiplier>(a->ptr, b->ptr)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t *el_modulator_create_adder(el_modulator_handle_t *a,
                                                     el_modulator_handle_t *b)
    {
        LOG_I("a=%p, b=%p", (void *)a, (void *)b);
        if (!a || !b)
        {
            LOG_E("el_modulator_create_adder: null arg");
            return nullptr;
        }
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Adder>(a->ptr, b->ptr)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t *el_modulator_create_remap(el_modulator_handle_t *inner,
                                                     float outMin, float outMax)
    {
        LOG_I("inner=%p, outMin=%f, outMax=%f", (void *)inner, outMin, outMax);
        if (!inner)
        {
            LOG_E("el_modulator_create_remap: inner is null");
            return nullptr;
        }
        try
        {
            return new el_modulator_handle_t{std::make_shared<EdgeLighting::Remap>(inner->ptr, outMin, outMax)};
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_destroy(el_modulator_handle_t *mod)
    {
        LOG_I("mod=%p", (void *)mod);
        if (!mod)
        {
            return EL_OK;
        }
        delete mod;
        return EL_OK;
    }

    el_result_e el_modulator_evaluate(el_modulator_handle_t *mod, float time, float *outValue)
    {
        LOG_I("mod=%p, time=%f, outValue=%p", (void *)mod, time, (void *)outValue);
        VALIDATE_MOD(mod, "el_modulator_evaluate");
        VALIDATE_OUT_PTR(outValue, "el_modulator_evaluate");
        if (!mod->ptr)
        {
            *outValue = 0.0f;
            return EL_OK;
        }
        *outValue = mod->ptr->Evaluate(time);
        return EL_OK;
    }

} // extern "C"
