#include "capi-internal.h"

namespace
{
    /// Seed for colour stops created by the set_*_count growers. Matches what
    /// el-effect.h documents (position 0, opaque white); a plain resize()
    /// value-initialises to transparent black instead, and because
    /// ColorStop::color.a is an emission scale rather than a blend opacity,
    /// that renders as "dark here" instead of as a merely unset colour.
    const EdgeLighting::ColorStop DEFAULT_COLOR_STOP{0.0f, glm::vec4(1.0f)};

    /// The opaque fill's cutoff on @p side. Returns nullptr and logs for a
    /// value outside el_cutoff_side_e - the enum crosses the ABI as a plain
    /// int, so an out-of-range one is a caller error, not something to clamp.
    ///
    /// Switched on as an int, not as the enum. C++ gives an unscoped enum
    /// whose enumerators are 0 and 1 the value range [0, 1], so a switch on
    /// the enum itself lets an optimiser assume nothing else arrives - and
    /// the rejection below is exactly the path it would be free to drop.
    EdgeLighting::Cutoff *OpaqueCutoffSlot(el_effect_handle_t effect, el_cutoff_side_e side, const char *who)
    {
        switch (static_cast<int>(side))
        {
        case EL_CUTOFF_SIDE_INSIDE:
        {
            return &effect->config.neon.opaqueInsideCutoff;
        }
        case EL_CUTOFF_SIDE_OUTSIDE:
        {
            return &effect->config.neon.opaqueOutsideCutoff;
        }
        default:
        {
            break;
        }
        }
        LOG_E("%s: invalid side %d", who, static_cast<int>(side));
        return nullptr;
    }

    /// Writes the ABI's (enable, size, softness) triple into @p c and reports
    /// whether anything moved. The shared body of every cutoff setter, glow
    /// and fill alike; logging stays with the caller so each line is
    /// attributed to the entry point that made the change.
    bool AssignCutoff(EdgeLighting::Cutoff &c, el_bool_t enable, float size, float softness)
    {
        const EdgeLighting::Cutoff next = {enable != 0, size, softness};
        if (c == next)
        {
            return false;
        }
        c = next;
        return true;
    }

    /// The getter-side mirror of @ref AssignCutoff. The out pointers are
    /// validated by the caller, which owns the name the error is logged under.
    void ReadCutoff(const EdgeLighting::Cutoff &c, el_bool_t *outEnable, float *outSize, float *outSoftness)
    {
        *outEnable = c.enable ? 1 : 0;
        *outSize = c.size;
        *outSoftness = c.softness;
    }

    /// Shared bounds check for the by-index spotlight accessors. Returns
    /// nullptr and logs when @p index names no lamp.
    EdgeLighting::SpotLight *SpotlightSlot(el_effect_handle_t effect, int32_t index, const char *who)
    {
        if (index < 0 || static_cast<size_t>(index) >= effect->config.spotlight.lamps.size())
        {
            LOG_E("%s: index %d out of range (size=%zu)", who, index,
                  effect->config.spotlight.lamps.size());
            return nullptr;
        }
        return &effect->config.spotlight.lamps[static_cast<size_t>(index)];
    }

    /// Copies a C layer array into RendererLayers, checking only its C shape:
    /// a non-negative count and a non-null array when the count is not zero.
    /// The flag values are the enum's (see the static_asserts in
    /// capi-internal.h); whether each names exactly one layer, once, is
    /// EdgeLightingEffect::IsValidLayerList's to judge.
    bool CopyLayerArray(const uint32_t *order, int32_t count, const char *fn,
                        std::vector<EdgeLighting::RendererLayer> &out)
    {
        if (count < 0 || (count > 0 && !order))
        {
            LOG_E("%s: invalid layer list (order=%p, count=%d)", fn, (const void *)order, count);
            return false;
        }
        out.clear();
        for (int32_t i = 0; i < count; i++)
        {
            out.push_back(static_cast<EdgeLighting::RendererLayer>(order[i]));
        }
        return true;
    }

    /// Fresh effect, then the layers in @p order, then Initialize. A renderer
    /// that fails is dropped by Initialize and the rest keep running.
    /// Re-initialising a handle replaces its whole stack.
    el_result_e InitLayers(el_effect_handle_t effect, const std::vector<EdgeLighting::RendererLayer> &order)
    {
        effect->impl = std::make_unique<EdgeLighting::EdgeLightingEffect>();
        for (EdgeLighting::RendererLayer layer : order)
        {
            effect->impl->AddRenderer(layer);
        }
        if (!effect->impl->Initialize())
        {
            LOG_E("renderer initialisation failed - %zu of %zu layers kept",
                  effect->impl->GetLayerOrder().size(), order.size());
            return EL_ERROR_INIT_FAILED;
        }
        return EL_SUCCESS;
    }

    /// Shared entry check for every call that drives, reorders or queries the
    /// effect itself rather than the staging config: @c impl is created by
    /// @ref InitLayers, so until el_effect_init* has run there is no effect to
    /// call into. Those calls used to dereference it unchecked and crashed the
    /// host on a handle that had been created but not yet initialised.
    bool IsInitialized(el_effect_handle_t effect, const char *fn)
    {
        if (!effect->impl)
        {
            LOG_E("%s: effect is not initialised", fn);
            return false;
        }
        return true;
    }
}

// ==========================================================================
// Effect - config setters
// ==========================================================================

// --- Geometry ---

el_result_e el_effect_set_geometry(el_effect_handle_t effect,
                                   float width, float height, float posX, float posY, float cornerRadius)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, width, height, posX, posY, cornerRadius);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outWidth);
    VALIDATE_OUT_PTR(outHeight);
    VALIDATE_OUT_PTR(outPosX);
    VALIDATE_OUT_PTR(outPosY);
    VALIDATE_OUT_PTR(outCornerRadius);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_winding_e, winding);
    SET_AND_LOG(effect->config.geometry.winding, static_cast<EdgeLighting::Winding>(winding),
                "effect=%p, winding=%d", (void *)effect, (int)winding);
}

el_result_e el_effect_get_winding(el_effect_handle_t effect, el_winding_e *outWinding)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outWinding);
    *outWinding = static_cast<el_winding_e>(effect->config.geometry.winding);
    LOG_D("effect=%p, winding=%d", (void *)effect, (int)*outWinding);
    return EL_SUCCESS;
}

// --- Neon scalars ---

el_result_e el_effect_set_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.neon.enable, enabled != 0,
                "effect=%p, enabled=%d", (void *)effect, enabled);
}

el_result_e el_effect_get_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    *outEnabled = effect->config.neon.enable ? 1 : 0;
    LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
    return EL_SUCCESS;
}

el_result_e el_effect_set_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e mode)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_opaque_mode_e, mode);
    SET_AND_LOG(effect->config.neon.opaqueMode,
                static_cast<EdgeLighting::OpaqueMode>(mode),
                "effect=%p, mode=%d", (void *)effect, static_cast<int>(mode));
}

el_result_e el_effect_get_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e *outMode)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outMode);
    *outMode = static_cast<el_opaque_mode_e>(effect->config.neon.opaqueMode);
    LOG_D("effect=%p, mode=%d", (void *)effect, static_cast<int>(*outMode));
    return EL_SUCCESS;
}

