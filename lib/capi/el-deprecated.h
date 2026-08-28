/**
 * @file el-deprecated.h
 * @brief Deprecated C ABI surface, isolated so it can be deleted in one go.
 *
 * Everything here still works and is still exported. Each function is a thin
 * forwarder onto its replacement in @c el-effect.h and carries no behaviour of
 * its own, with one deliberate exception noted at
 * @ref el_effect_set_optimized_renderer_enabled.
 *
 * @section removal Removing this header
 *
 * The whole deprecated surface is confined to two files, so retiring it is:
 *
 * 1. delete @c lib/capi/el-deprecated.h and @c lib/capi/el-deprecated.cpp;
 * 2. drop @c capi/el-deprecated.cpp from @c lib/CMakeLists.txt;
 * 3. drop the @c \#include of this header from @c edge-lighting-capi.h;
 * 4. delete the two deprecated members of @ref el_renderer_flags_e in
 *    @c el-types.h - @c EL_RENDERER_WIREFRAME and
 *    @c EL_RENDERER_NEON_OPTIMIZED - and the branches in
 *    @c el_effect_init_with_renderers that OR them in.
 *
 * Step 4 is the only part that does not live here. Enum members cannot be
 * moved to another header without changing their type, and their numeric
 * values are frozen for ABI compatibility, so they stay in @c el-types.h and
 * are listed above instead.
 *
 * @section why Why these are deprecated
 *
 * The neon effect used to be two renderers - a full-resolution one and a
 * half-resolution @c NeonOptimizedRenderer - and the bounding box used to be a
 * renderer of its own. Both forks are gone: the neon layer draws at a
 * resolution scale, and the box is one overlay of a debug layer. The names
 * below describe that old shape and would mislead anyone reading them today.
 *
 * The overlay flags are renamed for a second reason: everything backed by the
 * debug config now carries @c debug in its name, so what a call touches is
 * legible without opening the header.
 *
 * @section migrating Migrating
 *
 * | deprecated | replacement |
 * | ---------- | ----------- |
 * | @c el_effect_set_optimized_resolution_scale   | @ref el_effect_set_neon_resolution_scale |
 * | @c el_effect_set_optimized_num_samples        | @ref el_effect_set_neon_num_samples |
 * | @c el_effect_set_optimized_gradient_lut_size  | @ref el_effect_set_neon_gradient_lut_size |
 * | @c el_effect_set_wireframe_renderer_enabled   | @ref el_effect_set_debug_show_wireframe |
 * | @c el_effect_set_show_gradient_lut            | @ref el_effect_set_debug_show_gradient_lut |
 * | @c el_effect_set_show_color_stops             | @ref el_effect_set_debug_show_color_stops |
 * | @c el_effect_set_wireframe_color              | @ref el_effect_set_debug_wireframe_color |
 * | @c el_effect_set_optimized_renderer_enabled   | @ref el_effect_set_neon_renderer_enabled + @ref el_effect_set_neon_resolution_scale |
 *
 * Getters map the same way. Define @c EL_NO_DEPRECATION_WARNINGS to silence
 * the attribute while migrating.
 */
#ifndef _EL_DEPRECATED_H_
#define _EL_DEPRECATED_H_

