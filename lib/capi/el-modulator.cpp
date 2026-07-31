#include "capi-internal.h"

extern "C"
{

    // ==========================================================================
    // Modulator factories
    // ==========================================================================

    el_modulator_handle_t el_modulator_create_constant(float value)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Constant>(value)};
            LOG_I("mod=%p, value=%f", (void *)handle, value);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_oscillator(float frequency,
                                                         float minValue, float maxValue, float phase, el_waveform_e waveform)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Oscillator>(
                frequency, minValue, maxValue, phase, toWaveform(waveform))};
            LOG_I("mod=%p, frequency=%f, minValue=%f, maxValue=%f, phase=%f, waveform=%d", (void *)handle, frequency, minValue, maxValue, phase, (int)waveform);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_ease(float from, float to,
                                                   float duration, el_easing_e easing, el_bool_t loop)
    {
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Ease>(
                from, to, duration, toEasing(easing), loop != 0)};
            LOG_I("mod=%p, from=%f, to=%f, duration=%f, easing=%d, loop=%d", (void *)handle, from, to, duration, (int)easing, loop);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_sequence(el_bool_t loop)
    {
        try
        {
            auto seq = std::make_shared<EdgeLighting::Sequence>();
            seq->SetLoop(loop != 0);
            auto *handle = new el_modulator_handle_impl{std::move(seq)};
            LOG_I("mod=%p, loop=%d", (void *)handle, loop);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_sequence_append(el_modulator_handle_t seq,
                                             el_modulator_handle_t stage, float duration)
    {
        LOG_I("seq=%p, stage=%p, duration=%f", (void *)seq, (void *)stage, duration);
        VALIDATE_MOD_PTR(seq, "el_modulator_sequence_append");
        VALIDATE_MOD_PTR(stage, "el_modulator_sequence_append");
        auto *s = dynamic_cast<EdgeLighting::Sequence *>(seq->ptr.get());
        if (!s)
        {
            LOG_E("el_modulator_sequence_append: seq is not a Sequence");
            return EL_ERROR_INVALID_PARAMETER;
        }
        s->Append(stage->ptr, duration);
        return EL_SUCCESS;
    }

    el_modulator_handle_t el_modulator_create_multiplier(el_modulator_handle_t a,
                                                         el_modulator_handle_t b)
    {
        if (!a || !b)
        {
            LOG_E("el_modulator_create_multiplier: null arg");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Multiplier>(a->ptr, b->ptr)};
            LOG_I("mod=%p, a=%p, b=%p", (void *)handle, (void *)a, (void *)b);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_adder(el_modulator_handle_t a,
                                                    el_modulator_handle_t b)
    {
        if (!a || !b)
        {
            LOG_E("el_modulator_create_adder: null arg");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Adder>(a->ptr, b->ptr)};
            LOG_I("mod=%p, a=%p, b=%p", (void *)handle, (void *)a, (void *)b);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_modulator_handle_t el_modulator_create_remap(el_modulator_handle_t inner,
                                                    float outMin, float outMax)
    {
        if (!inner)
        {
            LOG_E("el_modulator_create_remap: inner is null");
            return nullptr;
        }
        try
        {
            auto *handle = new el_modulator_handle_impl{std::make_shared<EdgeLighting::Remap>(inner->ptr, outMin, outMax)};
            LOG_I("mod=%p, inner=%p, outMin=%f, outMax=%f", (void *)handle, (void *)inner, outMin, outMax);
            return handle;
        }
        catch (const std::exception &e)
        {
            LOG_E("exception: %s", e.what());
            return nullptr;
        }
    }

    el_result_e el_modulator_destroy(el_modulator_handle_t mod)
    {
        LOG_I("mod=%p", (void *)mod);
        if (!mod)
        {
            return EL_SUCCESS;
        }
        delete mod;
        return EL_SUCCESS;
    }

    el_result_e el_modulator_evaluate(el_modulator_handle_t mod, float time, float *outValue)
    {
        VALIDATE_MOD_PTR(mod, "el_modulator_evaluate");
        VALIDATE_OUT_PTR(outValue, "el_modulator_evaluate");
        *outValue = mod->ptr ? mod->ptr->Evaluate(time) : 0.0f;
        LOG_D("mod=%p, time=%f, value=%f", (void *)mod, time, *outValue);
        return EL_SUCCESS;
    }

} // extern "C"