el_result_e el_effect_set_opaque_color(el_effect_handle_t effect,
                                       float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b, a);
    SET_AND_LOG(effect->config.neon.opaqueColor, glm::vec4(r, g, b, a),
                "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
}

el_result_e el_effect_get_opaque_color(el_effect_handle_t effect,
                                       float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
    *outR = effect->config.neon.opaqueColor.r;
    *outG = effect->config.neon.opaqueColor.g;
    *outB = effect->config.neon.opaqueColor.b;
    *outA = effect->config.neon.opaqueColor.a;
    LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
    return EL_SUCCESS;
}

el_result_e el_effect_set_opaque_cutoff(el_effect_handle_t effect, el_cutoff_side_e side,
                                        el_bool_t enable, float size, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, size, softness);
    EdgeLighting::Cutoff *c = OpaqueCutoffSlot(effect, side, __func__);
    if (!c)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (AssignCutoff(*c, enable, size, softness))
    {
        LOG_I("effect=%p, side=%d, enable=%d, size=%f, softness=%f", (void *)effect,
              static_cast<int>(side), enable, size, softness);
    }
    return EL_SUCCESS;
}

el_result_e el_effect_get_opaque_cutoff(el_effect_handle_t effect, el_cutoff_side_e side,
                                        el_bool_t *outEnable, float *outSize, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnable);
    VALIDATE_OUT_PTR(outSize);
    VALIDATE_OUT_PTR(outSoftness);
    const EdgeLighting::Cutoff *c = OpaqueCutoffSlot(effect, side, __func__);
    if (!c)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    ReadCutoff(*c, outEnable, outSize, outSoftness);
    LOG_D("effect=%p, side=%d, enable=%d, size=%f, softness=%f", (void *)effect,
          static_cast<int>(side), *outEnable, *outSize, *outSoftness);
    return EL_SUCCESS;
}

el_result_e el_effect_set_inside_cutoff(el_effect_handle_t effect,
                                        el_bool_t enable, float size, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, size, softness);
    if (AssignCutoff(effect->config.neon.insideCutoff, enable, size, softness))
    {
        LOG_I("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, enable, size, softness);
    }
    return EL_SUCCESS;
}

el_result_e el_effect_get_inside_cutoff(el_effect_handle_t effect,
                                        el_bool_t *outEnable, float *outSize, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnable);
    VALIDATE_OUT_PTR(outSize);
    VALIDATE_OUT_PTR(outSoftness);
    ReadCutoff(effect->config.neon.insideCutoff, outEnable, outSize, outSoftness);
    LOG_D("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, *outEnable, *outSize, *outSoftness);
    return EL_SUCCESS;
}

el_result_e el_effect_set_outside_cutoff(el_effect_handle_t effect,
                                         el_bool_t enable, float size, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, size, softness);
    if (AssignCutoff(effect->config.neon.outsideCutoff, enable, size, softness))
    {
        LOG_I("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, enable, size, softness);
    }
    return EL_SUCCESS;
}

el_result_e el_effect_get_outside_cutoff(el_effect_handle_t effect,
                                         el_bool_t *outEnable, float *outSize, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnable);
    VALIDATE_OUT_PTR(outSize);
    VALIDATE_OUT_PTR(outSoftness);
    ReadCutoff(effect->config.neon.outsideCutoff, outEnable, outSize, outSoftness);
    LOG_D("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, *outEnable, *outSize, *outSoftness);
    return EL_SUCCESS;
}

el_result_e el_effect_set_line_width(el_effect_handle_t effect, float width)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, width);
    SET_AND_LOG(effect->config.neon.lineWidth, width,
                "effect=%p, width=%f", (void *)effect, width);
}

el_result_e el_effect_get_line_width(el_effect_handle_t effect, float *outWidth)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outWidth);
    *outWidth = effect->config.neon.lineWidth;
    LOG_D("effect=%p, width=%f", (void *)effect, *outWidth);
    return EL_SUCCESS;
}

el_result_e el_effect_set_filament_falloff(el_effect_handle_t effect, float falloff)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, falloff);
    SET_AND_LOG(effect->config.neon.filamentFalloff, falloff,
                "effect=%p, falloff=%f", (void *)effect, falloff);
}

el_result_e el_effect_get_filament_falloff(el_effect_handle_t effect, float *outFalloff)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outFalloff);
    *outFalloff = effect->config.neon.filamentFalloff;
    LOG_D("effect=%p, falloff=%f", (void *)effect, *outFalloff);
    return EL_SUCCESS;
}

el_result_e el_effect_set_intensity(el_effect_handle_t effect, float intensity)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, intensity);
    SET_AND_LOG(effect->config.neon.intensity, intensity,
                "effect=%p, intensity=%f", (void *)effect, intensity);
}

el_result_e el_effect_get_intensity(el_effect_handle_t effect, float *outIntensity)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outIntensity);
    *outIntensity = effect->config.neon.intensity;
    LOG_D("effect=%p, intensity=%f", (void *)effect, *outIntensity);
    return EL_SUCCESS;
}

el_result_e el_effect_set_glow_radius(el_effect_handle_t effect, float radius)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, radius);
    SET_AND_LOG(effect->config.neon.glowRadius, radius,
                "effect=%p, radius=%f", (void *)effect, radius);
}

el_result_e el_effect_get_glow_radius(el_effect_handle_t effect, float *outRadius)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outRadius);
    *outRadius = effect->config.neon.glowRadius;
    LOG_D("effect=%p, radius=%f", (void *)effect, *outRadius);
    return EL_SUCCESS;
}

el_result_e el_effect_set_bloom_strength(el_effect_handle_t effect, float strength)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, strength);
    SET_AND_LOG(effect->config.neon.bloomStrength, strength,
                "effect=%p, strength=%f", (void *)effect, strength);
}

el_result_e el_effect_get_bloom_strength(el_effect_handle_t effect, float *outStrength)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outStrength);
    *outStrength = effect->config.neon.bloomStrength;
    LOG_D("effect=%p, strength=%f", (void *)effect, *outStrength);
    return EL_SUCCESS;
}

el_result_e el_effect_set_glow_side(el_effect_handle_t effect, el_glow_side_e side)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_glow_side_e, side);
    SET_AND_LOG(effect->config.neon.glowSide, static_cast<EdgeLighting::GlowSide>(side),
                "effect=%p, side=%d", (void *)effect, (int)side);
}

el_result_e el_effect_get_glow_side(el_effect_handle_t effect, el_glow_side_e *outSide)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSide);
    *outSide = static_cast<el_glow_side_e>(effect->config.neon.glowSide);
    LOG_D("effect=%p, side=%d", (void *)effect, (int)*outSide);
    return EL_SUCCESS;
}

el_result_e el_effect_set_glow_side_softness(el_effect_handle_t effect, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, softness);
    SET_AND_LOG(effect->config.neon.glowSideSoftness, softness,
                "effect=%p, softness=%f", (void *)effect, softness);
}

el_result_e el_effect_get_glow_side_softness(el_effect_handle_t effect, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSoftness);
    *outSoftness = effect->config.neon.glowSideSoftness;
    LOG_D("effect=%p, softness=%f", (void *)effect, *outSoftness);
    return EL_SUCCESS;
}