#include "el-types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @name Deprecated: the "optimized" neon renderer
     *  There is no separate half-res renderer any more. These read and write
     *  the one neon layer's settings.
     *  @{ */

    /** @brief Enable the neon layer at the old half-res renderer's defaults.
     *  @deprecated Use @ref el_effect_set_neon_renderer_enabled together with
     *              @ref el_effect_set_neon_resolution_scale.
     *  @details The ONE function here that is not a pure forwarder, because
     *           the flag it used to own no longer exists:
     *
     *           - @c set(..., 1) enables the neon layer AND, if it is
     *             currently at full resolution, moves it to 0.5 - the scale
     *             the old half-res renderer defaulted to. A scale already
     *             below 1.0 is left alone, so this cannot undo an explicit
     *             @ref el_effect_set_neon_resolution_scale that came first.
     *           - @c set(..., 0) returns the layer to full resolution and does
     *             NOT disable the neon. Under the old ABI this call silenced
     *             one of two renderers, so clearing the enable here would
     *             blank the effect for a host that is merely switching paths.
     *           - @c get(...) reports true when the neon layer is enabled and
     *             below full resolution.
     *
     *           Note it does not restore the old sample count. That renderer
     *           defaulted to 64 samples and the unified one defaults to the
     *           shader maximum, so a host relying on the old cost should also
     *           call @ref el_effect_set_neon_num_samples with 64. */
    EL_DEPRECATED("use el_effect_set_neon_renderer_enabled + el_effect_set_neon_resolution_scale")
    EL_API el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_DEPRECATED("use el_effect_get_neon_renderer_enabled + el_effect_get_neon_resolution_scale")
    EL_API el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @deprecated Use @ref el_effect_set_neon_resolution_scale. */
    EL_DEPRECATED("use el_effect_set_neon_resolution_scale")
    EL_API el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t effect, float scale);
    /** @deprecated Use @ref el_effect_get_neon_resolution_scale. */
    EL_DEPRECATED("use el_effect_get_neon_resolution_scale")
    EL_API el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t effect, float *outScale);

    /** @deprecated Use @ref el_effect_set_neon_num_samples. */
    EL_DEPRECATED("use el_effect_set_neon_num_samples")
    EL_API el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t effect, int32_t samples);
    /** @deprecated Use @ref el_effect_get_neon_num_samples. */
    EL_DEPRECATED("use el_effect_get_neon_num_samples")
    EL_API el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t effect, int32_t *outSamples);

    /** @deprecated Use @ref el_effect_set_neon_gradient_lut_size. */
    EL_DEPRECATED("use el_effect_set_neon_gradient_lut_size")
    EL_API el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t size);
    /** @deprecated Use @ref el_effect_get_neon_gradient_lut_size. */
    EL_DEPRECATED("use el_effect_get_neon_gradient_lut_size")
    EL_API el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize);

    /** @} */

    /** @name Deprecated: the wireframe renderer
     *  The 1 px box is an overlay of the debug layer now, not a renderer.
     *  @{ */

    /** @deprecated Use @ref el_effect_set_debug_show_wireframe. */
    EL_DEPRECATED("use el_effect_set_debug_show_wireframe")
    EL_API el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    /** @deprecated Use @ref el_effect_get_debug_show_wireframe. */
    EL_DEPRECATED("use el_effect_get_debug_show_wireframe")
    EL_API el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @} */

    /** @name Deprecated: debug overlay flags without the `debug` prefix
     *  Everything backed by the debug config now carries @c debug in its name.
     *  @{ */

    /** @deprecated Use @ref el_effect_set_debug_show_gradient_lut. */
    EL_DEPRECATED("use el_effect_set_debug_show_gradient_lut")
    EL_API el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t effect, el_bool_t show);
    /** @deprecated Use @ref el_effect_get_debug_show_gradient_lut. */
    EL_DEPRECATED("use el_effect_get_debug_show_gradient_lut")
    EL_API el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow);

    /** @deprecated Use @ref el_effect_set_debug_show_color_stops. */
    EL_DEPRECATED("use el_effect_set_debug_show_color_stops")
    EL_API el_result_e el_effect_set_show_color_stops(el_effect_handle_t effect, el_bool_t show);
    /** @deprecated Use @ref el_effect_get_debug_show_color_stops. */
    EL_DEPRECATED("use el_effect_get_debug_show_color_stops")
    EL_API el_result_e el_effect_get_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow);

    /** @deprecated Use @ref el_effect_set_debug_wireframe_color. */
    EL_DEPRECATED("use el_effect_set_debug_wireframe_color")
    EL_API el_result_e el_effect_set_wireframe_color(el_effect_handle_t effect,
                                                     float r, float g, float b, float a);
    /** @deprecated Use @ref el_effect_get_debug_wireframe_color. */
    EL_DEPRECATED("use el_effect_get_debug_wireframe_color")
    EL_API el_result_e el_effect_get_wireframe_color(el_effect_handle_t effect,
                                                     float *outR, float *outG, float *outB, float *outA);

    /** @} */

#ifdef __cplusplus
}
#endif

#endif // _EL_DEPRECATED_H_
