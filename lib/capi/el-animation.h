/**
 * @file el-animation.h
 * @brief Animation lifecycle, factories, control, callbacks, and field-bound
 *        bindings for the EdgeLighting C API.
 */
#ifndef _EL_ANIMATION_H_
#define _EL_ANIMATION_H_

#include "el-types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ======================================================================
     * Animation lifecycle and factories
     * ==================================================================== */

    /** @name Preset factory
     *  Build a named preset animation. Prefer the dedicated factories below
     *  when you want to override the internal parameters.
     *  @{ */

    /** @brief Build an animation from an @ref el_animation_preset_e.
     *  @returns A fresh animation handle on success, @c NULL on failure. */
    EL_API el_animation_handle_t el_animation_create(el_animation_preset_e preset);

    /** @brief Destroy an animation handle. Detach first if attached. */
    EL_API el_result_e el_animation_destroy(el_animation_handle_t anim);

    /** @} */

    /** @name Neon animation factories
     *  Each factory returns a preconfigured animation ready to
     *  @ref el_effect_attach_animation and @ref el_animation_play.
     *  All handles must be destroyed with @ref el_animation_destroy.
     *  @{ */

    /** @brief Sinusoidal intensity pulse looping at @p duration seconds.
     *  @param duration     Period of one full pulse (seconds).
     *  @param minIntensity Trough intensity multiplier.
     *  @param maxIntensity Crest intensity multiplier. */
    EL_API el_animation_handle_t el_animation_create_intensity_pulse(float duration,
                                                                     float minIntensity, float maxIntensity);

    /** @brief Square-wave intensity strobe.
     *  @param duration     Period of one on+off cycle (seconds).
     *  @param offIntensity Intensity during the "off" half.
     *  @param onIntensity  Intensity during the "on" half. */
    EL_API el_animation_handle_t el_animation_create_intensity_strobe(float duration,
                                                                      float offIntensity, float onIntensity);

    /** @brief One-shot intensity fade-in.
     *  @param targetIntensity Final intensity value.
     *  @param duration        Length of the transition (seconds).
     *  @param easing          Easing curve applied to the interpolation. */
    EL_API el_animation_handle_t el_animation_create_intensity_fade_in(float targetIntensity,
                                                                       float duration, el_easing_e easing);

    /** @brief One-shot intensity fade-out from @p startIntensity to 0. */
    EL_API el_animation_handle_t el_animation_create_intensity_fade_out(float startIntensity,
                                                                        float duration, el_easing_e easing);

    /** @brief Sinusoidal glow-radius breath looping at @p duration seconds. */
    EL_API el_animation_handle_t el_animation_create_glow_radius_breath(float duration,
                                                                        float minRadius, float maxRadius);

    /** @brief Sinusoidal bloom-strength pulse looping at @p duration seconds. */
    EL_API el_animation_handle_t el_animation_create_bloom_pulse(float duration,
                                                                 float minStrength, float maxStrength);

    /** @brief Hue-rotation reversal: rate ramps from -peak to +peak (or v.v.)
     *         under a square-shaped envelope. */
    EL_API el_animation_handle_t el_animation_create_hue_rotation_reverse(float peakRate,
                                                                          float duration);

    /** @brief Hue-rotation reversal shaped by a smooth ease-in/ease-out curve. */
    EL_API el_animation_handle_t el_animation_create_hue_rotation_ease_reverse(float peakRate,
                                                                               float duration);

    /** @brief Single segment travelling once around the perimeter. */
    EL_API el_animation_handle_t el_animation_create_segment_travel(float duration,
                                                                    float length, float boost);

    /** @brief Single segment ping-ponging back and forth on the perimeter. */
    EL_API el_animation_handle_t el_animation_create_segment_bounce(float duration,
                                                                    float length, float boost);

    /** @brief One-shot outline tracer - arc[0] length grows from 0 to
     *         @p maxLength (default 1 = full perimeter; may exceed 1 to keep
     *         the head wrapping past the tail). */
    EL_API el_animation_handle_t el_animation_create_outline_tracer(float duration,
                                                                    el_easing_e easing,
                                                                    float maxLength);

    /** @brief One-shot arc wipe - arc[0] grows from @p startPosition to
     *         @p endPosition, capped at @p maxLength. */
    EL_API el_animation_handle_t el_animation_create_arc_wipe(float duration,
                                                              float startPosition, float endPosition, float maxLength,
                                                              el_easing_e easing);

    /** @} */

    /** @name Animation control
     *  Play, pause, stop, reset, update, and apply. Each animation carries
     *  its own play state and elapsed accumulator - pausing one does not
     *  affect any other. Explicit Update / Apply are only needed for
     *  detached animations; attached animations are driven automatically by
     *  @ref el_effect_update.
     *
     *  @par Threading
     *  Any thread, for every @c el_animation_* call in this header. An ATTACHED
     *  animation takes the lock of the effect it is attached to, so these
     *  exclude @ref el_effect_update - which is required, since that call reads
     *  and writes the very elapsed, state and mode these touch. A DETACHED
     *  animation takes no lock at all and belongs to whoever built it: drive it
     *  from one thread.
     *
     *  It is the effect's lock and not one of the animation's own, deliberately.
     *  One mutex means these calls can never deadlock against an effect call,
     *  and it is what lets a host callback fired from inside
     *  @ref el_effect_update turn round and call @ref el_animation_pause on the
     *  animation that fired it.
     *
     *  The gap: attaching and detaching WRITE that adopted lock, and the write
     *  is not atomic. Do not attach or detach a handle while another thread is
     *  calling something else on that same handle.
     *  @{ */

    /** @brief Enter the PLAYING state.
     *  @details From STOPPED, elapsed is reset to 0 (a completed one-shot
     *           restarts from the beginning). From PAUSED, elapsed continues
     *           - there is no separate "resume". From PLAYING, this is a
     *           no-op. */
    EL_API el_result_e el_animation_play(el_animation_handle_t anim);

    /** @brief Freeze elapsed at its current value. Apply keeps writing it.
     *  @details Valid from PLAYING only; no-op otherwise. */
    EL_API el_result_e el_animation_pause(el_animation_handle_t anim);

    /** @brief Enter the STOPPED state.
     *  @details Symmetric with natural one-shot completion. The next Apply
     *           dispatches on the animation's end-action. */
    EL_API el_result_e el_animation_stop(el_animation_handle_t anim);

    /** @brief Zero elapsed and write the modulator's t=0 value into the
     *         effect's staging config. Does NOT change state.
     *  @note Writes staging, so it carries the same persistence caveat as
     *        @ref el_animation_apply - the t=0 value becomes an authored one. */
    EL_API el_result_e el_animation_reset(el_animation_handle_t anim, el_effect_handle_t effect);

    /** @brief Advance the animation by @p dt seconds (only if PLAYING). */
    EL_API el_result_e el_animation_update(el_animation_handle_t anim, float dt);

    /** @brief Write the animation's current value into the effect's staging
     *         config.
     *  @details This is the MANUAL composition path, and it is not what an
     *           attached animation does. Staging is committed to the base
     *           config by the next @ref el_effect_update, so a value written
     *           here becomes an authored value and outlives the animation -
     *           stopping or detaching it will not take the value back.
     *
     *           An animation attached with @ref el_effect_attach_animation is
     *           applied by @ref el_effect_update into the ACTIVE config
     *           instead, leaving the base untouched, which is what makes a
     *           stopped animation revert. Do not call this on an attached
     *           animation: it would apply the overlay twice, once
     *           permanently. */
    EL_API el_result_e el_animation_apply(el_animation_handle_t anim, el_effect_handle_t effect);

    /** @} */

    /** @name Animation introspection and tuning
     *  Read/write play state, elapsed, progress, duration, speed, and end
     *  action. Setters take effect on the next frame.
     *  @{ */

    /** @brief Read the animation's current @ref el_animation_state_e. */
    EL_API el_result_e el_animation_get_state(el_animation_handle_t anim,
                                              el_animation_state_e *outState);

    /** @brief Read the animation's elapsed accumulator (seconds). */
    EL_API el_result_e el_animation_get_elapsed(el_animation_handle_t anim, float *outElapsed);
    /** @brief Directly overwrite the elapsed accumulator.
     *  @details Useful for scrubbing / testing. Does NOT change state or
     *           trigger completion callbacks - the next @ref el_animation_update
     *           tick will complete a one-shot whose elapsed has crossed
     *           its duration. Negative values are clamped to 0. */
    EL_API el_result_e el_animation_set_elapsed(el_animation_handle_t anim, float elapsed);

    /** @brief Normalised playback position (elapsed / duration) in [0, 1].
     *         Returns 0 when the animation's duration is 0 (its modulator
     *         owns its own periodicity). */
    EL_API el_result_e el_animation_get_progress(el_animation_handle_t anim, float *outProgress);
    /** @brief Set the normalised playback position; @p progress is clamped to
     *         [0, 1] before scaling. No-op when the animation's duration is 0.
     *         Same "does not change state" caveat as @ref el_animation_set_elapsed. */
    EL_API el_result_e el_animation_set_progress(el_animation_handle_t anim, float progress);

    /** @brief Read the animation's end-action policy. */
    EL_API el_result_e el_animation_get_end_action(el_animation_handle_t anim,
                                                   el_end_action_e *outAction);
    /** @brief Set the animation's end-action policy.
     *  @details Takes effect on the frame after stop - the currently running
     *           value is untouched. See @ref el_end_action_e for the menu. */
    /** @note All of HOLD_CURRENT / HOLD_END / HOLD_START / RESTORE keep
     *        WRITING once the animation stops, so the effect's own config value
     *        stays hidden underneath and @c el_effect_set_* edits to that field
     *        have no visible result. @ref EL_END_ACTION_HOLD_NONE stops writing and
     *        hands the field back; @ref el_effect_detach_animation does the
     *        same from the outside. */
    EL_API el_result_e el_animation_set_end_action(el_animation_handle_t anim,
                                                   el_end_action_e action);

    /** @brief Snapshot the effect's staging config for later RESTORE.
     *  @details Must be called BEFORE @ref el_animation_play if the animation
     *           uses @ref EL_END_ACTION_RESTORE. Silently no-ops on
     *           animations whose subclass does not implement CaptureBaseline. */
    EL_API el_result_e el_animation_capture_baseline(el_animation_handle_t anim,
                                                     el_effect_handle_t effect);

    /** @brief Read the animation's playback mode (loop vs. one-shot). */
    EL_API el_result_e el_animation_get_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e *outMode);
    /** @brief Set the animation's playback mode. Does NOT touch duration. */
    EL_API el_result_e el_animation_set_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e mode);

    /** @brief Read the length of one animation cycle (seconds). */
    EL_API el_result_e el_animation_get_duration(el_animation_handle_t anim, float *outSeconds);
    /** @brief Set the length of one animation cycle (seconds).
     *  @details 0 means the internal modulator owns its own periodicity.
     *
     *  COMPOSITE PRESETS. @c EL_ANIM_SHIMMER and @c EL_ANIM_AURORA are groups
     *  of several animations, and a group holds no field of its own - this
     *  reaches its children, and the group's own duration is DERIVED back from
     *  them. That derivation reports 0 whenever any child loops, which every
     *  built-in preset's children do, so on those two presets this call takes
     *  effect while @ref el_animation_get_duration still answers 0. Send
     *  @ref el_animation_set_playback_mode @c EL_PLAYBACK_ONE_SHOT first and
     *  the setter and getter agree. The same applies to
     *  @ref el_animation_set_progress, which normalises against that duration.
     *  Non-composite presets are unaffected. */
    EL_API el_result_e el_animation_set_duration(el_animation_handle_t anim, float seconds);

    /** @brief Read the playback speed multiplier (1.0 = normal). */
    EL_API el_result_e el_animation_get_speed(el_animation_handle_t anim, float *outSpeed);
    /** @brief Set the playback speed multiplier.
     *  @details 1.0 = normal, 2.0 = double, 0.5 = half. 0 keeps the state
     *           PLAYING but freezes elapsed - use @ref el_animation_pause if
     *           you want the state to reflect it. Negative values are
     *           clamped to 0. */
    EL_API el_result_e el_animation_set_speed(el_animation_handle_t anim, float speed);

    /** @} */

    /* ======================================================================
     * Animation callbacks (C function pointer + user data)
     * ==================================================================== */

    /** @brief Fired once when a ONE_SHOT animation completes its cycle.
     *  @details Called from inside @ref el_effect_update, on whichever thread
     *           called it, with the effect handle's lock held. Calling back
     *           into the effect is therefore legal - including detaching the
     *           animation that fired - but BLOCKING here stalls that thread,
     *           so marshal to your UI thread rather than working in it.
     *           Never fires for LOOP animations. */
    typedef void (*el_animation_on_completed_callback)(void *userData);

    /** @brief Fired on every animation state transition.
     *  @param previous The state before the transition.
     *  @param current  The state after the transition.
     *  @param userData Opaque pointer supplied at registration. */
    typedef void (*el_animation_on_state_changed_callback)(el_animation_state_e previous,
                                                           el_animation_state_e current,
                                                           void *userData);

    /** @brief Set (or clear, with @c callback == NULL) the completion callback. */
    EL_API el_result_e el_animation_set_on_complete_callback(el_animation_handle_t anim,
                                                             el_animation_on_completed_callback callback, void *userData);
    /** @brief Set (or clear, with @c callback == NULL) the state-changed callback. */
    EL_API el_result_e el_animation_set_on_state_changed_callback(el_animation_handle_t anim,
                                                                  el_animation_on_state_changed_callback callback, void *userData);

    /* ======================================================================
     * Field-bound animations
     *
     * A field-bound animation carries a list of (field, modulator) bindings.
     * Each frame, every binding's modulator is evaluated with the animation's
     * elapsed and the result is written to the field. Use this for "one
     * animation, several fields moving together" - phase-locked pulses on
     * intensity + glow + bloom, coordinated segment motion, etc.
     * ==================================================================== */

    /** @name Field-bound animation construction
     *  @{ */

    /** @brief One-binding shortcut: build a field-bound animation that drives
     *         one scalar leaf with @p mod.
     *  @param field Scalar field to drive.
     *  @param mod   Modulator supplying the values. Passing @c NULL returns
     *               @c NULL.
     *  @returns A fresh animation handle on success, @c NULL on failure. */
    EL_API el_animation_handle_t el_animation_from_modulator(el_config_field_e field,
                                                             el_modulator_handle_t mod);

    /** @brief Empty field-bound animation. Extend with the @c el_animation_add_*
     *         family. Duration defaults to 0 (modulator owns periodicity)
     *         and mode to LOOP. */
    EL_API el_animation_handle_t el_animation_create_field_bound(void);

    /** @brief Add a scalar-leaf binding to a field-bound animation. */
    EL_API el_result_e el_animation_add_field(el_animation_handle_t anim,
                                              el_config_field_e field, el_modulator_handle_t mod);

    /** @brief Add a segment-field binding.
     *  @param index Segment slot. Must already exist in @c segmentBoosts (no
     *               auto-grow; an out-of-range index is a logged no-op). */
    EL_API el_result_e el_animation_add_segment_field(el_animation_handle_t anim,
                                                      int32_t index, el_segment_field_e field, el_modulator_handle_t mod);

    /** @brief Add a preserved-segment-field binding, addressed by stable id.
     *  @details Drives a scalar of the preserved entry owning @p id (see
     *  @ref el_effect_acquire_preserved_segment). Unlike
     *  @ref el_animation_add_segment_field this never creates the entry - acquire
     *  it first; a binding whose id is not live at apply time is skipped. Because
     *  it targets the preserved pool by id, the animation is immune to overrides
     *  of the transient @c segmentBoosts pool.
     *  @param id Stable id from @ref el_effect_acquire_preserved_segment. */
    EL_API el_result_e el_animation_add_preserved_segment_field(el_animation_handle_t anim,
                                                                uint32_t id, el_segment_field_e field, el_modulator_handle_t mod);

    /** @brief Add a binding into one channel of one colour stop inside the
     *         preserved entry owning @p id.
     *  @details By-id analogue of @ref el_animation_add_segment_stop_field.
     *  Nothing auto-grows: acquire the entry and size its stops first (via
     *  @ref el_effect_set_preserved_segment_color_stop_count). A binding whose id
     *  is not live, or whose @p stopIndex is past the current stop count, is a
     *  skipped no-op at apply time.
     *  @param id        Stable id from @ref el_effect_acquire_preserved_segment.
     *  @param stopIndex Stop slot within that entry's colour-stops list.
     *  @param field     Which channel (position / R / G / B / A) to drive. */
    EL_API el_result_e el_animation_add_preserved_segment_stop_field(el_animation_handle_t anim,
                                                                     uint32_t id, int32_t stopIndex,
                                                                     el_color_stop_field_e field, el_modulator_handle_t mod);

    /** @brief Add an arc-field binding.
     *  @param index Arc slot. Must already exist in @c arcs (no auto-grow; an
     *               out-of-range index is a logged no-op). */
    EL_API el_result_e el_animation_add_arc_field(el_animation_handle_t anim,
                                                  int32_t index, el_arc_field_e field, el_modulator_handle_t mod);

    /** @brief Add a binding into one channel of one colour stop inside one arc.
     *  @param arcIndex   Arc slot.
     *  @param stopIndex  Stop slot within that arc.
     *  @param field      Which channel (position / R / G / B / A) to drive. */
    EL_API el_result_e el_animation_add_arc_stop_field(el_animation_handle_t anim,
                                                       int32_t arcIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                       el_modulator_handle_t mod);

    /** @brief Add a binding into one channel of one colour stop inside one segment. */
    EL_API el_result_e el_animation_add_segment_stop_field(el_animation_handle_t anim,
                                                           int32_t segmentIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                           el_modulator_handle_t mod);

    /** @} */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_ANIMATION_H_