el_result_e el_effect_set_blend_space(el_effect_handle_t effect, el_blend_space_e space)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_blend_space_e, space);
    SET_AND_LOG(effect->config.neon.blendSpace, static_cast<EdgeLighting::BlendSpace>(space),
                "effect=%p, space=%d", (void *)effect, (int)space);
}

el_result_e el_effect_get_blend_space(el_effect_handle_t effect, el_blend_space_e *outSpace)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSpace);
    *outSpace = static_cast<el_blend_space_e>(effect->config.neon.blendSpace);
    LOG_D("effect=%p, space=%d", (void *)effect, (int)*outSpace);
    return EL_SUCCESS;
}

el_result_e el_effect_set_hue_rotation_rate(el_effect_handle_t effect, float rate)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, rate);
    SET_AND_LOG(effect->config.neon.hueRotationRate, rate,
                "effect=%p, rate=%f", (void *)effect, rate);
}

el_result_e el_effect_get_hue_rotation_rate(el_effect_handle_t effect, float *outRate)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outRate);
    *outRate = effect->config.neon.hueRotationRate;
    LOG_D("effect=%p, rate=%f", (void *)effect, *outRate);
    return EL_SUCCESS;
}

el_result_e el_effect_set_color_transition_duration(el_effect_handle_t effect, float seconds)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, seconds);
    SET_AND_LOG(effect->config.neon.colorTransitionDuration, seconds,
                "effect=%p, seconds=%f", (void *)effect, seconds);
}

el_result_e el_effect_get_color_transition_duration(el_effect_handle_t effect, float *outSeconds)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSeconds);
    *outSeconds = effect->config.neon.colorTransitionDuration;
    LOG_D("effect=%p, seconds=%f", (void *)effect, *outSeconds);
    return EL_SUCCESS;
}

// --- Neon colour stops ---

el_result_e el_effect_set_color_stop_count(el_effect_handle_t effect, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect);
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
    effect->config.neon.colorStops.resize(newSize, DEFAULT_COLOR_STOP);
    return EL_SUCCESS;
}

el_result_e el_effect_get_color_stop_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = static_cast<int32_t>(effect->config.neon.colorStops.size());
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_set_color_stop(el_effect_handle_t effect, int32_t index,
                                     float position, float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, r, g, b, a);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
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
    VALIDATE_EFFECT_PTR(effect);
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
    VALIDATE_EFFECT_PTR(effect);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = static_cast<int32_t>(effect->config.neon.segmentBoosts.size());
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_set_segment_boost(el_effect_handle_t effect, int32_t index,
                                        float position, float length, float boost)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, length, boost);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outLength);
    VALIDATE_OUT_PTR(outBoost);
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
    VALIDATE_EFFECT_PTR(effect);
    if (effect->config.neon.segmentBoosts.empty())
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p", (void *)effect);
    effect->config.neon.segmentBoosts.clear();
    return EL_SUCCESS;
}

// --- Preserved segment boosts (id-addressed, override-proof) ---

el_result_e el_effect_acquire_preserved_segment(el_effect_handle_t effect, uint32_t *outId)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outId);
    uint32_t id = EdgeLighting::SegmentUtils::AcquireSegment(effect->config.neon);
    if (id == 0)
    {
        LOG_E("el_effect_acquire_preserved_segment: pool full (cap=%d)",
              EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outId = id;
    LOG_I("effect=%p, id=%u", (void *)effect, id);
    return EL_SUCCESS;
}

el_result_e el_effect_set_preserved_segment(el_effect_handle_t effect, uint32_t id,
                                            float position, float length, float boost)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, length, boost);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_set_preserved_segment: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &b = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
    if (b.position == position && b.length == length && b.boost == boost)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, id=%u, position=%f, length=%f, boost=%f", (void *)effect, id, position, length, boost);
    b.position = position;
    b.length = length;
    b.boost = boost;
    return EL_SUCCESS;
}

el_result_e el_effect_get_preserved_segment(el_effect_handle_t effect, uint32_t id,
                                            float *outPosition, float *outLength, float *outBoost)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outLength);
    VALIDATE_OUT_PTR(outBoost);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_get_preserved_segment: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    const auto &b = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
    *outPosition = b.position;
    *outLength = b.length;
    *outBoost = b.boost;
    LOG_D("effect=%p, id=%u, position=%f, length=%f, boost=%f", (void *)effect, id, *outPosition, *outLength, *outBoost);
    return EL_SUCCESS;
}

el_result_e el_effect_release_preserved_segment(el_effect_handle_t effect, uint32_t id)
{
    VALIDATE_EFFECT_PTR(effect);
    if (!EdgeLighting::SegmentUtils::ReleaseSegment(effect->config.neon, id))
    {
        LOG_E("el_effect_release_preserved_segment: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    LOG_I("effect=%p, id=%u", (void *)effect, id);
    return EL_SUCCESS;
}

el_result_e el_effect_get_preserved_segment_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = static_cast<int32_t>(effect->config.neon.preservedSegmentBoosts.size());
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_clear_preserved_segments(el_effect_handle_t effect)
{
    VALIDATE_EFFECT_PTR(effect);
    if (effect->config.neon.preservedSegmentBoosts.empty())
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p", (void *)effect);
    effect->config.neon.preservedSegmentBoosts.clear();
    return EL_SUCCESS;
}

// --- Preserved segment blend space + colour stops (by id) ---

el_result_e el_effect_set_preserved_segment_blend_space(el_effect_handle_t effect,
                                                        uint32_t id, el_blend_space_e blendSpace)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_blend_space_e, blendSpace);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_set_preserved_segment_blend_space: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &seg = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
    auto newVal = static_cast<EdgeLighting::BlendSpace>(blendSpace);
    if (seg.blendSpace == newVal)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, id=%u, blendSpace=%d", (void *)effect, id, (int)blendSpace);
    seg.blendSpace = newVal;
    return EL_SUCCESS;
}

el_result_e el_effect_get_preserved_segment_blend_space(el_effect_handle_t effect,
                                                        uint32_t id, el_blend_space_e *outBlendSpace)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outBlendSpace);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_get_preserved_segment_blend_space: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outBlendSpace = static_cast<el_blend_space_e>(
        effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment.blendSpace);
    LOG_D("effect=%p, id=%u, blendSpace=%d", (void *)effect, id, (int)*outBlendSpace);
    return EL_SUCCESS;
}

el_result_e el_effect_set_preserved_segment_color_stop_count(el_effect_handle_t effect,
                                                             uint32_t id, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect);
    if (count < 0)
    {
        LOG_E("el_effect_set_preserved_segment_color_stop_count: negative count");
        return EL_ERROR_INVALID_PARAMETER;
    }
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_set_preserved_segment_color_stop_count: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &stops = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment.colorStops;
    size_t newSize = static_cast<size_t>(count);
    if (stops.size() == newSize)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, id=%u, count=%d", (void *)effect, id, count);
    stops.resize(newSize);
    return EL_SUCCESS;
}

