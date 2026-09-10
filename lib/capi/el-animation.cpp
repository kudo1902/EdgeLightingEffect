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
        // Unregister from the split-mode effect's LIVE-handle list so a later
        // el_effect_detach_all_animations cannot dereference this handle.
        //
        // Deliberately does NOT detach the animation. The manager holds its own
        // shared_ptr and keeps ticking it, which is exactly what destroying an
        // attached handle does in single mode - so stagedAttached, which is
        // what the count and contains getters read, is left alone.
        if (anim->stagedOwner && anim->stagedOwner->threaded)
        {
            auto &handles = anim->stagedOwner->threaded->stagedHandles;
            handles.erase(std::remove(handles.begin(), handles.end(), anim), handles.end());
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
                                                             el_easing_e easing,
                                                             float maxLength)
    {
        try
        {
            auto *handle = new el_animation_handle_impl{std::make_shared<EdgeLighting::OutlineTracer>(duration, toEasing(easing), maxLength)};
            LOG_I("anim=%p, duration=%f, easing=%d, maxLength=%f", (void *)handle, duration, (int)easing, maxLength);
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
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_IF_SPLIT(anim, PLAY, 0.0f, 0);
        anim->ptr->Play();
        return EL_SUCCESS;
    }

    el_result_e el_animation_pause(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANIM_PTR(anim, "el_animation_pause");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_IF_SPLIT(anim, PAUSE, 0.0f, 0);
        anim->ptr->Pause();
        return EL_SUCCESS;
    }

    el_result_e el_animation_stop(el_animation_handle_t anim)
    {
        LOG_I("anim=%p", (void *)anim);
        VALIDATE_ANIM_PTR(anim, "el_animation_stop");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_IF_SPLIT(anim, STOP, 0.0f, 0);
        anim->ptr->Stop();
        return EL_SUCCESS;
    }

    el_result_e el_animation_reset(el_animation_handle_t anim, el_effect_handle_t effect)
    {
        LOG_I("anim=%p, effect=%p", (void *)anim, (void *)effect);
        VALIDATE_ANIM_PTR(anim, "el_animation_reset");
        VALIDATE_EFFECT_PTR(effect, "el_animation_reset");
        // Queued against the effect it is ATTACHED to, which need not be the
        // one passed in. The render thread runs it against that effect's base
        // config rather than this staging one - see runCommand.
        QUEUE_IF_SPLIT(anim, RESET, 0.0f, 0);
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
        // The manual-composition escape hatch, from before the effect owned an
        // AnimationManager. For an ATTACHED animation in split mode there is
        // nothing sensible to do with it: the manager is already ticking this
        // object every frame on the other thread, so running it here is a data
        // race and not a feature. Detached, it works exactly as it always has.
        if (splitOwner(anim))
        {
            LOG_E("el_animation_update: animation is attached to an "
                  "EL_THREADING_SPLIT effect - the render thread ticks it");
            return EL_ERROR_INVALID_PARAMETER;
        }
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
        // See el_animation_update - same escape hatch, same reason.
        if (splitOwner(anim))
        {
            LOG_E("el_animation_apply: animation is attached to an "
                  "EL_THREADING_SPLIT effect - the render thread composites it");
            return EL_ERROR_INVALID_PARAMETER;
        }
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
        if (splitOwner(anim) && anim->mirror)
        {
            *outState = static_cast<el_animation_state_e>(
                anim->mirror->state.load(std::memory_order_relaxed));
        }
        else if (!anim->ptr)
        {
            *outState = EL_ANIM_STATE_STOPPED;
        }
        else
        {
            *outState = fromAnimationState(anim->ptr->GetState());
        }
        LOG_D("anim=%p, state=%d", (void *)anim, (int)*outState);
        return EL_SUCCESS;
    }

    el_result_e el_animation_get_elapsed(el_animation_handle_t anim, float *outElapsed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_get_elapsed");
        VALIDATE_OUT_PTR(outElapsed, "el_animation_get_elapsed");
        // The render thread republishes this every frame; reading the live
        // Animation from here would race its tick.
        *outElapsed = (splitOwner(anim) && anim->mirror)
                        ? anim->mirror->elapsed.load(std::memory_order_relaxed)
                        : (anim->ptr ? anim->ptr->GetElapsed() : 0.0f);
        LOG_D("anim=%p, elapsed=%f", (void *)anim, *outElapsed);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_elapsed(el_animation_handle_t anim, float elapsed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_elapsed");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_IF_SPLIT(anim, SET_ELAPSED, elapsed, 0);
        if (anim->ptr->GetElapsed() == elapsed)
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
        // The render thread republishes this every frame; reading the live
        // Animation from here would race its tick.
        *outProgress = (splitOwner(anim) && anim->mirror)
                        ? anim->mirror->progress.load(std::memory_order_relaxed)
                        : (anim->ptr ? anim->ptr->GetProgress() : 0.0f);
        LOG_D("anim=%p, progress=%f", (void *)anim, *outProgress);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_progress(el_animation_handle_t anim, float progress)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_progress");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_IF_SPLIT(anim, SET_PROGRESS, progress, 0);
        if (anim->ptr->GetProgress() == progress)
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
        *outAction = splitOwner(anim)
                         ? static_cast<el_end_action_e>(anim->shadowEndAction)
                         : (anim->ptr ? fromEndAction(anim->ptr->GetEndAction())
                                      : EL_END_ACTION_HOLD_CURRENT);
        LOG_D("anim=%p, action=%d", (void *)anim, (int)*outAction);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_end_action(el_animation_handle_t anim, el_end_action_e action)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_end_action");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_SHADOWED(anim, SET_END_ACTION, 0.0f, static_cast<int32_t>(action),
                       shadowEndAction, static_cast<int32_t>(action));
        const auto newVal = toEndAction(action);
        if (anim->ptr->GetEndAction() == newVal)
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
        QUEUE_IF_SPLIT(anim, CAPTURE_BASELINE, 0.0f, 0);
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
        *outMode = splitOwner(anim)
                       ? static_cast<el_playback_mode_e>(anim->shadowPlaybackMode)
                       : (anim->ptr ? fromPlaybackMode(anim->ptr->GetPlaybackMode())
                                    : EL_PLAYBACK_LOOP);
        LOG_D("anim=%p, mode=%d", (void *)anim, (int)*outMode);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_playback_mode(el_animation_handle_t anim, el_playback_mode_e mode)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_playback_mode");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_SHADOWED(anim, SET_PLAYBACK_MODE, 0.0f, static_cast<int32_t>(mode),
                       shadowPlaybackMode, static_cast<int32_t>(mode));
        const auto newVal = toPlaybackMode(mode);
        if (anim->ptr->GetPlaybackMode() == newVal)
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
        *outSeconds = splitOwner(anim) ? anim->shadowDuration
                                        : (anim->ptr ? anim->ptr->GetDuration() : 0.0f);
        LOG_D("anim=%p, seconds=%f", (void *)anim, *outSeconds);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_duration(el_animation_handle_t anim, float seconds)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_duration");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_SHADOWED(anim, SET_DURATION, seconds, 0, shadowDuration, seconds);
        if (anim->ptr->GetDuration() == seconds)
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
        *outSpeed = splitOwner(anim) ? anim->shadowSpeed
                                      : (anim->ptr ? anim->ptr->GetSpeed() : 1.0f);
        LOG_D("anim=%p, speed=%f", (void *)anim, *outSpeed);
        return EL_SUCCESS;
    }

    el_result_e el_animation_set_speed(el_animation_handle_t anim, float speed)
    {
        VALIDATE_ANIM_PTR(anim, "el_animation_set_speed");
        if (!anim->ptr)
        {
            return EL_SUCCESS;
        }
        QUEUE_SHADOWED(anim, SET_SPEED, speed, 0, shadowSpeed, speed);
        if (anim->ptr->GetSpeed() == speed)
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
            // gDispatchTarget is non-null exactly inside a split-mode render
            // thread frame. Deferring there is what makes it legal for the
            // host's callback to turn round and call el_effect_set_*.
            el_effect_handle_t target = EdgeLighting::Capi::gDispatchTarget;
            if (target != nullptr && target->threaded != nullptr)
            {
                EdgeLighting::Capi::PendingCallback pending;
                pending.complete = callback;
                pending.userData = userData;
                enqueueCallback(target, pending);
                return;
            }
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
            const el_animation_state_e previous = fromAnimationState(prev);
            const el_animation_state_e current = fromAnimationState(now);
            // See OnComplete above.
            el_effect_handle_t target = EdgeLighting::Capi::gDispatchTarget;
            if (target != nullptr && target->threaded != nullptr)
            {
                EdgeLighting::Capi::PendingCallback pending;
                pending.stateChanged = callback;
                pending.userData = userData;
                pending.previous = previous;
                pending.current = current;
                enqueueCallback(target, pending);
                return;
            }
            callback(previous, current, userData);
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
