#include "capi-internal.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace EdgeLighting
{
    namespace Capi
    {
        /// Declared in capi-internal.h; defined here because el-animation.cpp
        /// reads it and this is the translation unit that writes it.
        thread_local el_effect_handle_t gDispatchTarget = nullptr;
    }
}

namespace
{
    /// Seed for colour stops created by the set_*_count growers. Matches what
    /// el-effect.h documents (position 0, opaque white); a plain resize()
    /// value-initialises to transparent black instead, and because
    /// ColorStop::color.a is an emission scale rather than a blend opacity,
    /// that renders as "dark here" instead of as a merely unset colour.
    const EdgeLighting::ColorStop DEFAULT_COLOR_STOP{0.0f, glm::vec4(1.0f)};

    using EdgeLighting::Capi::Command;
    using EdgeLighting::Capi::CommandOp;

    /// Run one queued mutation. RENDER THREAD, called from the drain in
    /// el_effect_update with gDispatchTarget already set - which is what makes
    /// a state-change callback fired from in here defer like any other.
    ///
    /// An op whose animation went null between queue and drain is skipped
    /// rather than treated as an error: the only way to get one is a handle
    /// created with a null animation, which every el_animation_* entry point
    /// already tolerates.
    void runCommand(el_effect_handle_t effect, Command &cmd)
    {
        auto &st = *effect->threaded;
        auto &impl = *effect->impl;

        // Effect-level ops first - these carry no animation.
        switch (cmd.op)
        {
        case CommandOp::CLOCK_PLAY:
        {
            impl.GetClock().Play();
            return;
        }
        case CommandOp::CLOCK_PAUSE:
        {
            impl.GetClock().Pause();
            return;
        }
        case CommandOp::DETACH_ALL:
        {
            impl.GetAnimationManager().DetachAll();
            // Acknowledge before dropping the list - this is what releases
            // each animation back to the data thread. See AnimMirror::attached.
            for (auto &entry : st.attached)
            {
                if (entry.second)
                {
                    entry.second->attached.store(false, std::memory_order_release);
                }
            }
            st.attached.clear();
            return;
        }
        default:
        {
            break;
        }
        }

        if (!cmd.anim)
        {
            return;
        }

        switch (cmd.op)
        {
        case CommandOp::PLAY:
        {
            cmd.anim->Play();
            break;
        }
        case CommandOp::PAUSE:
        {
            cmd.anim->Pause();
            break;
        }
        case CommandOp::STOP:
        {
            cmd.anim->Stop();
            break;
        }
        case CommandOp::RESET:
        {
            // Into a throwaway seeded from the effect's BASE config, not into
            // the data thread's staging - staging lives on the other thread,
            // and the config half of Reset is redundant for an attached
            // animation anyway. See ThreadedState::resetScratch.
            st.resetScratch = impl.GetConfig();
            cmd.anim->Reset(st.resetScratch);
            break;
        }
        case CommandOp::CAPTURE_BASELINE:
        {
            cmd.anim->CaptureBaseline(impl.GetConfig());
            break;
        }
        case CommandOp::SET_ELAPSED:
        {
            cmd.anim->SetElapsed(cmd.f);
            break;
        }
        case CommandOp::SET_PROGRESS:
        {
            cmd.anim->SetProgress(cmd.f);
            break;
        }
        case CommandOp::SET_SPEED:
        {
            cmd.anim->SetSpeed(cmd.f);
            break;
        }
        case CommandOp::SET_DURATION:
        {
            cmd.anim->SetDuration(cmd.f);
            break;
        }
        case CommandOp::SET_END_ACTION:
        {
            cmd.anim->SetEndAction(toEndAction(static_cast<el_end_action_e>(cmd.i)));
            break;
        }
        case CommandOp::SET_PLAYBACK_MODE:
        {
            cmd.anim->SetPlaybackMode(toPlaybackMode(static_cast<el_playback_mode_e>(cmd.i)));
            break;
        }
        case CommandOp::ATTACH:
        {
            impl.Attach(cmd.anim);
            st.attached.emplace_back(cmd.anim, cmd.mirror);
            // Already true - the data thread set it when it queued this - but
            // set it again so a re-attach queued behind a DETACH still ends up
            // true after both have run, whatever order the host asked in.
            if (cmd.mirror)
            {
                cmd.mirror->attached.store(true, std::memory_order_release);
            }
            break;
        }
        case CommandOp::DETACH:
        {
            impl.Detach(cmd.anim);
            for (auto &entry : st.attached)
            {
                if (entry.first == cmd.anim && entry.second)
                {
                    entry.second->attached.store(false, std::memory_order_release);
                }
            }
            st.attached.erase(
                std::remove_if(st.attached.begin(), st.attached.end(),
                               [&cmd](const auto &entry)
                               { return entry.first == cmd.anim; }),
                st.attached.end());
            break;
        }
        default:
        {
            break;
        }
        }
    }