el_result_e el_effect_get_preserved_segment_color_stop_count(el_effect_handle_t effect,
                                                             uint32_t id, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_get_preserved_segment_color_stop_count: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outCount = static_cast<int32_t>(
        effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment.colorStops.size());
    LOG_D("effect=%p, id=%u, count=%d", (void *)effect, id, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_set_preserved_segment_color_stop(el_effect_handle_t effect,
                                                       uint32_t id, int32_t stopIndex,
                                                       float position, float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, r, g, b, a);
    if (stopIndex < 0)
    {
        LOG_E("el_effect_set_preserved_segment_color_stop: negative stopIndex");
        return EL_ERROR_INVALID_PARAMETER;
    }
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_set_preserved_segment_color_stop: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &stops = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment.colorStops;
    size_t stopIdx = static_cast<size_t>(stopIndex);
    if (stopIdx >= stops.size())
    {
        LOG_E("el_effect_set_preserved_segment_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, stops.size());
        return EL_ERROR_INVALID_PARAMETER;
    }
    EdgeLighting::ColorStop newStop{position, glm::vec4(r, g, b, a)};
    if (stops[stopIdx] == newStop)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, id=%u, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f",
          (void *)effect, id, stopIndex, position, r, g, b, a);
    stops[stopIdx] = newStop;
    return EL_SUCCESS;
}

el_result_e el_effect_get_preserved_segment_color_stop(el_effect_handle_t effect,
                                                       uint32_t id, int32_t stopIndex,
                                                       float *outPosition, float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_get_preserved_segment_color_stop: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    const auto &seg = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
    if (stopIndex < 0 || static_cast<size_t>(stopIndex) >= seg.colorStops.size())
    {
        LOG_E("el_effect_get_preserved_segment_color_stop: stopIndex %d out of range (size=%zu)", stopIndex, seg.colorStops.size());
        return EL_ERROR_INVALID_PARAMETER;
    }
    const auto &s = seg.colorStops[static_cast<size_t>(stopIndex)];
    *outPosition = s.position;
    *outR = s.color.r;
    *outG = s.color.g;
    *outB = s.color.b;
    *outA = s.color.a;
    LOG_D("effect=%p, id=%u, stopIndex=%d, position=%f, r=%f, g=%f, b=%f, a=%f",
          (void *)effect, id, stopIndex, *outPosition, *outR, *outG, *outB, *outA);
    return EL_SUCCESS;
}

el_result_e el_effect_clear_preserved_segment_color_stops(el_effect_handle_t effect, uint32_t id)
{
    VALIDATE_EFFECT_PTR(effect);
    int idx = EdgeLighting::SegmentUtils::FindPreservedSegment(effect->config.neon, id);
    if (idx < 0)
    {
        LOG_E("el_effect_clear_preserved_segment_color_stops: no preserved entry with id %u", id);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &stops = effect->config.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment.colorStops;
    if (stops.empty())
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, id=%u", (void *)effect, id);
    stops.clear();
    return EL_SUCCESS;
}

// --- Neon segment blend space + colour stops ---

el_result_e el_effect_set_segment_blend_space(el_effect_handle_t effect,
                                              int32_t segmentIndex, el_blend_space_e blendSpace)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_blend_space_e, blendSpace);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outBlendSpace);
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
    VALIDATE_EFFECT_PTR(effect);
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
    boosts[segIdx].colorStops.resize(newSize, DEFAULT_COLOR_STOP);
    return EL_SUCCESS;
}

el_result_e el_effect_get_segment_color_stop_count(el_effect_handle_t effect,
                                                   int32_t segmentIndex, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
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
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, r, g, b, a);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
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
    VALIDATE_EFFECT_PTR(effect);
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
    VALIDATE_EFFECT_PTR(effect);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = static_cast<int32_t>(effect->config.neon.arcs.size());
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_set_arc(el_effect_handle_t effect, int32_t index,
                              float start, float length, float intensity, el_blend_space_e blendSpace)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_blend_space_e, blendSpace);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, start, length, intensity);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outStart);
    VALIDATE_OUT_PTR(outLength);
    VALIDATE_OUT_PTR(outIntensity);
    VALIDATE_OUT_PTR(outBlendSpace);
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
    VALIDATE_EFFECT_PTR(effect);
    if (effect->config.neon.arcs.empty())
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p", (void *)effect);
    effect->config.neon.arcs.clear();
    return EL_SUCCESS;
}

// --- Spotlights ---
//
// Positions are APP COORDINATES (top-left origin, +y down), the same space
// as el_effect_set_position. Setters are grouped by concern rather than one
// per scalar; all of them address a lamp by index and none of them grows
// the list, mirroring the arc family above.

