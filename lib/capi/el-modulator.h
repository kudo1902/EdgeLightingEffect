/**
 * @file el-modulator.h
 * @brief Modulator factories (pure time->float composables) for the
 *        EdgeLighting C API.
 */
#ifndef _EL_MODULATOR_H_
#define _EL_MODULATOR_H_

#include "el-types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ======================================================================
     * Modulator factories
     *
     * A modulator is a pure @c time -> float function. Compose them with
     * Sequence / Multiplier / Adder / Remap to build arbitrarily complex
     * signals without touching the animation subclasses. Sharing ownership
     * across composites is safe - the composite retains a strong reference
     * internally, so destroying the outer handle keeps the composite alive.
     * ==================================================================== */

    /** @name Modulator factories
     *  Every factory returns @c NULL on failure. All non-null return values
     *  must eventually be destroyed with @ref el_modulator_destroy - the
     *  destroy call releases the outer handle; the underlying modulator is
     *  kept alive by any composite still referring to it.
     *  @{ */

    /** @brief Always returns @p value. */
    EL_API el_modulator_handle_t el_modulator_create_constant(float value);

    /** @brief Periodic waveform in [@p minValue, @p maxValue].
     *  @param frequency Cycles per second (Hz).
     *  @param minValue  Lower bound of the output range.
     *  @param maxValue  Upper bound of the output range.
     *  @param phase     Initial phase offset in cycles (typically [0, 1)).
     *  @param waveform  Shape of the oscillation. */
    EL_API el_modulator_handle_t el_modulator_create_oscillator(float frequency,
                                                                float minValue, float maxValue, float phase, el_waveform_e waveform);

    /** @brief One-shot (or looping) eased interpolation from @p from to @p to.
     *  @param duration Length of the transition (seconds).
     *  @param easing   Easing curve.
     *  @param loop     If true (non-zero), the transition repeats when time
     *                  exceeds duration. */
    EL_API el_modulator_handle_t el_modulator_create_ease(float from, float to,
                                                          float duration, el_easing_e easing, el_bool_t loop);

    /** @brief Empty sequence; append stages with @ref el_modulator_sequence_append.
     *  @param loop If true, the sequence loops back to its first stage once
     *              the last stage ends; otherwise the final value is held. */
    EL_API el_modulator_handle_t el_modulator_create_sequence(el_bool_t loop);

    /** @brief Append @p stage to the tail of @p seq, running for @p duration seconds.
     *  @details The sequence retains a strong reference to @p stage; the
     *           caller can destroy the outer stage handle immediately. */
    EL_API el_result_e el_modulator_sequence_append(el_modulator_handle_t seq,
                                                    el_modulator_handle_t stage, float duration);

    /** @brief Product modulator: evaluates as @p a(t) * @p b(t). */
    EL_API el_modulator_handle_t el_modulator_create_multiplier(el_modulator_handle_t a,
                                                                el_modulator_handle_t b);

    /** @brief Sum modulator: evaluates as @p a(t) + @p b(t). */
    EL_API el_modulator_handle_t el_modulator_create_adder(el_modulator_handle_t a,
                                                           el_modulator_handle_t b);

    /** @brief Rescale @p inner's output into [@p outMin, @p outMax].
     *  @details Handy for driving several fields from one canonical [0, 1]
     *           signal (e.g. an @c Ease) at different ranges. */
    EL_API el_modulator_handle_t el_modulator_create_remap(el_modulator_handle_t inner,
                                                           float outMin, float outMax);

    /** @brief Destroy a modulator handle.
     *  @details Releases the outer handle only. If the underlying modulator
     *           is still referenced by a composite or an animation binding
     *           it keeps living; otherwise it is freed here. */
    EL_API el_result_e el_modulator_destroy(el_modulator_handle_t mod);

    /** @brief Evaluate a modulator at time @p time and write the result to
     *         @p outValue. Useful for debugging / previewing signals. */
    EL_API el_result_e el_modulator_evaluate(el_modulator_handle_t mod,
                                             float time, float *outValue);

    /** @} */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_MODULATOR_H_
