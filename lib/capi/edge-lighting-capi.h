/**
 * @file edge-lighting-capi.h
 * @brief Flat C ABI over the EdgeLighting C++ renderer for FFI / C# interop.
 */
#ifndef _EDGE_LIGHTING_CAPI_H_
#define _EDGE_LIGHTING_CAPI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* --------------------------------------------------------------------------
 * Export / visibility
 * ------------------------------------------------------------------------ */
#if defined(_WIN32)
#define EL_API __declspec(dllexport)
#else
#define EL_API __attribute__((visibility("default")))
#endif

    /* --------------------------------------------------------------------------
     * Result codes
     * ------------------------------------------------------------------------ */
    typedef enum el_result_e
    {
        EL_OK = 0,
        EL_ERR_NULL_ARG = 1,    /**< A required pointer argument was null. */
        EL_ERR_INIT_FAILED = 2, /**< Renderer/GL initialisation failed. */
        EL_ERR_EXCEPTION = 3,   /**< A C++ exception was caught at the ABI boundary. */
        EL_ERR_INVALID_ARG = 4  /**< A non-null arg was out of range or the wrong shape. */
    } el_result_e;

    /* --------------------------------------------------------------------------
     * Enums
     * ------------------------------------------------------------------------ */
    typedef enum el_winding_e
    {
        EL_WINDING_CLOCKWISE = 0,
        EL_WINDING_COUNTER_CLOCKWISE = 1
    } el_winding_e;

    typedef enum el_glow_side_e
    {
        EL_GLOW_SIDE_BOTH = 0,
        EL_GLOW_SIDE_INSIDE = 1,
        EL_GLOW_SIDE_OUTSIDE = 2
    } el_glow_side_e;

    typedef enum el_blend_space_e
    {
        EL_BLEND_SPACE_RGB = 0,
        EL_BLEND_SPACE_HSV = 1,
        EL_BLEND_SPACE_HSL = 2
    } el_blend_space_e;

    typedef enum el_playback_mode_e
    {
        EL_PLAYBACK_LOOP = 0,
        EL_PLAYBACK_ONE_SHOT = 1
    } el_playback_mode_e;

    typedef enum el_animation_state_e
    {
        EL_ANIM_STATE_STOPPED = 0,
        EL_ANIM_STATE_PLAYING = 1,
        EL_ANIM_STATE_PAUSED = 2
    } el_animation_state_e;

    typedef enum el_end_action_e
    {
        EL_END_ACTION_HOLD_CURRENT = 0,
        EL_END_ACTION_HOLD_END = 1,
        EL_END_ACTION_HOLD_START = 2,
        EL_END_ACTION_RESTORE = 3
    } el_end_action_e;

    typedef enum el_easing_e
    {
        EL_EASE_LINEAR = 0,
        EL_EASE_IN_QUAD = 1,
        EL_EASE_OUT_QUAD = 2,
        EL_EASE_INOUT_QUAD = 3,
        EL_EASE_IN_CUBIC = 4,
        EL_EASE_OUT_CUBIC = 5,
        EL_EASE_INOUT_CUBIC = 6,
        EL_EASE_IN_SINE = 7,
        EL_EASE_OUT_SINE = 8,
        EL_EASE_INOUT_SINE = 9,
        EL_EASE_IN_EXPO = 10,
        EL_EASE_OUT_EXPO = 11,
        EL_EASE_INOUT_EXPO = 12
    } el_easing_e;

    typedef enum el_animation_preset_e
    {
        EL_ANIM_NONE = 0,
        EL_ANIM_BREATHING = 1,
        EL_ANIM_STROBE = 2,
        EL_ANIM_HEARTBEAT = 3,
        EL_ANIM_SHIMMER = 4,
        EL_ANIM_AURORA = 5,
        EL_ANIM_REVERSE_SWEEP = 6,
        EL_ANIM_FADE_IN = 7,
        EL_ANIM_SEGMENT_TRAVEL = 8,
        EL_ANIM_SEGMENT_BOUNCE = 9,
        EL_ANIM_COMET = 10,
        EL_ANIM_OUTLINE_TRACER = 11,
        EL_ANIM_FADE_OUT = 12,
        EL_ANIM_HUE_REVERSE = 13,
        EL_ANIM_ARC_WIPE = 14
    } el_animation_preset_e;

    typedef enum el_waveform_e
    {
        EL_WAVE_SINE = 0,
        EL_WAVE_TRIANGLE = 1,
        EL_WAVE_SQUARE = 2,
        EL_WAVE_SAWTOOTH = 3
    } el_waveform_e;

    typedef enum el_config_field_e
    {
        EL_FIELD_NEON_INTENSITY = 0,
        EL_FIELD_NEON_LINE_WIDTH = 1,
        EL_FIELD_NEON_GLOW_RADIUS = 2,
        EL_FIELD_NEON_BLOOM_STRENGTH = 3,
        EL_FIELD_NEON_FILAMENT_FALLOFF = 4,
        EL_FIELD_NEON_GLOW_SIDE_SOFTNESS = 5,
        EL_FIELD_NEON_HUE_ROTATION_RATE = 6
    } el_config_field_e;

    typedef enum el_segment_field_e
    {
        EL_SEGMENT_FIELD_POSITION = 0,
        EL_SEGMENT_FIELD_LENGTH = 1,
        EL_SEGMENT_FIELD_BOOST = 2
    } el_segment_field_e;

    typedef enum el_arc_field_e
    {
        EL_ARC_FIELD_START = 0,
        EL_ARC_FIELD_LENGTH = 1,
        EL_ARC_FIELD_INTENSITY = 2
    } el_arc_field_e;

    typedef enum el_color_stop_field_e
    {
        EL_STOP_FIELD_POSITION = 0,
        EL_STOP_FIELD_R = 1,
        EL_STOP_FIELD_G = 2,
        EL_STOP_FIELD_B = 3,
        EL_STOP_FIELD_A = 4
    } el_color_stop_field_e;

    /* --------------------------------------------------------------------------
     * Opaque handles
     * ------------------------------------------------------------------------ */
    typedef struct el_effect_handle_impl *el_effect_handle_t;
    typedef struct el_animation_handle_impl *el_animation_handle_t;
    typedef struct el_modulator_handle_impl *el_modulator_handle_t;

    /* --------------------------------------------------------------------------
     * Effect - config setters (each setter modifies the handle's staging config)
     * ------------------------------------------------------------------------ */

    EL_API el_result_e el_effect_set_geometry(el_effect_handle_t fx,
                                              float width, float height, float posX, float posY, float cornerRadius);
    EL_API el_result_e el_effect_get_geometry(el_effect_handle_t fx,
                                              float *outWidth, float *outHeight, float *outPosX, float *outPosY,
                                              float *outCornerRadius);

    EL_API el_result_e el_effect_set_winding(el_effect_handle_t fx, el_winding_e winding);
    EL_API el_result_e el_effect_get_winding(el_effect_handle_t fx, el_winding_e *outWinding);

    EL_API el_result_e el_effect_set_neon_renderer_enabled(el_effect_handle_t fx, int enabled);
    EL_API el_result_e el_effect_get_neon_renderer_enabled(el_effect_handle_t fx, int *outEnabled);

    EL_API el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t fx, int show);
    EL_API el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t fx, int *outShow);

    EL_API el_result_e el_effect_set_show_color_stops(el_effect_handle_t fx, int show);
    EL_API el_result_e el_effect_get_show_color_stops(el_effect_handle_t fx, int *outShow);

    EL_API el_result_e el_effect_set_opaque(el_effect_handle_t fx, int opaque);
    EL_API el_result_e el_effect_get_opaque(el_effect_handle_t fx, int *outOpaque);

    EL_API el_result_e el_effect_set_opaque_color(el_effect_handle_t fx,
                                                  float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_opaque_color(el_effect_handle_t fx,
                                                  float *outR, float *outG, float *outB, float *outA);

    EL_API el_result_e el_effect_set_line_width(el_effect_handle_t fx, float width);
    EL_API el_result_e el_effect_get_line_width(el_effect_handle_t fx, float *outWidth);

    EL_API el_result_e el_effect_set_filament_falloff(el_effect_handle_t fx, float falloff);
    EL_API el_result_e el_effect_get_filament_falloff(el_effect_handle_t fx, float *outFalloff);

    EL_API el_result_e el_effect_set_intensity(el_effect_handle_t fx, float val);
    EL_API el_result_e el_effect_get_intensity(el_effect_handle_t fx, float *outVal);

    EL_API el_result_e el_effect_set_glow_radius(el_effect_handle_t fx, float radius);
    EL_API el_result_e el_effect_get_glow_radius(el_effect_handle_t fx, float *outRadius);

    EL_API el_result_e el_effect_set_bloom_strength(el_effect_handle_t fx, float val);
    EL_API el_result_e el_effect_get_bloom_strength(el_effect_handle_t fx, float *outVal);

    EL_API el_result_e el_effect_set_glow_side(el_effect_handle_t fx, el_glow_side_e side);
    EL_API el_result_e el_effect_get_glow_side(el_effect_handle_t fx, el_glow_side_e *outSide);

    EL_API el_result_e el_effect_set_glow_side_softness(el_effect_handle_t fx, float val);
    EL_API el_result_e el_effect_get_glow_side_softness(el_effect_handle_t fx, float *outVal);

    EL_API el_result_e el_effect_set_blend_space(el_effect_handle_t fx, el_blend_space_e space);
    EL_API el_result_e el_effect_get_blend_space(el_effect_handle_t fx, el_blend_space_e *outSpace);

    EL_API el_result_e el_effect_set_hue_rotation_rate(el_effect_handle_t fx, float rate);
    EL_API el_result_e el_effect_get_hue_rotation_rate(el_effect_handle_t fx, float *outRate);

    EL_API el_result_e el_effect_set_color_transition_duration(el_effect_handle_t fx, float seconds);
    EL_API el_result_e el_effect_get_color_transition_duration(el_effect_handle_t fx, float *outSeconds);

    EL_API el_result_e el_effect_set_color_stop_count(el_effect_handle_t fx, int32_t count);
    EL_API el_result_e el_effect_get_color_stop_count(el_effect_handle_t fx, int32_t *outCount);

    EL_API el_result_e el_effect_set_color_stop(el_effect_handle_t fx, int32_t index,
                                                float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_color_stop(el_effect_handle_t fx, int32_t index,
                                                float *outPosition, float *outR, float *outG, float *outB, float *outA);
    EL_API el_result_e el_effect_clear_color_stops(el_effect_handle_t fx);

    EL_API el_result_e el_effect_set_segment_boost_count(el_effect_handle_t fx, int32_t count);
    EL_API el_result_e el_effect_get_segment_boost_count(el_effect_handle_t fx, int32_t *outCount);

    EL_API el_result_e el_effect_set_segment_boost(el_effect_handle_t fx, int32_t index,
                                                   float position, float length, float boost);
    EL_API el_result_e el_effect_get_segment_boost(el_effect_handle_t fx, int32_t index,
                                                   float *outPosition, float *outLength, float *outBoost);
    EL_API el_result_e el_effect_clear_segment_boosts(el_effect_handle_t fx);

    EL_API el_result_e el_effect_set_segment_blend_space(el_effect_handle_t fx,
                                                         int32_t segmentIndex, el_blend_space_e blendSpace);
    EL_API el_result_e el_effect_get_segment_blend_space(el_effect_handle_t fx,
                                                         int32_t segmentIndex, el_blend_space_e *outBlendSpace);

    EL_API el_result_e el_effect_set_segment_color_stop_count(el_effect_handle_t fx,
                                                              int32_t segmentIndex, int32_t count);
    EL_API el_result_e el_effect_get_segment_color_stop_count(el_effect_handle_t fx,
                                                              int32_t segmentIndex, int32_t *outCount);

    EL_API el_result_e el_effect_set_segment_color_stop(el_effect_handle_t fx,
                                                        int32_t segmentIndex, int32_t stopIndex,
                                                        float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_segment_color_stop(el_effect_handle_t fx,
                                                        int32_t segmentIndex, int32_t stopIndex,
                                                        float *outPosition, float *outR, float *outG, float *outB, float *outA);
    EL_API el_result_e el_effect_clear_segment_color_stops(el_effect_handle_t fx, int32_t segmentIndex);

    EL_API el_result_e el_effect_set_arc_count(el_effect_handle_t fx, int32_t count);
    EL_API el_result_e el_effect_get_arc_count(el_effect_handle_t fx, int32_t *outCount);

    EL_API el_result_e el_effect_set_arc(el_effect_handle_t fx, int32_t index,
                                         float start, float length, float intensity, el_blend_space_e blendSpace);
    EL_API el_result_e el_effect_get_arc(el_effect_handle_t fx, int32_t index,
                                         float *outStart, float *outLength, float *outIntensity, el_blend_space_e *outBlendSpace);
    EL_API el_result_e el_effect_clear_arcs(el_effect_handle_t fx);

    EL_API el_result_e el_effect_set_arc_color_stop_count(el_effect_handle_t fx,
                                                          int32_t arcIndex, int32_t count);
    EL_API el_result_e el_effect_get_arc_color_stop_count(el_effect_handle_t fx,
                                                          int32_t arcIndex, int32_t *outCount);

    EL_API el_result_e el_effect_set_arc_color_stop(el_effect_handle_t fx,
                                                    int32_t arcIndex, int32_t stopIndex,
                                                    float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_arc_color_stop(el_effect_handle_t fx,
                                                    int32_t arcIndex, int32_t stopIndex,
                                                    float *outPosition, float *outR, float *outG, float *outB, float *outA);
    EL_API el_result_e el_effect_clear_arc_color_stops(el_effect_handle_t fx, int32_t arcIndex);

    EL_API el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t fx, int enabled);
    EL_API el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t fx, int *outEnabled);

    EL_API el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t fx, float scale);
    EL_API el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t fx, float *outScale);

    EL_API el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t fx, int32_t samples);
    EL_API el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t fx, int32_t *outSamples);

    EL_API el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t fx, int32_t size);
    EL_API el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t fx, int32_t *outSize);

    EL_API el_result_e el_effect_set_optimized_show_half_res(el_effect_handle_t fx, int show);
    EL_API el_result_e el_effect_get_optimized_show_half_res(el_effect_handle_t fx, int *outShow);

    EL_API el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t fx, int enabled);
    EL_API el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t fx, int *outEnabled);

    EL_API el_result_e el_effect_set_wireframe_color(el_effect_handle_t fx,
                                                     float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_wireframe_color(el_effect_handle_t fx,
                                                     float *outR, float *outG, float *outB, float *outA);

    /* --------------------------------------------------------------------------
     * Effect lifecycle
     * ------------------------------------------------------------------------ */

    EL_API el_effect_handle_t el_effect_create(void);
    EL_API el_result_e el_effect_destroy(el_effect_handle_t fx);
    EL_API el_result_e el_effect_initialize(el_effect_handle_t fx);

    /** @brief Pull the effect's base config back into the handle's staging config.
     *  @details Snapshots the last-authored values (what SetConfig received) -
     *           does NOT include animation overlays. Use this to re-sync the
     *           staging config after external mutation (e.g. presets applied
     *           inside the effect). */
    EL_API el_result_e el_effect_capture(el_effect_handle_t fx);

    /** @brief Apply staging config then tick clock, animations, and renderers. */
    EL_API el_result_e el_effect_update(el_effect_handle_t fx, float deltaTime);
    EL_API el_result_e el_effect_render(el_effect_handle_t fx,
                                        int32_t viewportWidth, int32_t viewportHeight);

    EL_API el_result_e el_effect_attach_animation(el_effect_handle_t fx, el_animation_handle_t anim);
    EL_API el_result_e el_effect_detach_animation(el_effect_handle_t fx, el_animation_handle_t anim);
    EL_API el_result_e el_effect_detach_all_animations(el_effect_handle_t fx);
    EL_API el_result_e el_effect_get_animation_count(el_effect_handle_t fx, int32_t *outCount);
    EL_API el_result_e el_effect_contains_animation(el_effect_handle_t fx,
                                                    el_animation_handle_t anim, int *outContains);

    /* --------------------------------------------------------------------------
     * Animation lifecycle
     * ------------------------------------------------------------------------ */

    EL_API el_animation_handle_t el_animation_create(el_animation_preset_e preset);
    EL_API el_result_e el_animation_destroy(el_animation_handle_t anim);

    EL_API el_animation_handle_t el_animation_create_intensity_pulse(float duration,
                                                                     float min, float max);
    EL_API el_animation_handle_t el_animation_create_intensity_strobe(float duration,
                                                                      float offIntensity, float onIntensity);
    EL_API el_animation_handle_t el_animation_create_intensity_fade_in(float target,
                                                                       float duration, el_easing_e easing);
    EL_API el_animation_handle_t el_animation_create_intensity_fade_out(float start,
                                                                        float duration, el_easing_e easing);
    EL_API el_animation_handle_t el_animation_create_glow_radius_breath(float duration,
                                                                        float minRadius, float maxRadius);
    EL_API el_animation_handle_t el_animation_create_bloom_pulse(float duration,
                                                                 float min, float max);
    EL_API el_animation_handle_t el_animation_create_hue_rotation_reverse(float baseRate,
                                                                          float duration);
    EL_API el_animation_handle_t el_animation_create_hue_rotation_ease_reverse(float maxRate,
                                                                               float duration);
    EL_API el_animation_handle_t el_animation_create_segment_travel(float duration,
                                                                    float length, float boost);
    EL_API el_animation_handle_t el_animation_create_segment_bounce(float duration,
                                                                    float length, float boost);
    EL_API el_animation_handle_t el_animation_create_outline_tracer(float duration,
                                                                    el_easing_e easing);
    EL_API el_animation_handle_t el_animation_create_arc_wipe(float duration,
                                                              float startPos, float endPos, float maxLength,
                                                              el_easing_e easing);

    EL_API el_result_e el_animation_play(el_animation_handle_t anim);
    EL_API el_result_e el_animation_pause(el_animation_handle_t anim);
    EL_API el_result_e el_animation_stop(el_animation_handle_t anim);
    EL_API el_result_e el_animation_reset(el_animation_handle_t anim, el_effect_handle_t fx);
    EL_API el_result_e el_animation_update(el_animation_handle_t anim, float dt);
    EL_API el_result_e el_animation_apply(el_animation_handle_t anim, el_effect_handle_t fx);

    EL_API el_result_e el_animation_get_state(el_animation_handle_t anim,
                                              el_animation_state_e *outState);
    EL_API el_result_e el_animation_get_elapsed(el_animation_handle_t anim, float *outElapsed);
    EL_API el_result_e el_animation_set_elapsed(el_animation_handle_t anim, float elapsed);

    EL_API el_result_e el_animation_get_end_action(el_animation_handle_t anim,
                                                   el_end_action_e *outAction);
    EL_API el_result_e el_animation_set_end_action(el_animation_handle_t anim,
                                                   el_end_action_e action);
    EL_API el_result_e el_animation_capture_baseline(el_animation_handle_t anim,
                                                     el_effect_handle_t fx);

    EL_API el_result_e el_animation_get_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e *outMode);
    EL_API el_result_e el_animation_set_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e mode);

    EL_API el_result_e el_animation_get_duration(el_animation_handle_t anim, float *outSeconds);
    EL_API el_result_e el_animation_set_duration(el_animation_handle_t anim, float seconds);

    EL_API el_result_e el_animation_get_speed(el_animation_handle_t anim, float *outSpeed);
    EL_API el_result_e el_animation_set_speed(el_animation_handle_t anim, float speed);

    /* --------------------------------------------------------------------------
     * Animation callbacks (C function pointer + user data)
     * ------------------------------------------------------------------------ */
    typedef void (*el_animation_on_completed_callback)(void *userData);
    typedef void (*el_animation_on_state_changed_callback)(int prevState, int currState, void *userData);

    EL_API el_result_e el_animation_set_on_complete_callback(el_animation_handle_t anim,
                                                             el_animation_on_completed_callback callback, void *userData);
    EL_API el_result_e el_animation_set_on_state_changed_callback(el_animation_handle_t anim,
                                                                  el_animation_on_state_changed_callback callback, void *userData);

    EL_API el_animation_handle_t el_animation_from_modulator(el_config_field_e field,
                                                             el_modulator_handle_t mod);
    EL_API el_animation_handle_t el_animation_create_field_bound(void);
    EL_API el_result_e el_animation_add_field(el_animation_handle_t anim,
                                              el_config_field_e field, el_modulator_handle_t mod);
    EL_API el_result_e el_animation_add_segment_field(el_animation_handle_t anim,
                                                      int32_t index, el_segment_field_e field, el_modulator_handle_t mod);
    EL_API el_result_e el_animation_add_arc_field(el_animation_handle_t anim,
                                                  int32_t index, el_arc_field_e field, el_modulator_handle_t mod);
    EL_API el_result_e el_animation_add_arc_stop_field(el_animation_handle_t anim,
                                                       int32_t arcIdx, int32_t stopIdx, el_color_stop_field_e field,
                                                       el_modulator_handle_t mod);
    EL_API el_result_e el_animation_add_segment_stop_field(el_animation_handle_t anim,
                                                           int32_t segIdx, int32_t stopIdx, el_color_stop_field_e field,
                                                           el_modulator_handle_t mod);

    /* --------------------------------------------------------------------------
     * Modulator factories
     * ------------------------------------------------------------------------ */

    EL_API el_modulator_handle_t el_modulator_create_constant(float value);
    EL_API el_modulator_handle_t el_modulator_create_oscillator(float frequency,
                                                                float min, float max, float phase, el_waveform_e waveform);
    EL_API el_modulator_handle_t el_modulator_create_ease(float from, float to,
                                                          float duration, el_easing_e easing, int loop);
    EL_API el_modulator_handle_t el_modulator_create_sequence(int loop);
    EL_API el_result_e el_modulator_sequence_append(el_modulator_handle_t seq,
                                                    el_modulator_handle_t segment, float duration);
    EL_API el_modulator_handle_t el_modulator_create_multiplier(el_modulator_handle_t a,
                                                                el_modulator_handle_t b);
    EL_API el_modulator_handle_t el_modulator_create_adder(el_modulator_handle_t a,
                                                           el_modulator_handle_t b);
    EL_API el_modulator_handle_t el_modulator_create_remap(el_modulator_handle_t inner,
                                                           float outMin, float outMax);
    EL_API el_result_e el_modulator_destroy(el_modulator_handle_t mod);
    EL_API el_result_e el_modulator_evaluate(el_modulator_handle_t mod,
                                             float time, float *outValue);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EDGE_LIGHTING_CAPI_H_