el_result_e el_effect_set_spotlight_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    const bool v = (enabled != 0);
    if (effect->config.spotlight.enable == v)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, enabled=%d", (void *)effect, (int)enabled);
    effect->config.spotlight.enable = v;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    *outEnabled = effect->config.spotlight.enable ? 1 : 0;
    LOG_D("effect=%p, enabled=%d", (void *)effect, (int)*outEnabled);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_count(el_effect_handle_t effect, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect);
    if (count < 0)
    {
        LOG_E("el_effect_set_spotlight_count: negative count");
        return EL_ERROR_INVALID_PARAMETER;
    }
    size_t newSize = static_cast<size_t>(count);
    if (effect->config.spotlight.lamps.size() == newSize)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, count=%d", (void *)effect, count);
    effect->config.spotlight.lamps.resize(newSize);
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = static_cast<int32_t>(effect->config.spotlight.lamps.size());
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_placement(el_effect_handle_t effect, int32_t index,
                                              float x, float y, float angle)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, x, y, angle);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (l->position.x == x && l->position.y == y && l->angle == angle)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, x=%f, y=%f, angle=%f", (void *)effect, index, x, y, angle);
    l->position.x = x;
    l->position.y = y;
    l->angle = angle;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_placement(el_effect_handle_t effect, int32_t index,
                                              float *outX, float *outY, float *outAngle)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outX);
    VALIDATE_OUT_PTR(outY);
    VALIDATE_OUT_PTR(outAngle);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outX = l->position.x;
    *outY = l->position.y;
    *outAngle = l->angle;
    LOG_D("effect=%p, index=%d, x=%f, y=%f, angle=%f", (void *)effect, index, *outX, *outY, *outAngle);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_beam(el_effect_handle_t effect, int32_t index,
                                         float beamAngle, float throwLength,
                                         float apertureWidth, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, beamAngle, throwLength, apertureWidth, softness);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (l->beamAngle == beamAngle && l->throwLength == throwLength &&
        l->apertureWidth == apertureWidth && l->softness == softness)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, beamAngle=%f, throwLength=%f, apertureWidth=%f, softness=%f",
          (void *)effect, index, beamAngle, throwLength, apertureWidth, softness);
    l->beamAngle = beamAngle;
    l->throwLength = throwLength;
    l->apertureWidth = apertureWidth;
    l->softness = softness;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_beam(el_effect_handle_t effect, int32_t index,
                                         float *outBeamAngle, float *outThrowLength,
                                         float *outApertureWidth, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outBeamAngle);
    VALIDATE_OUT_PTR(outThrowLength);
    VALIDATE_OUT_PTR(outApertureWidth);
    VALIDATE_OUT_PTR(outSoftness);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outBeamAngle = l->beamAngle;
    *outThrowLength = l->throwLength;
    *outApertureWidth = l->apertureWidth;
    *outSoftness = l->softness;
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_look(el_effect_handle_t effect, int32_t index,
                                         float intensity, float bloom,
                                         float bloomRadius, float colorTemp)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, intensity, bloom, bloomRadius, colorTemp);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (l->intensity == intensity && l->bloom == bloom &&
        l->bloomRadius == bloomRadius && l->colorTemp == colorTemp)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, intensity=%f, bloom=%f, bloomRadius=%f, colorTemp=%f",
          (void *)effect, index, intensity, bloom, bloomRadius, colorTemp);
    l->intensity = intensity;
    l->bloom = bloom;
    l->bloomRadius = bloomRadius;
    l->colorTemp = colorTemp;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_look(el_effect_handle_t effect, int32_t index,
                                         float *outIntensity, float *outBloom,
                                         float *outBloomRadius, float *outColorTemp)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outIntensity);
    VALIDATE_OUT_PTR(outBloom);
    VALIDATE_OUT_PTR(outBloomRadius);
    VALIDATE_OUT_PTR(outColorTemp);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outIntensity = l->intensity;
    *outBloom = l->bloom;
    *outBloomRadius = l->bloomRadius;
    *outColorTemp = l->colorTemp;
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_spread_falloff(el_effect_handle_t effect, int32_t index,
                                                   float spreadFalloff)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, spreadFalloff);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (l->spreadFalloff == spreadFalloff)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, spreadFalloff=%f", (void *)effect, index, spreadFalloff);
    l->spreadFalloff = spreadFalloff;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_spread_falloff(el_effect_handle_t effect, int32_t index,
                                                   float *outSpreadFalloff)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSpreadFalloff);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outSpreadFalloff = l->spreadFalloff;
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_tint(el_effect_handle_t effect, int32_t index,
                                         float r, float g, float b)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (l->tint.r == r && l->tint.g == g && l->tint.b == b)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, r=%f, g=%f, b=%f", (void *)effect, index, r, g, b);
    l->tint.r = r;
    l->tint.g = g;
    l->tint.b = b;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_tint(el_effect_handle_t effect, int32_t index,
                                         float *outR, float *outG, float *outB)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outR = l->tint.r;
    *outG = l->tint.g;
    *outB = l->tint.b;
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_enabled(el_effect_handle_t effect, int32_t index, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    const bool v = (enabled != 0);
    if (l->enable == v)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, enabled=%d", (void *)effect, index, (int)enabled);
    l->enable = v;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_enabled(el_effect_handle_t effect, int32_t index, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outEnabled = l->enable ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_resolution_scale(el_effect_handle_t effect, float scale)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, scale);
    if (effect->config.spotlight.resolutionScale == scale)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, scale=%f", (void *)effect, scale);
    effect->config.spotlight.resolutionScale = scale;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_resolution_scale(el_effect_handle_t effect, float *outScale)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outScale);
    *outScale = effect->config.spotlight.resolutionScale;
    LOG_D("effect=%p, scale=%f", (void *)effect, *outScale);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_clipped(el_effect_handle_t effect, int32_t index, el_bool_t clipped)
{
    VALIDATE_EFFECT_PTR(effect);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    const bool v = (clipped != 0);
    if (l->clipped == v)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, clipped=%d", (void *)effect, index, (int)clipped);
    l->clipped = v;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_clipped(el_effect_handle_t effect, int32_t index, el_bool_t *outClipped)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outClipped);
    auto *l = SpotlightSlot(effect, index, __func__);
    if (!l)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outClipped = l->clipped ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_effect_clear_spotlights(el_effect_handle_t effect)
{
    VALIDATE_EFFECT_PTR(effect);
    if (effect->config.spotlight.lamps.empty())
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p", (void *)effect);
    effect->config.spotlight.lamps.clear();
    return EL_SUCCESS;
}

// --- Spotlight clip area ---

// Parameter order mirrors el_effect_set_geometry exactly - width, height,
// x, y, cornerRadius - so a host lining the clip up with the rect can
// forward one call's output into the other. See the header.
el_result_e el_effect_set_spotlight_clip_rect(el_effect_handle_t effect,
                                              float width, float height,
                                              float x, float y,
                                              float cornerRadius)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, width, height, x, y, cornerRadius);
    EdgeLighting::ClipArea &c = effect->config.spotlight.clipArea;
    if (c.width == width && c.height == height &&
        c.position.x == x && c.position.y == y &&
        c.cornerRadius == cornerRadius)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, width=%f, height=%f, x=%f, y=%f, cornerRadius=%f",
          (void *)effect, width, height, x, y, cornerRadius);
    c.width = width;
    c.height = height;
    c.position.x = x;
    c.position.y = y;
    c.cornerRadius = cornerRadius;
    return EL_SUCCESS;
}

el_result_e el_effect_get_spotlight_clip_rect(el_effect_handle_t effect,
                                              float *outWidth, float *outHeight,
                                              float *outX, float *outY,
                                              float *outCornerRadius)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outWidth);
    VALIDATE_OUT_PTR(outHeight);
    VALIDATE_OUT_PTR(outX);
    VALIDATE_OUT_PTR(outY);
    VALIDATE_OUT_PTR(outCornerRadius);
    const EdgeLighting::ClipArea &c = effect->config.spotlight.clipArea;
    *outWidth = c.width;
    *outHeight = c.height;
    *outX = c.position.x;
    *outY = c.position.y;
    *outCornerRadius = c.cornerRadius;
    LOG_D("effect=%p, width=%f, height=%f, x=%f, y=%f, cornerRadius=%f",
          (void *)effect, *outWidth, *outHeight, *outX, *outY, *outCornerRadius);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_clip_softness(el_effect_handle_t effect, float softness)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, softness);
    SET_AND_LOG(effect->config.spotlight.clipArea.edgeSoftness, softness,
                "effect=%p, softness=%f", (void *)effect, softness);
}

el_result_e el_effect_get_spotlight_clip_softness(el_effect_handle_t effect, float *outSoftness)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSoftness);
    *outSoftness = effect->config.spotlight.clipArea.edgeSoftness;
    LOG_D("effect=%p, softness=%f", (void *)effect, *outSoftness);
    return EL_SUCCESS;
}

el_result_e el_effect_set_spotlight_clip_mode(el_effect_handle_t effect, el_clip_mode_e mode)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ENUM(el_clip_mode_e, mode);
    SET_AND_LOG(effect->config.spotlight.clipArea.mode,
                static_cast<EdgeLighting::ClipMode>(mode),
                "effect=%p, mode=%d", (void *)effect, (int)mode);
}

el_result_e el_effect_get_spotlight_clip_mode(el_effect_handle_t effect, el_clip_mode_e *outMode)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outMode);
    *outMode = static_cast<el_clip_mode_e>(effect->config.spotlight.clipArea.mode);
    LOG_D("effect=%p, mode=%d", (void *)effect, (int)*outMode);
    return EL_SUCCESS;
}

// --- Neon arc colour stops ---

el_result_e el_effect_set_arc_color_stop_count(el_effect_handle_t effect,
                                               int32_t arcIndex, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect);
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
    arcs[arcIdx].colorStops.resize(newSize, DEFAULT_COLOR_STOP);
    return EL_SUCCESS;
}

el_result_e el_effect_get_arc_color_stop_count(el_effect_handle_t effect,
                                               int32_t arcIndex, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
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
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position, r, g, b, a);
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
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
    VALIDATE_EFFECT_PTR(effect);
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
// --- Neon performance knobs ---