    /// Republish the render thread's animation state for the data thread's
    /// getters. RENDER THREAD, after the manager has ticked.
    void publishMirrors(el_effect_handle_t effect)
    {
        auto &st = *effect->threaded;
        st.clockPlaying.store(effect->impl->GetClock().IsPlaying(),
                              std::memory_order_relaxed);
        for (auto &entry : st.attached)
        {
            if (!entry.second || !entry.first)
            {
                continue;
            }
            entry.second->state.store(static_cast<int32_t>(fromAnimationState(entry.first->GetState())),
                                      std::memory_order_relaxed);
            entry.second->elapsed.store(entry.first->GetElapsed(), std::memory_order_relaxed);
            entry.second->progress.store(entry.first->GetProgress(), std::memory_order_relaxed);
        }
    }
}

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

    el_result_e el_effect_set_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e mode)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_opaque_mode");
        SET_AND_LOG(effect->config.neon.opaqueMode,
                    static_cast<EdgeLighting::OpaqueMode>(mode),
                    "effect=%p, mode=%d", (void *)effect, static_cast<int>(mode));
    }

    el_result_e el_effect_get_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e *outMode)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_opaque_mode");
        VALIDATE_OUT_PTR(outMode, "el_effect_get_opaque_mode");
        *outMode = static_cast<el_opaque_mode_e>(effect->config.neon.opaqueMode);
        LOG_D("effect=%p, mode=%d", (void *)effect, static_cast<int>(*outMode));
        return EL_SUCCESS;
    }

    // Lives in DebugConfig with the overlay flags, but it selects which of the
    // NEON renderer's passes run - so it is declared and implemented here with
    // the rest of the opaque group rather than with the overlays.

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

    el_result_e el_effect_set_opaque_softness(el_effect_handle_t effect, float softness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_opaque_softness");
        SET_AND_LOG(effect->config.neon.opaqueSoftness, softness,
                    "effect=%p, softness=%f", (void *)effect, softness);
    }

    el_result_e el_effect_get_opaque_softness(el_effect_handle_t effect, float *outSoftness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_opaque_softness");
        VALIDATE_OUT_PTR(outSoftness, "el_effect_get_opaque_softness");
        *outSoftness = effect->config.neon.opaqueSoftness;
        LOG_D("effect=%p, softness=%f", (void *)effect, *outSoftness);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_inside_cutoff(el_effect_handle_t effect,
                                            el_bool_t enable, float size, float softness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_inside_cutoff");
        auto &c = effect->config.neon.insideCutoff;
        bool en = (enable != 0);
        if (c.enable == en && c.size == size && c.softness == softness)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, enable, size, softness);
        c.enable = en;
        c.size = size;
        c.softness = softness;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_inside_cutoff(el_effect_handle_t effect,
                                            el_bool_t *outEnable, float *outSize, float *outSoftness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_inside_cutoff");
        VALIDATE_OUT_PTR(outEnable, "el_effect_get_inside_cutoff");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_inside_cutoff");
        VALIDATE_OUT_PTR(outSoftness, "el_effect_get_inside_cutoff");
        const auto &c = effect->config.neon.insideCutoff;
        *outEnable = c.enable ? 1 : 0;
        *outSize = c.size;
        *outSoftness = c.softness;
        LOG_D("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, *outEnable, *outSize, *outSoftness);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_outside_cutoff(el_effect_handle_t effect,
                                             el_bool_t enable, float size, float softness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_outside_cutoff");
        auto &c = effect->config.neon.outsideCutoff;
        bool en = (enable != 0);
        if (c.enable == en && c.size == size && c.softness == softness)
        {
            return EL_SUCCESS;
        }
        LOG_I("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, enable, size, softness);
        c.enable = en;
        c.size = size;
        c.softness = softness;
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_outside_cutoff(el_effect_handle_t effect,
                                             el_bool_t *outEnable, float *outSize, float *outSoftness)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_outside_cutoff");
        VALIDATE_OUT_PTR(outEnable, "el_effect_get_outside_cutoff");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_outside_cutoff");
        VALIDATE_OUT_PTR(outSoftness, "el_effect_get_outside_cutoff");
        const auto &c = effect->config.neon.outsideCutoff;
        *outEnable = c.enable ? 1 : 0;
        *outSize = c.size;
        *outSoftness = c.softness;
        LOG_D("effect=%p, enable=%d, size=%f, softness=%f", (void *)effect, *outEnable, *outSize, *outSoftness);
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
        effect->config.neon.colorStops.resize(newSize, DEFAULT_COLOR_STOP);
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

    // --- Preserved segment boosts (id-addressed, override-proof) ---

    el_result_e el_effect_acquire_preserved_segment(el_effect_handle_t effect, uint32_t *outId)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_acquire_preserved_segment");
        VALIDATE_OUT_PTR(outId, "el_effect_acquire_preserved_segment");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_preserved_segment");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_preserved_segment");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_preserved_segment");
        VALIDATE_OUT_PTR(outLength, "el_effect_get_preserved_segment");
        VALIDATE_OUT_PTR(outBoost, "el_effect_get_preserved_segment");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_release_preserved_segment");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_preserved_segment_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_preserved_segment_count");
        *outCount = static_cast<int32_t>(effect->config.neon.preservedSegmentBoosts.size());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_clear_preserved_segments(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_preserved_segments");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_preserved_segment_blend_space");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_preserved_segment_blend_space");
        VALIDATE_OUT_PTR(outBlendSpace, "el_effect_get_preserved_segment_blend_space");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_preserved_segment_color_stop_count");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_preserved_segment_color_stop_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_preserved_segment_color_stop_count");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_preserved_segment_color_stop");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_preserved_segment_color_stop");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_preserved_segment_color_stop");
        VALIDATE_OUT_PTR(outR, "el_effect_get_preserved_segment_color_stop");
        VALIDATE_OUT_PTR(outG, "el_effect_get_preserved_segment_color_stop");
        VALIDATE_OUT_PTR(outB, "el_effect_get_preserved_segment_color_stop");
        VALIDATE_OUT_PTR(outA, "el_effect_get_preserved_segment_color_stop");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_clear_preserved_segment_color_stops");
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
        boosts[segIdx].colorStops.resize(newSize, DEFAULT_COLOR_STOP);
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
        arcs[arcIdx].colorStops.resize(newSize, DEFAULT_COLOR_STOP);
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
    // --- Neon performance knobs ---

    el_result_e el_effect_set_neon_resolution_scale(el_effect_handle_t effect, float scale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_neon_resolution_scale");
        SET_AND_LOG(effect->config.neon.resolutionScale, scale,
                    "effect=%p, scale=%f", (void *)effect, scale);
    }

    el_result_e el_effect_get_neon_resolution_scale(el_effect_handle_t effect, float *outScale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_neon_resolution_scale");
        VALIDATE_OUT_PTR(outScale, "el_effect_get_neon_resolution_scale");
        *outScale = effect->config.neon.resolutionScale;
        LOG_D("effect=%p, scale=%f", (void *)effect, *outScale);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_neon_num_samples(el_effect_handle_t effect, int32_t samples)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_neon_num_samples");
        SET_AND_LOG(effect->config.neon.numSamples, samples,
                    "effect=%p, samples=%d", (void *)effect, samples);
    }

    el_result_e el_effect_get_neon_num_samples(el_effect_handle_t effect, int32_t *outSamples)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_neon_num_samples");
        VALIDATE_OUT_PTR(outSamples, "el_effect_get_neon_num_samples");
        *outSamples = effect->config.neon.numSamples;
        LOG_D("effect=%p, samples=%d", (void *)effect, *outSamples);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_neon_gradient_lut_size(el_effect_handle_t effect, int32_t size)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_neon_gradient_lut_size");
        SET_AND_LOG(effect->config.neon.gradientLutSize, size,
                    "effect=%p, size=%d", (void *)effect, size);
    }

    el_result_e el_effect_get_neon_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_neon_gradient_lut_size");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_neon_gradient_lut_size");
        *outSize = effect->config.neon.gradientLutSize;
        LOG_D("effect=%p, size=%d", (void *)effect, *outSize);
        return EL_SUCCESS;
    }

    // --- Droplets ---

    el_result_e el_effect_set_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_renderer_enabled");
        SET_AND_LOG(effect->config.droplets.enable, enabled != 0, "effect=%p, enabled=%d", (void *)effect, enabled);
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
        SET_AND_LOG(effect->config.droplets.amount, amount, "effect=%p, amount=%f", (void *)effect, amount);
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
        SET_AND_LOG(effect->config.droplets.speed, speed, "effect=%p, speed=%f", (void *)effect, speed);
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
        SET_AND_LOG(effect->config.droplets.lanes, lanes, "effect=%p, lanes=%d", (void *)effect, lanes);
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
        SET_AND_LOG(effect->config.droplets.bandWidth, bandWidth, "effect=%p, bandWidth=%f", (void *)effect, bandWidth);
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
        SET_AND_LOG(effect->config.droplets.bandOffset, bandOffset, "effect=%p, bandOffset=%f", (void *)effect, bandOffset);
    }
    el_result_e el_effect_get_droplets_band_offset(el_effect_handle_t effect, float *outBandOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_droplets_band_offset");
        VALIDATE_OUT_PTR(outBandOffset, "el_effect_get_droplets_band_offset");
        *outBandOffset = effect->config.droplets.bandOffset;
        LOG_D("effect=%p, bandOffset=%f", (void *)effect, *outBandOffset);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_droplets_tint(el_effect_handle_t effect, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_droplets_tint");
        SET_AND_LOG(effect->config.droplets.tint, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }
    el_result_e el_effect_get_droplets_tint(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
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

    // --- Lens flare ---

    el_result_e el_effect_set_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_renderer_enabled");
        SET_AND_LOG(effect->config.lensFlare.enable, enabled != 0, "effect=%p, enabled=%d", (void *)effect, enabled);
    }
    el_result_e el_effect_get_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_renderer_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_lens_flare_renderer_enabled");
        *outEnabled = effect->config.lensFlare.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_perimeter_position(el_effect_handle_t effect, float position)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_perimeter_position");
        SET_AND_LOG(effect->config.lensFlare.perimeterPosition, position, "effect=%p, position=%f", (void *)effect, position);
    }
    el_result_e el_effect_get_lens_flare_perimeter_position(el_effect_handle_t effect, float *outPosition)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_perimeter_position");
        VALIDATE_OUT_PTR(outPosition, "el_effect_get_lens_flare_perimeter_position");
        *outPosition = effect->config.lensFlare.perimeterPosition;
        LOG_D("effect=%p, position=%f", (void *)effect, *outPosition);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_perimeter_offset(el_effect_handle_t effect, float offset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_perimeter_offset");
        SET_AND_LOG(effect->config.lensFlare.perimeterOffset, offset, "effect=%p, offset=%f", (void *)effect, offset);
    }
    el_result_e el_effect_get_lens_flare_perimeter_offset(el_effect_handle_t effect, float *outOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_perimeter_offset");
        VALIDATE_OUT_PTR(outOffset, "el_effect_get_lens_flare_perimeter_offset");
        *outOffset = effect->config.lensFlare.perimeterOffset;
        LOG_D("effect=%p, offset=%f", (void *)effect, *outOffset);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_size(el_effect_handle_t effect, float size)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_size");
        SET_AND_LOG(effect->config.lensFlare.size, size, "effect=%p, size=%f", (void *)effect, size);
    }
    el_result_e el_effect_get_lens_flare_size(el_effect_handle_t effect, float *outSize)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_size");
        VALIDATE_OUT_PTR(outSize, "el_effect_get_lens_flare_size");
        *outSize = effect->config.lensFlare.size;
        LOG_D("effect=%p, size=%f", (void *)effect, *outSize);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_color(el_effect_handle_t effect, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_color");
        SET_AND_LOG(effect->config.lensFlare.color, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }
    el_result_e el_effect_get_lens_flare_color(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_lens_flare_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_lens_flare_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_lens_flare_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_lens_flare_color");
        *outR = effect->config.lensFlare.color.r;
        *outG = effect->config.lensFlare.color.g;
        *outB = effect->config.lensFlare.color.b;
        *outA = effect->config.lensFlare.color.a;
        LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_intensity(el_effect_handle_t effect, float intensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_intensity");
        SET_AND_LOG(effect->config.lensFlare.intensity, intensity, "effect=%p, intensity=%f", (void *)effect, intensity);
    }
    el_result_e el_effect_get_lens_flare_intensity(el_effect_handle_t effect, float *outIntensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_intensity");
        VALIDATE_OUT_PTR(outIntensity, "el_effect_get_lens_flare_intensity");
        *outIntensity = effect->config.lensFlare.intensity;
        LOG_D("effect=%p, intensity=%f", (void *)effect, *outIntensity);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_spread(el_effect_handle_t effect, float spread)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_spread");
        SET_AND_LOG(effect->config.lensFlare.spread, spread, "effect=%p, spread=%f", (void *)effect, spread);
    }
    el_result_e el_effect_get_lens_flare_spread(el_effect_handle_t effect, float *outSpread)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_spread");
        VALIDATE_OUT_PTR(outSpread, "el_effect_get_lens_flare_spread");
        *outSpread = effect->config.lensFlare.spread;
        LOG_D("effect=%p, spread=%f", (void *)effect, *outSpread);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ghost_spacing(el_effect_handle_t effect, float ghostSpacing)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ghost_spacing");
        SET_AND_LOG(effect->config.lensFlare.ghostSpacing, ghostSpacing, "effect=%p, ghostSpacing=%f", (void *)effect, ghostSpacing);
    }
    el_result_e el_effect_get_lens_flare_ghost_spacing(el_effect_handle_t effect, float *outGhostSpacing)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ghost_spacing");
        VALIDATE_OUT_PTR(outGhostSpacing, "el_effect_get_lens_flare_ghost_spacing");
        *outGhostSpacing = effect->config.lensFlare.ghostSpacing;
        LOG_D("effect=%p, ghostSpacing=%f", (void *)effect, *outGhostSpacing);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ghost_size(el_effect_handle_t effect, float ghostSize)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ghost_size");
        SET_AND_LOG(effect->config.lensFlare.ghostSize, ghostSize, "effect=%p, ghostSize=%f", (void *)effect, ghostSize);
    }
    el_result_e el_effect_get_lens_flare_ghost_size(el_effect_handle_t effect, float *outGhostSize)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ghost_size");
        VALIDATE_OUT_PTR(outGhostSize, "el_effect_get_lens_flare_ghost_size");
        *outGhostSize = effect->config.lensFlare.ghostSize;
        LOG_D("effect=%p, ghostSize=%f", (void *)effect, *outGhostSize);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ghost_offset(el_effect_handle_t effect, float ghostOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ghost_offset");
        SET_AND_LOG(effect->config.lensFlare.ghostOffset, ghostOffset, "effect=%p, ghostOffset=%f", (void *)effect, ghostOffset);
    }
    el_result_e el_effect_get_lens_flare_ghost_offset(el_effect_handle_t effect, float *outGhostOffset)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ghost_offset");
        VALIDATE_OUT_PTR(outGhostOffset, "el_effect_get_lens_flare_ghost_offset");
        *outGhostOffset = effect->config.lensFlare.ghostOffset;
        LOG_D("effect=%p, ghostOffset=%f", (void *)effect, *outGhostOffset);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ghost_color(el_effect_handle_t effect, float r, float g, float b)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ghost_color");
        SET_AND_LOG(effect->config.lensFlare.ghostColor, glm::vec3(r, g, b), "effect=%p, r=%f, g=%f, b=%f", (void *)effect, r, g, b);
    }
    el_result_e el_effect_get_lens_flare_ghost_color(el_effect_handle_t effect, float *outR, float *outG, float *outB)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ghost_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_lens_flare_ghost_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_lens_flare_ghost_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_lens_flare_ghost_color");
        *outR = effect->config.lensFlare.ghostColor.r;
        *outG = effect->config.lensFlare.ghostColor.g;
        *outB = effect->config.lensFlare.ghostColor.b;
        LOG_D("effect=%p, r=%f, g=%f, b=%f", (void *)effect, *outR, *outG, *outB);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ghost_tint(el_effect_handle_t effect, float ghostTint)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ghost_tint");
        SET_AND_LOG(effect->config.lensFlare.ghostTint, ghostTint, "effect=%p, ghostTint=%f", (void *)effect, ghostTint);
    }
    el_result_e el_effect_get_lens_flare_ghost_tint(el_effect_handle_t effect, float *outGhostTint)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ghost_tint");
        VALIDATE_OUT_PTR(outGhostTint, "el_effect_get_lens_flare_ghost_tint");
        *outGhostTint = effect->config.lensFlare.ghostTint;
        LOG_D("effect=%p, ghostTint=%f", (void *)effect, *outGhostTint);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_flare_center(el_effect_handle_t effect, float x, float y)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_flare_center");
        SET_AND_LOG(effect->config.lensFlare.flareCenter, glm::vec2(x, y), "effect=%p, x=%f, y=%f", (void *)effect, x, y);
    }
    el_result_e el_effect_get_lens_flare_flare_center(el_effect_handle_t effect, float *outX, float *outY)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_flare_center");
        VALIDATE_OUT_PTR(outX, "el_effect_get_lens_flare_flare_center");
        VALIDATE_OUT_PTR(outY, "el_effect_get_lens_flare_flare_center");
        *outX = effect->config.lensFlare.flareCenter.x;
        *outY = effect->config.lensFlare.flareCenter.y;
        LOG_D("effect=%p, x=%f, y=%f", (void *)effect, *outX, *outY);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_ray_density(el_effect_handle_t effect, float rayDensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_ray_density");
        SET_AND_LOG(effect->config.lensFlare.rayDensity, rayDensity, "effect=%p, rayDensity=%f", (void *)effect, rayDensity);
    }
    el_result_e el_effect_get_lens_flare_ray_density(el_effect_handle_t effect, float *outRayDensity)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_ray_density");
        VALIDATE_OUT_PTR(outRayDensity, "el_effect_get_lens_flare_ray_density");
        *outRayDensity = effect->config.lensFlare.rayDensity;
        LOG_D("effect=%p, rayDensity=%f", (void *)effect, *outRayDensity);
        return EL_SUCCESS;
    }
    el_result_e el_effect_set_lens_flare_rotation_rate(el_effect_handle_t effect, float rate)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_rotation_rate");
        SET_AND_LOG(effect->config.lensFlare.rotationRate, rate, "effect=%p, rate=%f", (void *)effect, rate);
    }
    el_result_e el_effect_get_lens_flare_rotation_rate(el_effect_handle_t effect, float *outRate)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_rotation_rate");
        VALIDATE_OUT_PTR(outRate, "el_effect_get_lens_flare_rotation_rate");
        *outRate = effect->config.lensFlare.rotationRate;
        LOG_D("effect=%p, rate=%f", (void *)effect, *outRate);
        return EL_SUCCESS;
    }
    // --- Lens flare performance knob ---

    el_result_e el_effect_set_lens_flare_resolution_scale(el_effect_handle_t effect, float scale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_lens_flare_resolution_scale");
        SET_AND_LOG(effect->config.lensFlare.resolutionScale, scale,
                    "effect=%p, scale=%f", (void *)effect, scale);
    }

    el_result_e el_effect_get_lens_flare_resolution_scale(el_effect_handle_t effect, float *outScale)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_lens_flare_resolution_scale");
        VALIDATE_OUT_PTR(outScale, "el_effect_get_lens_flare_resolution_scale");
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_enabled");
        SET_AND_LOG(effect->config.debug.enable, enabled != 0,
                    "effect=%p, enabled=%d", (void *)effect, enabled);
    }

    el_result_e el_effect_get_debug_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_enabled");
        VALIDATE_OUT_PTR(outEnabled, "el_effect_get_debug_enabled");
        *outEnabled = effect->config.debug.enable ? 1 : 0;
        LOG_D("effect=%p, enabled=%d", (void *)effect, *outEnabled);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_debug_show_gradient_lut(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_show_gradient_lut");
        SET_AND_LOG(effect->config.debug.showGradientLUT, show != 0,
                    "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_debug_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_show_gradient_lut");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_debug_show_gradient_lut");
        *outShow = effect->config.debug.showGradientLUT ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_debug_show_color_stops(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_show_color_stops");
        SET_AND_LOG(effect->config.debug.showColorStops, show != 0,
                    "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_debug_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_show_color_stops");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_debug_show_color_stops");
        *outShow = effect->config.debug.showColorStops ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_debug_show_wireframe(el_effect_handle_t effect, el_bool_t show)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_show_wireframe");
        SET_AND_LOG(effect->config.debug.showWireframe, show != 0, "effect=%p, show=%d", (void *)effect, show);
    }

    el_result_e el_effect_get_debug_show_wireframe(el_effect_handle_t effect, el_bool_t *outShow)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_show_wireframe");
        VALIDATE_OUT_PTR(outShow, "el_effect_get_debug_show_wireframe");
        *outShow = effect->config.debug.showWireframe ? 1 : 0;
        LOG_D("effect=%p, show=%d", (void *)effect, *outShow);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_debug_wireframe_color(el_effect_handle_t effect, float r, float g, float b, float a)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_wireframe_color");
        SET_AND_LOG(effect->config.debug.wireframeColor, glm::vec4(r, g, b, a), "effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, r, g, b, a);
    }

    el_result_e el_effect_get_debug_wireframe_color(el_effect_handle_t effect, float *outR, float *outG, float *outB, float *outA)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_wireframe_color");
        VALIDATE_OUT_PTR(outR, "el_effect_get_debug_wireframe_color");
        VALIDATE_OUT_PTR(outG, "el_effect_get_debug_wireframe_color");
        VALIDATE_OUT_PTR(outB, "el_effect_get_debug_wireframe_color");
        VALIDATE_OUT_PTR(outA, "el_effect_get_debug_wireframe_color");
        *outR = effect->config.debug.wireframeColor.r;
        *outG = effect->config.debug.wireframeColor.g;
        *outB = effect->config.debug.wireframeColor.b;
        *outA = effect->config.debug.wireframeColor.a;
        LOG_D("effect=%p, r=%f, g=%f, b=%f, a=%f", (void *)effect, *outR, *outG, *outB, *outA);
        return EL_SUCCESS;
    }

    el_result_e el_effect_set_debug_opaque_only(el_effect_handle_t effect, el_bool_t opaqueOnly)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_debug_opaque_only");
        SET_AND_LOG(effect->config.debug.opaqueOnly, opaqueOnly != 0,
                    "effect=%p, opaqueOnly=%d", (void *)effect, opaqueOnly);
    }

    el_result_e el_effect_get_debug_opaque_only(el_effect_handle_t effect, el_bool_t *outOpaqueOnly)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_debug_opaque_only");
        VALIDATE_OUT_PTR(outOpaqueOnly, "el_effect_get_debug_opaque_only");
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
        if (isSplit(effect))
        {
            // Animation handles outlive the effect in plenty of hosts ("destroy
            // the effect, then the animations"), and each one attached to this
            // effect still points at it. Sever those now, or the first
            // el_animation_* call after this is a use-after-free on the queue.
            //
            // Safe to touch data-thread state from here: by the shutdown
            // protocol the data thread has already called el_effect_shutdown
            // and stopped touching the handle.
            for (auto *handle : effect->threaded->stagedHandles)
            {
                handle->stagedOwner = nullptr;
                if (handle->mirror)
                {
                    handle->mirror->attached.store(false, std::memory_order_release);
                }
            }
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_init_with_renderers");
        try
        {
            effect->impl = std::make_unique<EdgeLighting::EdgeLightingEffect>();

            // ONE bit per layer: the content layers dense from 0, the debug
            // layer on the top bit (see el_renderer_flags_e), which is what
            // pins its value while content layers keep taking the next free
            // low bit. The deprecated "optimized" and wireframe
            // aliases that used to be ORed in beside these are gone: the
            // half-res paths are a resolution scale on the neon and flare
            // layers, and the bounding box is one of the debug layer's
            // overlays. Nothing can double-register a layer any more, which is
            // what the paired tests here existed to prevent.
            //
            // Compositing order is THIS sequence of ifs, and it matches the bit
            // order: content layers first on the low bits, the debug layer
            // last on the highest.
            if (rendererMask & EL_RENDERER_NEON)
            {
                LOG_I("registering NeonRenderer");
                effect->impl->AddRenderer(std::make_shared<EdgeLighting::NeonRenderer>());
            }
            if (rendererMask & EL_RENDERER_DROPLETS)
            {
                LOG_I("registering DropletsRenderer");
                effect->impl->AddRenderer(std::make_shared<EdgeLighting::DropletsRenderer>());
            }
            if (rendererMask & EL_RENDERER_LENS_FLARE)
            {
                LOG_I("registering LensFlareRenderer");
                effect->impl->AddRenderer(std::make_shared<EdgeLighting::LensFlareRenderer>());
            }
            // LAST, so the annotations sit above every layer they describe -
            // after the neon whose glow the strip and markers measure, and
            // above the droplets and flare that would otherwise cover them.
            if (rendererMask & EL_RENDERER_DEBUG)
            {
                LOG_I("registering DebugRenderer");
                effect->impl->AddRenderer(std::make_shared<EdgeLighting::DebugRenderer>());
            }
            if (!effect->impl->Initialize())
            {
                LOG_E("el_effect_init_with_renderers: renderer initialisation failed");
                return EL_ERROR_INIT_FAILED;
            }
            if (isSplit(effect))
            {
                // Re-seed: the host may have configured the effect between
                // choosing the mode and initialising. Nothing else is running
                // yet, so this needs no lock - but taking it costs nothing on
                // a one-off path and keeps every touch of `consumed` uniform.
                auto &st = *effect->threaded;
                std::lock_guard<std::mutex> lock(st.mutex);
                st.consumed = effect->config;
                st.clockPlaying.store(effect->impl->GetClock().IsPlaying(),
                                      std::memory_order_relaxed);
            }
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_set_threading_mode(el_effect_handle_t effect, el_threading_mode_e mode)
    {
        LOG_I("effect=%p, mode=%d", (void *)effect, (int)mode);
        VALIDATE_EFFECT_PTR(effect, "el_effect_set_threading_mode");
        if (mode != EL_THREADING_SINGLE && mode != EL_THREADING_SPLIT)
        {
            LOG_E("el_effect_set_threading_mode: unknown mode %d", (int)mode);
            return EL_ERROR_INVALID_PARAMETER;
        }
        // Immutable past init: the mode decides who owns the staging config,
        // and a live render thread is already reading on that assumption.
        if (effect->impl)
        {
            LOG_E("el_effect_set_threading_mode: effect already initialised");
            return EL_ERROR_INVALID_PARAMETER;
        }
        if (effect->mode == mode)
        {
            return EL_SUCCESS;
        }
        try
        {
            effect->mode = mode;
            if (mode == EL_THREADING_SPLIT)
            {
                effect->threaded = std::make_unique<EdgeLighting::Capi::ThreadedState>();
                // Seed both slots from staging so a host that configures the
                // effect up front and then starts its render thread gets the
                // config it authored on frame one, published or not.
                effect->threaded->consumed = effect->config;
                effect->threaded->pending = effect->config;
            }
            else
            {
                effect->threaded.reset();
            }
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_publish(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_publish");
        if (!isSplit(effect))
        {
            return EL_SUCCESS;
        }
        try
        {
            auto &st = *effect->threaded;
            std::lock_guard<std::mutex> lock(st.mutex);
            if (st.closed)
            {
                return EL_SUCCESS;
            }
            // Copy-ASSIGN into the warm slot, never a copy-construct: the
            // assignment reuses the vector capacity already there, so a steady
            // stream of publishes allocates nothing. Same property
            // refreshActiveConfig's mScratchConfig documents at length.
            st.pending = effect->config;
            st.hasPending = true;
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_poll_callbacks(el_effect_handle_t effect)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_poll_callbacks");
        if (!isSplit(effect))
        {
            return EL_SUCCESS;
        }
        auto &st = *effect->threaded;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            std::swap(st.callbacks, st.callbackDrain);
        }
        // Invoked OUTSIDE the lock. A host callback is arbitrary code and may
        // well call straight back into el_effect_set_* / el_effect_publish -
        // which is the entire point of deferring it to this thread, and would
        // self-deadlock if we were still holding the mutex.
        for (auto &cb : st.callbackDrain)
        {
            if (cb.complete)
            {
                cb.complete(cb.userData);
            }
            else if (cb.stateChanged)
            {
                cb.stateChanged(cb.previous, cb.current, cb.userData);
            }
        }
        st.callbackDrain.clear();
        return EL_SUCCESS;
    }

    el_result_e el_effect_shutdown(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_shutdown");
        if (!isSplit(effect))
        {
            return EL_SUCCESS;
        }
        auto &st = *effect->threaded;
        std::lock_guard<std::mutex> lock(st.mutex);
        st.closed = true;
        return EL_SUCCESS;
    }

    el_result_e el_effect_capture(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_capture");
        if (isSplit(effect))
        {
            // Not merely unsafe - meaningless. In split mode staging is
            // UPSTREAM of the effect's base config, so the value this would
            // fetch is one the data thread published itself.
            LOG_E("el_effect_capture: not available in EL_THREADING_SPLIT mode");
            return EL_ERROR_INVALID_PARAMETER;
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_update");
        try
        {
            if (!isSplit(effect))
            {
                effect->impl->SetConfig(effect->config);
                effect->impl->Update(deltaTime);
                return EL_SUCCESS;
            }

            auto &st = *effect->threaded;
            {
                std::lock_guard<std::mutex> lock(st.mutex);
                // SWAP, not copy. This is the whole reason the mailbox is two
                // slots and a mutex rather than one slot: the render thread's
                // time under the lock is six vector pointer swaps, so it can
                // never be made to wait on the data thread's copy.
                if (st.hasPending)
                {
                    std::swap(st.pending, st.consumed);
                    st.hasPending = false;
                }
                std::swap(st.commands, st.commandDrain);
            }

            // Everything below runs outside the lock. gDispatchTarget marks
            // the window in which a host callback fired by a command or by the
            // manager's own tick must be deferred rather than run here.
            EdgeLighting::Capi::gDispatchTarget = effect;
            for (auto &cmd : st.commandDrain)
            {
                runCommand(effect, cmd);
            }
            // Releases the AnimationPtr each command held, but keeps the
            // capacity the next swap hands back to the producer.
            st.commandDrain.clear();

            effect->impl->SetConfig(st.consumed);
            effect->impl->Update(deltaTime);
            publishMirrors(effect);
            EdgeLighting::Capi::gDispatchTarget = nullptr;
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            // Must not leave the marker set on a thread that is about to go
            // back to the host - a later direct callback would defer into a
            // queue nobody is polling.
            EdgeLighting::Capi::gDispatchTarget = nullptr;
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
        if (isSplit(effect))
        {
            // The mirror is a frame stale, so this filters repeat calls
            // without being load-bearing - Clock::Play is idempotent, and a
            // duplicate command is one branch on the render thread.
            if (effect->threaded->clockPlaying.load(std::memory_order_relaxed))
            {
                return EL_SUCCESS;
            }
            LOG_I("effect=%p", (void *)effect);
            return enqueueCommand(effect, {EdgeLighting::Capi::CommandOp::CLOCK_PLAY});
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_clock_pause");
        if (isSplit(effect))
        {
            if (!effect->threaded->clockPlaying.load(std::memory_order_relaxed))
            {
                return EL_SUCCESS;
            }
            LOG_I("effect=%p", (void *)effect);
            return enqueueCommand(effect, {EdgeLighting::Capi::CommandOp::CLOCK_PAUSE});
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
        VALIDATE_EFFECT_PTR(effect, "el_effect_clock_is_playing");
        VALIDATE_OUT_PTR(outPlaying, "el_effect_clock_is_playing");
        if (isSplit(effect))
        {
            *outPlaying = effect->threaded->clockPlaying.load(std::memory_order_relaxed) ? 1 : 0;
        }
        else
        {
            *outPlaying = effect->impl->GetClock().IsPlaying() ? 1 : 0;
        }
        LOG_D("effect=%p, playing=%d", (void *)effect, *outPlaying);
        return EL_SUCCESS;
    }

    // --- Animation attachment ---

    el_result_e el_effect_attach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
    {
        LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
        VALIDATE_EFFECT_PTR(effect, "el_effect_attach_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_attach_animation");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        if (!isSplit(effect))
        {
            effect->impl->Attach(anim->ptr);
            return EL_SUCCESS;
        }
        try
        {
            auto &st = *effect->threaded;
            // The manager ignores duplicates; this mirrors that here so the
            // data thread's count cannot drift above the render thread's.
            if (std::find(st.stagedAttached.begin(), st.stagedAttached.end(), anim->ptr) !=
                st.stagedAttached.end())
            {
                return EL_SUCCESS;
            }
            if (!anim->mirror)
            {
                anim->mirror = std::make_shared<EdgeLighting::Capi::AnimMirror>();
            }
            // Seed the data-thread shadows here, the last moment at which the
            // animation is provably not on the render thread's list yet, so
            // this read of the live object cannot race the drain.
            anim->shadowSpeed = anim->ptr->GetSpeed();
            anim->shadowDuration = anim->ptr->GetDuration();
            anim->shadowEndAction = static_cast<int32_t>(fromEndAction(anim->ptr->GetEndAction()));
            anim->shadowPlaybackMode = static_cast<int32_t>(fromPlaybackMode(anim->ptr->GetPlaybackMode()));
            anim->mirror->state.store(static_cast<int32_t>(fromAnimationState(anim->ptr->GetState())),
                                      std::memory_order_relaxed);
            anim->mirror->elapsed.store(anim->ptr->GetElapsed(), std::memory_order_relaxed);
            anim->mirror->progress.store(anim->ptr->GetProgress(), std::memory_order_relaxed);
            // Set BEFORE the command is queued, so the window between here and
            // the render thread running the ATTACH already routes mutations
            // through the queue rather than applying them inline.
            anim->mirror->attached.store(true, std::memory_order_release);

            EdgeLighting::Capi::Command cmd;
            cmd.op = EdgeLighting::Capi::CommandOp::ATTACH;
            cmd.anim = anim->ptr;
            cmd.mirror = anim->mirror;
            const el_result_e rc = enqueueCommand(effect, std::move(cmd));
            if (rc != EL_SUCCESS)
            {
                return rc;
            }
            // Only once the command is safely queued, so a full queue does not
            // leave the data thread believing in an attach that never happened.
            st.stagedAttached.push_back(anim->ptr);
            if (std::find(st.stagedHandles.begin(), st.stagedHandles.end(), anim) ==
                st.stagedHandles.end())
            {
                st.stagedHandles.push_back(anim);
            }
            anim->stagedOwner = effect;
            return EL_SUCCESS;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return mapExceptionToResult(e);
        }
    }

    el_result_e el_effect_detach_animation(el_effect_handle_t effect, el_animation_handle_t anim)
    {
        LOG_I("effect=%p, anim=%p", (void *)effect, (void *)anim);
        VALIDATE_EFFECT_PTR(effect, "el_effect_detach_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_detach_animation");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        if (!isSplit(effect))
        {
            effect->impl->Detach(anim->ptr);
            return EL_SUCCESS;
        }
        EdgeLighting::Capi::Command cmd;
        cmd.op = EdgeLighting::Capi::CommandOp::DETACH;
        cmd.anim = anim->ptr;
        const el_result_e rc = enqueueCommand(effect, std::move(cmd));
        if (rc != EL_SUCCESS)
        {
            return rc;
        }
        auto &st = *effect->threaded;
        st.stagedAttached.erase(
            std::remove(st.stagedAttached.begin(), st.stagedAttached.end(), anim->ptr),
            st.stagedAttached.end());
        // stagedOwner and the stagedHandles entry deliberately SURVIVE. The
        // render thread has not run the DETACH yet, so the animation is still
        // its to touch and mutations must keep queuing; splitOwner clears the
        // pointer once the acknowledgement arrives.
        return EL_SUCCESS;
    }

    el_result_e el_effect_detach_all_animations(el_effect_handle_t effect)
    {
        LOG_I("effect=%p", (void *)effect);
        VALIDATE_EFFECT_PTR(effect, "el_effect_detach_all_animations");
        if (!isSplit(effect))
        {
            effect->impl->GetAnimationManager().DetachAll();
            return EL_SUCCESS;
        }
        const el_result_e rc = enqueueCommand(effect, {EdgeLighting::Capi::CommandOp::DETACH_ALL});
        if (rc != EL_SUCCESS)
        {
            return rc;
        }
        // Same asynchrony as the single detach: stagedOwner survives on every
        // handle until the render thread acknowledges the DETACH_ALL.
        effect->threaded->stagedAttached.clear();
        return EL_SUCCESS;
    }

    el_result_e el_effect_get_animation_count(el_effect_handle_t effect, int32_t *outCount)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_get_animation_count");
        VALIDATE_OUT_PTR(outCount, "el_effect_get_animation_count");
        // Answered from the data thread's own records in split mode. It issued
        // every attach and detach, so it does not need to reach across for
        // this - and a queued command not yet drained still counts, which is
        // the answer a caller reading back its own writes expects.
        *outCount = isSplit(effect)
                        ? static_cast<int32_t>(effect->threaded->stagedAttached.size())
                        : static_cast<int32_t>(effect->impl->GetAnimationManager().GetCount());
        LOG_D("effect=%p, count=%d", (void *)effect, *outCount);
        return EL_SUCCESS;
    }

    el_result_e el_effect_contains_animation(el_effect_handle_t effect,
                                             el_animation_handle_t anim, el_bool_t *outContains)
    {
        VALIDATE_EFFECT_PTR(effect, "el_effect_contains_animation");
        VALIDATE_ANIM_PTR(anim, "el_effect_contains_animation");
        VALIDATE_OUT_PTR(outContains, "el_effect_contains_animation");
        if (isSplit(effect))
        {
            const auto &staged = effect->threaded->stagedAttached;
            *outContains = (anim->ptr &&
                            std::find(staged.begin(), staged.end(), anim->ptr) != staged.end())
                               ? 1
                               : 0;
        }
        else
        {
            *outContains = (anim->ptr && effect->impl->GetAnimationManager().Contains(anim->ptr)) ? 1 : 0;
        }
        LOG_D("effect=%p, anim=%p, contains=%d", (void *)effect, (void *)anim, *outContains);
        return EL_SUCCESS;
    }

} // extern "C"
