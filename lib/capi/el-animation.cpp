#include "capi-internal.h"

extern "C"
{

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

    el_result_e el_animation_add_preserved_segment_field(el_animation_handle_t anim,
                                                         uint32_t id, el_segment_field_e field, el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, id=%u, field=%d, mod=%p", (void *)anim, id, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_preserved_segment_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_preserved_segment_field");
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_preserved_segment_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddPreservedSegmentField(id, static_cast<EdgeLighting::SegmentField>(field), mod->ptr);
        return EL_SUCCESS;
    }

    el_result_e el_animation_add_preserved_segment_stop_field(el_animation_handle_t anim,
                                                              uint32_t id, int32_t stopIndex,
                                                              el_color_stop_field_e field, el_modulator_handle_t mod)
    {
        LOG_I("anim=%p, id=%u, stopIndex=%d, field=%d, mod=%p", (void *)anim, id, stopIndex, (int)field, (void *)mod);
        VALIDATE_ANIM_PTR(anim, "el_animation_add_preserved_segment_stop_field");
        VALIDATE_MOD_PTR(mod, "el_animation_add_preserved_segment_stop_field");
        if (stopIndex < 0)
        {
            LOG_E("el_animation_add_preserved_segment_stop_field: negative stopIndex");
            return EL_ERROR_INVALID_PARAMETER;
        }
        auto *fb = dynamic_cast<EdgeLighting::FieldBoundAnimation *>(anim->ptr.get());
        if (!fb)
        {
            LOG_E("el_animation_add_preserved_segment_stop_field: animation is not a FieldBoundAnimation");
            return EL_ERROR_INVALID_PARAMETER;
        }
        fb->AddPreservedSegmentStopField(id, static_cast<size_t>(stopIndex),
                                         static_cast<EdgeLighting::ColorStopField>(field), mod->ptr);
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

} // extern "C"