el_result_e el_effect_set_neon_resolution_scale(el_effect_handle_t effect, float scale)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, scale);
    SET_AND_LOG(effect->config.neon.resolutionScale, scale,
                "effect=%p, scale=%f", (void *)effect, scale);
}

el_result_e el_effect_get_neon_resolution_scale(el_effect_handle_t effect, float *outScale)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outScale);
    *outScale = effect->config.neon.resolutionScale;
    LOG_D("effect=%p, scale=%f", (void *)effect, *outScale);
    return EL_SUCCESS;
}

el_result_e el_effect_set_neon_num_samples(el_effect_handle_t effect, int32_t samples)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.neon.numSamples, samples,
                "effect=%p, samples=%d", (void *)effect, samples);
}

el_result_e el_effect_get_neon_num_samples(el_effect_handle_t effect, int32_t *outSamples)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSamples);
    *outSamples = effect->config.neon.numSamples;
    LOG_D("effect=%p, samples=%d", (void *)effect, *outSamples);
    return EL_SUCCESS;
}

el_result_e el_effect_set_neon_gradient_lut_size(el_effect_handle_t effect, int32_t size)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.neon.gradientLutSize, size,
                "effect=%p, size=%d", (void *)effect, size);
}

el_result_e el_effect_get_neon_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSize);
    *outSize = effect->config.neon.gradientLutSize;
    LOG_D("effect=%p, size=%d", (void *)effect, *outSize);
    return EL_SUCCESS;
}

// --- Droplets ---

el_result_e el_effect_set_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.droplets.enable, enabled != 0, "effect=%p, enabled=%d", (void *)effect, enabled);
}
el_result_e el_effect_get_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    *outEnabled = effect->config.droplets.enable ? 1 : 0;
    LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
    return EL_SUCCESS;
}
el_result_e el_effect_set_droplets_amount(el_effect_handle_t effect, float amount)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, amount);
    SET_AND_LOG(effect->config.droplets.amount, amount, "effect=%p, amount=%f", (void *)effect, amount);
}
el_result_e el_effect_get_droplets_amount(el_effect_handle_t effect, float *outAmount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outAmount);
    *outAmount = effect->config.droplets.amount;
    LOG_D("effect=%p, amount=%f", (void *)effect, *outAmount);
    return EL_SUCCESS;
}
el_result_e el_effect_set_droplets_speed(el_effect_handle_t effect, float speed)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, speed);
    SET_AND_LOG(effect->config.droplets.speed, speed, "effect=%p, speed=%f", (void *)effect, speed);
}
el_result_e el_effect_get_droplets_speed(el_effect_handle_t effect, float *outSpeed)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSpeed);
    *outSpeed = effect->config.droplets.speed;
    LOG_D("effect=%p, speed=%f", (void *)effect, *outSpeed);
    return EL_SUCCESS;
}
el_result_e el_effect_set_droplets_lanes(el_effect_handle_t effect, int lanes)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.droplets.lanes, lanes, "effect=%p, lanes=%d", (void *)effect, lanes);
}
el_result_e el_effect_get_droplets_lanes(el_effect_handle_t effect, int *outLanes)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outLanes);
    *outLanes = effect->config.droplets.lanes;
    LOG_D("effect=%p, lanes=%d", (void *)effect, *outLanes);
    return EL_SUCCESS;
}
el_result_e el_effect_set_droplets_band_width(el_effect_handle_t effect, float bandWidth)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, bandWidth);
    SET_AND_LOG(effect->config.droplets.bandWidth, bandWidth, "effect=%p, bandWidth=%f", (void *)effect, bandWidth);
}
el_result_e el_effect_get_droplets_band_width(el_effect_handle_t effect, float *outBandWidth)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outBandWidth);
    *outBandWidth = effect->config.droplets.bandWidth;
    LOG_D("effect=%p, bandWidth=%f", (void *)effect, *outBandWidth);
    return EL_SUCCESS;
}
el_result_e el_effect_set_droplets_band_offset(el_effect_handle_t effect, float bandOffset)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, bandOffset);
    SET_AND_LOG(effect->config.droplets.bandOffset, bandOffset, "effect=%p, bandOffset=%f", (void *)effect, bandOffset);
}
el_result_e el_effect_get_droplets_band_offset(el_effect_handle_t effect, float *outBandOffset)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outBandOffset);
    *outBandOffset = effect->config.droplets.bandOffset;
    LOG_D("effect=%p, bandOffset=%f", (void *)effect, *outBandOffset);
    return EL_SUCCESS;
}

el_result_e el_effect_set_droplets_tint(el_effect_handle_t effect, float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b, a);
    SET_AND_LOG(effect->config.droplets.tint, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
}
el_result_e el_effect_get_droplets_tint(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
    *outR = effect->config.droplets.tint.r;
    *outG = effect->config.droplets.tint.g;
    *outB = effect->config.droplets.tint.b;
    *outA = effect->config.droplets.tint.a;
    LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
    return EL_SUCCESS;
}

// --- Lens flare ---

