// Deprecated C ABI surface. Every function here forwards to its replacement in
// el-effect.cpp and holds no state or behaviour of its own, except
// el_effect_set/get_optimized_renderer_enabled, whose legacy enable semantics
// have no successor and so live here.
//
// Deleting this file plus el-deprecated.h retires the whole deprecated
// surface; see the removal checklist at the top of el-deprecated.h.
//
// The declarations are deprecated, so defining them would warn under
// -Wdeprecated-declarations on some compilers, and the forwarding bodies below
// deliberately call the replacements rather than each other.
#include "capi-internal.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace
{
    /// The resolution scale the retired half-res renderer defaulted to. Only
    /// the legacy enable shim needs it - nothing else restores an old default.
    constexpr float EL_LEGACY_OPTIMIZED_SCALE = 0.5f;
}

extern "C"
{

    // --- Deprecated: the "optimized" neon renderer -------------------------

    el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_renderer_enabled");
        if (enabled != 0)
        {
            // Enabling the "optimized renderer" now means enabling the neon
            // layer and taking it off full resolution. A scale already below
            // 1.0 is left alone, so this cannot undo an explicit
            // el_effect_set_neon_resolution_scale that came first.
            effect->config.neon.enable = true;
            if (effect->config.neon.resolutionScale >= 1.0f)
            {
                effect->config.neon.resolutionScale = EL_LEGACY_OPTIMIZED_SCALE;
            }
        }
        else
        {
            // Disabling means going back to full resolution - NOT turning the
            // neon off. Under the old ABI these were two renderers and this
            // call only ever silenced one of them, so clearing neon.enable
            // here would blank the effect for a host that is simply switching
            // paths.
            effect->config.neon.resolutionScale = 1.0f;
        }
        LOG_D("effect=%p, enabled=%d -> neon.enable=%d, scale=%f", (void *)effect, enabled,
              effect->config.neon.enable ? 1 : 0, effect->config.neon.resolutionScale);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_optimized_renderer_enabled");
        // "The optimized path is on" == neon is drawing, below full res.
        *outEnabled = (effect->config.neon.enable && effect->config.neon.resolutionScale < 1.0f) ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t effect, float scale)
    {
        return el_effect_set_neon_resolution_scale(effect, scale);
    }

    el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t effect, float *outScale)
    {
        return el_effect_get_neon_resolution_scale(effect, outScale);
    }

    el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t effect, int32_t samples)
    {
        return el_effect_set_neon_num_samples(effect, samples);
    }

    el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t effect, int32_t *outSamples)
    {
        return el_effect_get_neon_num_samples(effect, outSamples);
    }

    el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t size)
    {
        return el_effect_set_neon_gradient_lut_size(effect, size);
    }

    el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize)
    {
        return el_effect_get_neon_gradient_lut_size(effect, outSize);
    }

    // --- Deprecated: the "optimized" lens flare renderer --------------------

    el_result_e el_effect_set_optimized_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_optimized_lens_flare_renderer_enabled");
        if (enabled != 0)
        {
            effect->config.lensFlare.enable = true;
            if (effect->config.lensFlare.resolutionScale >= 1.0f)
            {
                effect->config.lensFlare.resolutionScale = EL_LEGACY_OPTIMIZED_SCALE;
            }
        }
        else
        {
            // Full resolution, but NOT disabled - see the neon shim above for
            // why clearing the enable here would blank the layer for a host
            // that is merely switching paths.
            effect->config.lensFlare.resolutionScale = 1.0f;
        }
        LOG_D("effect=%p, enabled=%d -> lensFlare.enable=%d, scale=%f", (void *)effect, enabled,
              effect->config.lensFlare.enable ? 1 : 0, effect->config.lensFlare.resolutionScale);
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_optimized_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_optimized_lens_flare_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_optimized_lens_flare_renderer_enabled");
        *outEnabled = (effect->config.lensFlare.enable &&
                       effect->config.lensFlare.resolutionScale < 1.0f) ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_optimized_lens_flare_resolution_scale(el_effect_handle_t effect, float scale)
    {
        return el_effect_set_lens_flare_resolution_scale(effect, scale);
    }

    el_result_e el_effect_get_optimized_lens_flare_resolution_scale(el_effect_handle_t effect, float *outScale)
    {
        return el_effect_get_lens_flare_resolution_scale(effect, outScale);
    }

    // --- Deprecated: the wireframe renderer --------------------------------

    el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        return el_effect_set_debug_show_wireframe(effect, enabled);
    }

    el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        return el_effect_get_debug_show_wireframe(effect, outEnabled);
    }

    // --- Deprecated: debug overlay flags without the `debug` prefix ---------

    el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t effect, el_bool_t show)
    {
        return el_effect_set_debug_show_gradient_lut(effect, show);
    }

    el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow)
    {
        return el_effect_get_debug_show_gradient_lut(effect, outShow);
    }

    el_result_e el_effect_set_show_color_stops(el_effect_handle_t effect, el_bool_t show)
    {
        return el_effect_set_debug_show_color_stops(effect, show);
    }

    el_result_e el_effect_get_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow)
    {
        return el_effect_get_debug_show_color_stops(effect, outShow);
    }

    el_result_e el_effect_set_wireframe_color(el_effect_handle_t effect,
                                              float r, float g, float b, float a)
    {
        return el_effect_set_debug_wireframe_color(effect, r, g, b, a);
    }

    el_result_e el_effect_get_wireframe_color(el_effect_handle_t effect,
                                              float *outR, float *outG, float *outB, float *outA)
    {
        return el_effect_get_debug_wireframe_color(effect, outR, outG, outB, outA);
    }
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