el_result_e el_effect_set_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.lensFlare.enable, enabled != 0, "effect=%p, enabled=%d", (void *)effect, enabled);
}
el_result_e el_effect_get_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    *outEnabled = effect->config.lensFlare.enable ? 1 : 0;
    LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_perimeter_position(el_effect_handle_t effect, float position)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, position);
    SET_AND_LOG(effect->config.lensFlare.perimeterPosition, position, "effect=%p, position=%f", (void *)effect, position);
}
el_result_e el_effect_get_lens_flare_perimeter_position(el_effect_handle_t effect, float *outPosition)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPosition);
    *outPosition = effect->config.lensFlare.perimeterPosition;
    LOG_D("effect=%p, position=%f", (void *)effect, *outPosition);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_perimeter_offset(el_effect_handle_t effect, float offset)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, offset);
    SET_AND_LOG(effect->config.lensFlare.perimeterOffset, offset, "effect=%p, offset=%f", (void *)effect, offset);
}
el_result_e el_effect_get_lens_flare_perimeter_offset(el_effect_handle_t effect, float *outOffset)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outOffset);
    *outOffset = effect->config.lensFlare.perimeterOffset;
    LOG_D("effect=%p, offset=%f", (void *)effect, *outOffset);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_size(el_effect_handle_t effect, float size)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, size);
    SET_AND_LOG(effect->config.lensFlare.size, size, "effect=%p, size=%f", (void *)effect, size);
}
el_result_e el_effect_get_lens_flare_size(el_effect_handle_t effect, float *outSize)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSize);
    *outSize = effect->config.lensFlare.size;
    LOG_D("effect=%p, size=%f", (void *)effect, *outSize);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_color(el_effect_handle_t effect, float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b, a);
    SET_AND_LOG(effect->config.lensFlare.color, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
}
el_result_e el_effect_get_lens_flare_color(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
    *outR = effect->config.lensFlare.color.r;
    *outG = effect->config.lensFlare.color.g;
    *outB = effect->config.lensFlare.color.b;
    *outA = effect->config.lensFlare.color.a;
    LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_intensity(el_effect_handle_t effect, float intensity)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, intensity);
    SET_AND_LOG(effect->config.lensFlare.intensity, intensity, "effect=%p, intensity=%f", (void *)effect, intensity);
}
el_result_e el_effect_get_lens_flare_intensity(el_effect_handle_t effect, float *outIntensity)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outIntensity);
    *outIntensity = effect->config.lensFlare.intensity;
    LOG_D("effect=%p, intensity=%f", (void *)effect, *outIntensity);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_spread(el_effect_handle_t effect, float spread)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, spread);
    SET_AND_LOG(effect->config.lensFlare.spread, spread, "effect=%p, spread=%f", (void *)effect, spread);
}
el_result_e el_effect_get_lens_flare_spread(el_effect_handle_t effect, float *outSpread)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outSpread);
    *outSpread = effect->config.lensFlare.spread;
    LOG_D("effect=%p, spread=%f", (void *)effect, *outSpread);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ghost_spacing(el_effect_handle_t effect, float ghostSpacing)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, ghostSpacing);
    SET_AND_LOG(effect->config.lensFlare.ghostSpacing, ghostSpacing, "effect=%p, ghostSpacing=%f", (void *)effect, ghostSpacing);
}
el_result_e el_effect_get_lens_flare_ghost_spacing(el_effect_handle_t effect, float *outGhostSpacing)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outGhostSpacing);
    *outGhostSpacing = effect->config.lensFlare.ghostSpacing;
    LOG_D("effect=%p, ghostSpacing=%f", (void *)effect, *outGhostSpacing);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ghost_size(el_effect_handle_t effect, float ghostSize)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, ghostSize);
    SET_AND_LOG(effect->config.lensFlare.ghostSize, ghostSize, "effect=%p, ghostSize=%f", (void *)effect, ghostSize);
}
el_result_e el_effect_get_lens_flare_ghost_size(el_effect_handle_t effect, float *outGhostSize)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outGhostSize);
    *outGhostSize = effect->config.lensFlare.ghostSize;
    LOG_D("effect=%p, ghostSize=%f", (void *)effect, *outGhostSize);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ghost_offset(el_effect_handle_t effect, float ghostOffset)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, ghostOffset);
    SET_AND_LOG(effect->config.lensFlare.ghostOffset, ghostOffset, "effect=%p, ghostOffset=%f", (void *)effect, ghostOffset);
}
el_result_e el_effect_get_lens_flare_ghost_offset(el_effect_handle_t effect, float *outGhostOffset)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outGhostOffset);
    *outGhostOffset = effect->config.lensFlare.ghostOffset;
    LOG_D("effect=%p, ghostOffset=%f", (void *)effect, *outGhostOffset);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ghost_color(el_effect_handle_t effect, float r, float g, float b)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b);
    SET_AND_LOG(effect->config.lensFlare.ghostColor, glm::vec3(r, g, b), "effect=%p, r=%f, g=%f, b=%f", (void *)effect, r, g, b);
}
el_result_e el_effect_get_lens_flare_ghost_color(el_effect_handle_t effect, float *outR, float *outG, float *outB)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    *outR = effect->config.lensFlare.ghostColor.r;
    *outG = effect->config.lensFlare.ghostColor.g;
    *outB = effect->config.lensFlare.ghostColor.b;
    LOG_D("effect=%p, r=%f, g=%f, b=%f", (void *)effect, *outR, *outG, *outB);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ghost_tint(el_effect_handle_t effect, float ghostTint)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, ghostTint);
    SET_AND_LOG(effect->config.lensFlare.ghostTint, ghostTint, "effect=%p, ghostTint=%f", (void *)effect, ghostTint);
}
el_result_e el_effect_get_lens_flare_ghost_tint(el_effect_handle_t effect, float *outGhostTint)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outGhostTint);
    *outGhostTint = effect->config.lensFlare.ghostTint;
    LOG_D("effect=%p, ghostTint=%f", (void *)effect, *outGhostTint);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_flare_center(el_effect_handle_t effect, float x, float y)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, x, y);
    SET_AND_LOG(effect->config.lensFlare.flareCenter, glm::vec2(x, y), "effect=%p, x=%f, y=%f", (void *)effect, x, y);
}
el_result_e el_effect_get_lens_flare_flare_center(el_effect_handle_t effect, float *outX, float *outY)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outX);
    VALIDATE_OUT_PTR(outY);
    *outX = effect->config.lensFlare.flareCenter.x;
    *outY = effect->config.lensFlare.flareCenter.y;
    LOG_D("effect=%p, x=%f, y=%f", (void *)effect, *outX, *outY);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_ray_density(el_effect_handle_t effect, float rayDensity)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, rayDensity);
    SET_AND_LOG(effect->config.lensFlare.rayDensity, rayDensity, "effect=%p, rayDensity=%f", (void *)effect, rayDensity);
}
el_result_e el_effect_get_lens_flare_ray_density(el_effect_handle_t effect, float *outRayDensity)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outRayDensity);
    *outRayDensity = effect->config.lensFlare.rayDensity;
    LOG_D("effect=%p, rayDensity=%f", (void *)effect, *outRayDensity);
    return EL_SUCCESS;
}
el_result_e el_effect_set_lens_flare_rotation_rate(el_effect_handle_t effect, float rate)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, rate);
    SET_AND_LOG(effect->config.lensFlare.rotationRate, rate, "effect=%p, rate=%f", (void *)effect, rate);
}
el_result_e el_effect_get_lens_flare_rotation_rate(el_effect_handle_t effect, float *outRate)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outRate);
    *outRate = effect->config.lensFlare.rotationRate;
    LOG_D("effect=%p, rate=%f", (void *)effect, *outRate);
    return EL_SUCCESS;
}
// --- Lens flare performance knob ---

el_result_e el_effect_set_lens_flare_resolution_scale(el_effect_handle_t effect, float scale)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, scale);
    SET_AND_LOG(effect->config.lensFlare.resolutionScale, scale,
                "effect=%p, scale=%f", (void *)effect, scale);
}

el_result_e el_effect_get_lens_flare_resolution_scale(el_effect_handle_t effect, float *outScale)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outScale);
    *outScale = effect->config.lensFlare.resolutionScale;
    LOG_D("effect=%p, scale=%f", (void *)effect, *outScale);
    return EL_SUCCESS;
}

// ==========================================================================
// Debug overlays and diagnostics
//
// Order matches the declaration order in el-effect.h. `opaque_only` sits
// last because it is the one entry here that drives the NEON layer rather
// than the overlay layer.
// ==========================================================================

el_result_e el_effect_set_debug_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.debug.enable, enabled != 0,
                "effect=%p, enabled=%d", (void *)effect, enabled);
}

el_result_e el_effect_get_debug_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outEnabled);
    *outEnabled = effect->config.debug.enable ? 1 : 0;
    LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
    return EL_SUCCESS;
}

el_result_e el_effect_set_debug_show_gradient_lut(el_effect_handle_t effect, el_bool_t show)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.debug.showGradientLUT, show != 0,
                "effect=%p, show=%d", (void *)effect, show);
}

el_result_e el_effect_get_debug_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outShow);
    *outShow = effect->config.debug.showGradientLUT ? 1 : 0;
    LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
    return EL_SUCCESS;
}

el_result_e el_effect_set_debug_show_color_stops(el_effect_handle_t effect, el_bool_t show)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.debug.showColorStops, show != 0,
                "effect=%p, show=%d", (void *)effect, show);
}

el_result_e el_effect_get_debug_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outShow);
    *outShow = effect->config.debug.showColorStops ? 1 : 0;
    LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
    return EL_SUCCESS;
}

el_result_e el_effect_set_debug_show_wireframe(el_effect_handle_t effect, el_bool_t show)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.debug.showWireframe, show != 0, "effect=%p, show=%d", (void *)effect, show);
}

el_result_e el_effect_get_debug_show_wireframe(el_effect_handle_t effect, el_bool_t *outShow)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outShow);
    *outShow = effect->config.debug.showWireframe ? 1 : 0;
    LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
    return EL_SUCCESS;
}

el_result_e el_effect_set_debug_wireframe_color(el_effect_handle_t effect, float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, r, g, b, a);
    SET_AND_LOG(effect->config.debug.wireframeColor, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
}

el_result_e el_effect_get_debug_wireframe_color(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outR);
    VALIDATE_OUT_PTR(outG);
    VALIDATE_OUT_PTR(outB);
    VALIDATE_OUT_PTR(outA);
    *outR = effect->config.debug.wireframeColor.r;
    *outG = effect->config.debug.wireframeColor.g;
    *outB = effect->config.debug.wireframeColor.b;
    *outA = effect->config.debug.wireframeColor.a;
    LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
    return EL_SUCCESS;
}

// Lives in DebugConfig with the overlay flags, but it selects which of the
// NEON renderer's passes run rather than drawing an overlay - which is why
// it sits last in this group.
el_result_e el_effect_set_debug_opaque_only(el_effect_handle_t effect, el_bool_t opaqueOnly)
{
    VALIDATE_EFFECT_PTR(effect);
    SET_AND_LOG(effect->config.debug.opaqueOnly, opaqueOnly != 0,
                "effect=%p, opaqueOnly=%d", (void *)effect, opaqueOnly);
}

el_result_e el_effect_get_debug_opaque_only(el_effect_handle_t effect, el_bool_t *outOpaqueOnly)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outOpaqueOnly);
    *outOpaqueOnly = effect->config.debug.opaqueOnly ? 1 : 0;
    LOG_D("effect=%p, opaqueOnly=%d", (void *)effect, *outOpaqueOnly);
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
    return el_effect_init_with_renderers(effect, EL_RENDERER_ALL);
}

el_result_e el_effect_init_with_renderers(el_effect_handle_t effect, uint32_t rendererMask)
{
    LOG_I("effect=%p, rendererMask=0x%x", (void *)effect, rendererMask);
    VALIDATE_EFFECT_PTR(effect);
    try
    {
        // The mask decides INCLUSION only; the order is the default one.
        std::vector<EdgeLighting::RendererLayer> order;
        for (EdgeLighting::RendererLayer layer : EdgeLighting::EdgeLightingEffect::GetDefaultLayerOrder())
        {
            if (rendererMask & static_cast<uint32_t>(layer))
            {
                order.push_back(layer);
            }
        }
        return InitLayers(effect, order);
    }
    catch (const std::exception &e)
    {
        LOG_E("exception: %s", e.what());
        return mapExceptionToResult(e);
    }
}

el_result_e el_effect_init_with_renderer_order(el_effect_handle_t effect,
                                               const uint32_t *order, int32_t count)
{
    LOG_I("effect=%p, count=%d", (void *)effect, count);
    VALIDATE_EFFECT_PTR(effect);
    try
    {
        // Validated BEFORE InitLayers replaces the effect, so a malformed
        // list leaves any previous stack running.
        std::vector<EdgeLighting::RendererLayer> layers;
        if (!CopyLayerArray(order, count, __func__, layers) ||
            !EdgeLighting::EdgeLightingEffect::IsValidLayerList(layers))
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        return InitLayers(effect, layers);
    }
    catch (const std::exception &e)
    {
        LOG_E("exception: %s", e.what());
        return mapExceptionToResult(e);
    }
}

el_result_e el_effect_set_renderer_order(el_effect_handle_t effect,
                                         const uint32_t *order, int32_t count)
{
    LOG_I("effect=%p, count=%d", (void *)effect, count);
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
    try
    {
        std::vector<EdgeLighting::RendererLayer> layers;
        if (!CopyLayerArray(order, count, __func__, layers) ||
            !effect->impl->SetLayerOrder(layers))
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        return EL_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_E("exception: %s", e.what());
        return mapExceptionToResult(e);
    }
}

el_result_e el_effect_get_renderer_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = effect->impl ? static_cast<int32_t>(effect->impl->GetLayerOrder().size()) : 0;
    return EL_SUCCESS;
}

el_result_e el_effect_get_renderer_at(el_effect_handle_t effect, int32_t index, uint32_t *outLayer)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outLayer);
    const std::vector<EdgeLighting::RendererLayer> order =
        effect->impl ? effect->impl->GetLayerOrder() : std::vector<EdgeLighting::RendererLayer>();
    if (index < 0 || static_cast<size_t>(index) >= order.size())
    {
        LOG_E("el_effect_get_renderer_at: index %d out of range (size=%zu)", index, order.size());
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outLayer = static_cast<uint32_t>(order[static_cast<size_t>(index)]);
    return EL_SUCCESS;
}

el_result_e el_effect_capture(el_effect_handle_t effect)
{
    LOG_I("effect=%p", (void *)effect);
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
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
    VALIDATE_EFFECT_PTR(effect);
    REQUIRE_FINITE(EL_ERROR_INVALID_PARAMETER, deltaTime);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
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
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
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
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
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
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
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
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outPlaying);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
    *outPlaying = effect->impl->GetClock().IsPlaying() ? 1 : 0;
    LOG_D("effect=%p, playing=%d", (void *)effect, *outPlaying);
    return EL_SUCCESS;
}

// --- Animation attachment ---

el_result_e el_effect_attach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
{
    LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ANIM_PTR(anim);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
    if (anim->ptr)
    {
        effect->impl->Attach(anim->ptr);
    }
    return EL_SUCCESS;
}

el_result_e el_effect_detach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
{
    LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ANIM_PTR(anim);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
    if (anim->ptr)
    {
        effect->impl->Detach(anim->ptr);
    }
    return EL_SUCCESS;
}

el_result_e el_effect_detach_all_animations(el_effect_handle_t effect)
{
    LOG_I("effect=%p", (void *)effect);
    VALIDATE_EFFECT_PTR(effect);
    if (!IsInitialized(effect, __func__))
    {
        return EL_ERROR_INVALID_HANDLE;
    }
    effect->impl->GetAnimationManager().DetachAll();
    return EL_SUCCESS;
}

el_result_e el_effect_get_animation_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_OUT_PTR(outCount);
    *outCount = effect->impl ? static_cast<int32_t>(effect->impl->GetAnimationManager().GetCount()) : 0;
    LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
    return EL_SUCCESS;
}

el_result_e el_effect_contains_animation(el_effect_handle_t effect,
                                         el_animation_handle_t anim, el_bool_t *outContains)
{
    VALIDATE_EFFECT_PTR(effect);
    VALIDATE_ANIM_PTR(anim);
    VALIDATE_OUT_PTR(outContains);
    *outContains = (anim->ptr && effect->impl && effect->impl->GetAnimationManager().Contains(anim->ptr)) ? 1 : 0;
    LOG_D("effect=%p, anim=%p, contains=%d", (void *)effect, (void *)anim, *outContains);
    return EL_SUCCESS;
}
