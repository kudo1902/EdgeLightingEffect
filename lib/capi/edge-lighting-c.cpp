/**
 * @file edge-lighting-c.cpp
 * @brief Implementation of the flat C ABI declared in edge-lighting-c.h.
 *
 * Each entry point: validates pointers, converts EL_* PODs to/from the C++
 * Config, runs the C++ call inside a try/catch so no exception escapes the
 * ABI, and records any message for el_last_error.
 */
#include "edge-lighting-c.h"

#include "gl/gl-header.h" // gladLoad* + GL symbols
#include "core/edge-lighting.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/neon-multi-pass-renderer.h"
#include "renderer/neon-optimized-renderer.h"
#include "animation/neon-animations.h"
#include "animation/modulator.h"

#include <algorithm>
#include <memory>
#include <string>

namespace
{
    /// Per-thread last-error string. Empty until something fails.
    thread_local std::string gLastError;

    /// Set true once el_load_gl succeeds. Guards el_effect_initialize.
    bool gGlLoaded = false;

    void setError(const char *msg) { gLastError = msg ? msg : ""; }

    EdgeLighting::EasingFunction::Curve toEasing(int32_t e)
    {
        using namespace EdgeLighting;
        switch (static_cast<EL_Easing>(e))
        {
        case EL_EASE_LINEAR:
        {
            return EasingFunction::Linear;
        }
        case EL_EASE_IN_QUAD:
        {
            return EasingFunction::InQuad;
        }
        case EL_EASE_OUT_QUAD:
        {
            return EasingFunction::OutQuad;
        }
        case EL_EASE_INOUT_QUAD:
        {
            return EasingFunction::InOutQuad;
        }
        case EL_EASE_IN_CUBIC:
        {
            return EasingFunction::InCubic;
        }
        case EL_EASE_OUT_CUBIC:
        {
            return EasingFunction::OutCubic;
        }
        case EL_EASE_INOUT_CUBIC:
        {
            return EasingFunction::InOutCubic;
        }
        case EL_EASE_IN_SINE:
        {
            return EasingFunction::InSine;
        }
        case EL_EASE_OUT_SINE:
        {
            return EasingFunction::OutSine;
        }
        case EL_EASE_INOUT_SINE:
        {
            return EasingFunction::InOutSine;
        }
        case EL_EASE_IN_EXPO:
        {
            return EasingFunction::InExpo;
        }
        case EL_EASE_OUT_EXPO:
        {
            return EasingFunction::OutExpo;
        }
        case EL_EASE_INOUT_EXPO:
        {
            return EasingFunction::InOutExpo;
        }
        default:
        {
            return EasingFunction::Linear;
        }
        }
    }

    // ----------------------------------------------------------------------
    // EL_Config  <->  EdgeLighting::Config conversion
    // ----------------------------------------------------------------------

    void copyStopsToC(const std::vector<EdgeLighting::ColorStop> &src, EL_ColorStop *dst, int32_t &count)
    {
        count = static_cast<int32_t>(std::min<size_t>(src.size(), EL_MAX_COLOR_STOPS));
        for (int32_t i = 0; i < count; ++i)
        {
            dst[i].position = src[i].position;
            dst[i].r = src[i].color.r;
            dst[i].g = src[i].color.g;
            dst[i].b = src[i].color.b;
            dst[i].a = src[i].color.a;
        }
    }

    std::vector<EdgeLighting::ColorStop> stopsFromC(const EL_ColorStop *src, int32_t count)
    {
        std::vector<EdgeLighting::ColorStop> out;
        int32_t n = std::max(0, std::min(count, EL_MAX_COLOR_STOPS));
        out.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            out.push_back({src[i].position, glm::vec4(src[i].r, src[i].g, src[i].b, src[i].a)});
        }
        return out;
    }

    void copySegmentsToC(const std::vector<EdgeLighting::LitSegment> &src, EL_LitSegment *dst, int32_t &count)
    {
        count = static_cast<int32_t>(std::min<size_t>(src.size(), EL_MAX_SEGMENTS));
        for (int32_t i = 0; i < count; ++i)
        {
            dst[i].position = src[i].position;
            dst[i].length = src[i].length;
            dst[i].brightness = src[i].brightness;
        }
    }

    std::vector<EdgeLighting::LitSegment> segmentsFromC(const EL_LitSegment *src, int32_t count)
    {
        std::vector<EdgeLighting::LitSegment> out;
        int32_t n = std::max(0, std::min(count, EL_MAX_SEGMENTS));
        out.reserve(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i)
        {
            out.push_back({src[i].position, src[i].length, src[i].brightness});
        }
        return out;
    }

    EdgeLighting::Config toConfig(const EL_Config &c)
    {
        using namespace EdgeLighting;
        Config out;

        out.geometry.width = c.geometry.width;
        out.geometry.height = c.geometry.height;
        out.geometry.position = glm::vec2(c.geometry.posX, c.geometry.posY);
        out.geometry.cornerRadius = c.geometry.cornerRadius;
        out.geometry.winding = static_cast<Winding>(c.geometry.winding);

        out.neon.enable = c.neon.enable != 0;
        out.neon.lineWidth = c.neon.lineWidth;
        out.neon.intensity = c.neon.intensity;
        out.neon.glowRadius = c.neon.glowRadius;
        out.neon.bloomStrength = c.neon.bloomStrength;
        out.neon.glowSide = static_cast<GlowSide>(c.neon.glowSide);
        out.neon.glowSideSoftness = c.neon.glowSideSoftness;
        out.neon.segments = segmentsFromC(c.neon.segments, c.neon.segmentCount);

        out.multipassNeon.enable = c.multipassNeon.enable != 0;
        out.multipassNeon.showPerimeterGradient = c.multipassNeon.showPerimeterGradient != 0;
        out.multipassNeon.showFullGradient = c.multipassNeon.showFullGradient != 0;
        out.multipassNeon.lineWidth = c.multipassNeon.lineWidth;
        out.multipassNeon.intensity = c.multipassNeon.intensity;
        out.multipassNeon.glowRadius = c.multipassNeon.glowRadius;
        out.multipassNeon.bloomStrength = c.multipassNeon.bloomStrength;
        out.multipassNeon.glowSide = static_cast<GlowSide>(c.multipassNeon.glowSide);
        out.multipassNeon.glowSideSoftness = c.multipassNeon.glowSideSoftness;
        out.multipassNeon.blendSpace = static_cast<BlendSpace>(c.multipassNeon.blendSpace);
        out.multipassNeon.colorStops = stopsFromC(c.multipassNeon.colorStops, c.multipassNeon.colorStopCount);
        out.multipassNeon.hueRotationRate = c.multipassNeon.hueRotationRate;

        out.optimizedNeon.enable = c.optimizedNeon.enable != 0;
        out.optimizedNeon.resolutionScale = c.optimizedNeon.resolutionScale;
        out.optimizedNeon.numSamples = c.optimizedNeon.numSamples;
        out.optimizedNeon.showHalfRes = c.optimizedNeon.showHalfRes != 0;

        out.wireframe.enable = c.wireframe.enable != 0;
        out.wireframe.color = glm::vec4(c.wireframe.r, c.wireframe.g, c.wireframe.b, c.wireframe.a);

        return out;
    }

    void fromConfig(const EdgeLighting::Config &c, EL_Config &out)
    {
        out.geometry.width = c.geometry.width;
        out.geometry.height = c.geometry.height;
        out.geometry.posX = c.geometry.position.x;
        out.geometry.posY = c.geometry.position.y;
        out.geometry.cornerRadius = c.geometry.cornerRadius;
        out.geometry.winding = static_cast<int32_t>(c.geometry.winding);

        out.neon.enable = c.neon.enable ? 1 : 0;
        out.neon.lineWidth = c.neon.lineWidth;
        out.neon.intensity = c.neon.intensity;
        out.neon.glowRadius = c.neon.glowRadius;
        out.neon.bloomStrength = c.neon.bloomStrength;
        out.neon.glowSide = static_cast<int32_t>(c.neon.glowSide);
        out.neon.glowSideSoftness = c.neon.glowSideSoftness;
        copySegmentsToC(c.neon.segments, out.neon.segments, out.neon.segmentCount);

        out.multipassNeon.enable = c.multipassNeon.enable ? 1 : 0;
        out.multipassNeon.showPerimeterGradient = c.multipassNeon.showPerimeterGradient ? 1 : 0;
        out.multipassNeon.showFullGradient = c.multipassNeon.showFullGradient ? 1 : 0;
        out.multipassNeon.lineWidth = c.multipassNeon.lineWidth;
        out.multipassNeon.intensity = c.multipassNeon.intensity;
        out.multipassNeon.glowRadius = c.multipassNeon.glowRadius;
        out.multipassNeon.bloomStrength = c.multipassNeon.bloomStrength;
        out.multipassNeon.glowSide = static_cast<int32_t>(c.multipassNeon.glowSide);
        out.multipassNeon.glowSideSoftness = c.multipassNeon.glowSideSoftness;
        out.multipassNeon.blendSpace = static_cast<int32_t>(c.multipassNeon.blendSpace);
        copyStopsToC(c.multipassNeon.colorStops, out.multipassNeon.colorStops, out.multipassNeon.colorStopCount);
        out.multipassNeon.hueRotationRate = c.multipassNeon.hueRotationRate;

        out.optimizedNeon.enable = c.optimizedNeon.enable ? 1 : 0;
        out.optimizedNeon.resolutionScale = c.optimizedNeon.resolutionScale;
        out.optimizedNeon.numSamples = c.optimizedNeon.numSamples;
        out.optimizedNeon.showHalfRes = c.optimizedNeon.showHalfRes ? 1 : 0;

        out.wireframe.enable = c.wireframe.enable ? 1 : 0;
        out.wireframe.r = c.wireframe.color.r;
        out.wireframe.g = c.wireframe.color.g;
        out.wireframe.b = c.wireframe.color.b;
        out.wireframe.a = c.wireframe.color.a;
    }
} // namespace

// ==========================================================================
// Opaque handle definitions
// ==========================================================================
struct EL_Effect
{
    EdgeLighting::EdgeLightingEffect effect;
};

struct EL_Animation
{
    EdgeLighting::AnimationPtr ptr;
};

extern "C"
{

    // ----------------------------------------------------------------------
    // Library-level
    // ----------------------------------------------------------------------
    uint32_t el_abi_version(void) { return EL_ABI_VERSION; }

    const char *el_last_error(void) { return gLastError.c_str(); }

    EL_Result el_load_gl(EL_GLGetProcAddress getProc)
    {
        if (!getProc)
        {
            setError("el_load_gl: getProc is null");
            return EL_ERR_NULL_ARG;
        }
#if defined(PLATFORM_MACOS)
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(getProc)))
        {
            setError("el_load_gl: gladLoadGLLoader failed (no current context?)");
            return EL_ERR_GL_NOT_LOADED;
        }
#elif defined(PLATFORM_WINDOWS)
        if (!gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(getProc)))
        {
            setError("el_load_gl: gladLoadGLES2Loader failed (no current context?)");
            return EL_ERR_GL_NOT_LOADED;
        }
#endif
        // Linux uses native GLES symbols linked at build time — nothing to load.
        gGlLoaded = true;
        setError("");
        return EL_OK;
    }

    EL_Result el_config_default(EL_Config *outCfg)
    {
        if (!outCfg)
        {
            setError("el_config_default: outCfg is null");
            return EL_ERR_NULL_ARG;
        }
        fromConfig(EdgeLighting::Config{}, *outCfg);
        return EL_OK;
    }

    // ----------------------------------------------------------------------
    // Effect lifecycle
    // ----------------------------------------------------------------------
    EL_Effect *el_effect_create(void)
    {
        try
        {
            auto *fx = new EL_Effect();
            // Register all renderers, mirroring demo/src/main.cpp.
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::WireframeRenderer>());
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::NeonRenderer>());
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::NeonMultiPassRenderer>());
            fx->effect.AddRenderer(std::make_shared<EdgeLighting::NeonOptimizedRenderer>());
            return fx;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return nullptr;
        }
    }

    void el_effect_destroy(EL_Effect *fx) { delete fx; }

    EL_Result el_effect_initialize(EL_Effect *fx)
    {
        if (!fx)
        {
            setError("el_effect_initialize: fx is null");
            return EL_ERR_NULL_ARG;
        }
        if (!gGlLoaded)
        {
            setError("el_effect_initialize: call el_load_gl first");
            return EL_ERR_GL_NOT_LOADED;
        }
        try
        {
            if (!fx->effect.Initialize())
            {
                setError("el_effect_initialize: renderer initialisation failed");
                return EL_ERR_INIT_FAILED;
            }
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    EL_Result el_effect_set_config(EL_Effect *fx, const EL_Config *cfg)
    {
        if (!fx || !cfg)
        {
            setError("el_effect_set_config: null argument");
            return EL_ERR_NULL_ARG;
        }
        try
        {
            fx->effect.SetConfig(toConfig(*cfg));
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    EL_Result el_effect_get_config(EL_Effect *fx, EL_Config *outCfg)
    {
        if (!fx || !outCfg)
        {
            setError("el_effect_get_config: null argument");
            return EL_ERR_NULL_ARG;
        }
        fromConfig(fx->effect.GetConfig(), *outCfg);
        return EL_OK;
    }

    EL_Result el_effect_update(EL_Effect *fx, float deltaTime)
    {
        if (!fx)
        {
            setError("el_effect_update: fx is null");
            return EL_ERR_NULL_ARG;
        }
        try
        {
            fx->effect.Update(deltaTime);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    EL_Result el_effect_render(EL_Effect *fx, int32_t viewportWidth, int32_t viewportHeight)
    {
        if (!fx)
        {
            setError("el_effect_render: fx is null");
            return EL_ERR_NULL_ARG;
        }
        try
        {
            fx->effect.Render(viewportWidth, viewportHeight);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    // --- Per-renderer enable/disable ---
    //
    // Implemented as syntactic sugar over GetConfig/SetConfig: the underlying
    // truth lives in the `enable` flag on each sub-config. SetConfig fires
    // OnConfigChanged on every renderer (LUT rebuilds etc.), which is heavier
    // than strictly necessary for a flag toggle but acceptable for an explicit
    // user action that isn't on the per-frame hot path.

    EL_Result el_effect_set_renderer_enabled(EL_Effect *fx, EL_RendererKind kind, EL_Bool enabled)
    {
        if (!fx)
        {
            setError("el_effect_set_renderer_enabled: fx is null");
            return EL_ERR_NULL_ARG;
        }
        try
        {
            EdgeLighting::Config cfg = fx->effect.GetConfig();
            bool b = enabled != 0;
            switch (kind)
            {
            case EL_RENDERER_WIREFRAME:
            {
                cfg.wireframe.enable = b;
                break;
            }
            case EL_RENDERER_NEON:
            {
                cfg.neon.enable = b;
                break;
            }
            case EL_RENDERER_MULTIPASS_NEON:
            {
                cfg.multipassNeon.enable = b;
                break;
            }
            case EL_RENDERER_OPTIMIZED_NEON:
            {
                cfg.optimizedNeon.enable = b;
                break;
            }
            default:
            {
                setError("el_effect_set_renderer_enabled: unknown EL_RendererKind");
                return EL_ERR_NULL_ARG;
            }
            }
            fx->effect.SetConfig(cfg);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    EL_Bool el_effect_is_renderer_enabled(EL_Effect *fx, EL_RendererKind kind)
    {
        if (!fx)
        {
            return 0;
        }
        const EdgeLighting::Config &cfg = fx->effect.GetConfig();
        switch (kind)
        {
        case EL_RENDERER_WIREFRAME:
        {
            return cfg.wireframe.enable ? 1 : 0;
        }
        case EL_RENDERER_NEON:
        {
            return cfg.neon.enable ? 1 : 0;
        }
        case EL_RENDERER_MULTIPASS_NEON:
        {
            return cfg.multipassNeon.enable ? 1 : 0;
        }
        case EL_RENDERER_OPTIMIZED_NEON:
        {
            return cfg.optimizedNeon.enable ? 1 : 0;
        }
        default:
        {
            return 0;
        }
        }
    }

    // --- Clock ---
    void el_effect_play(EL_Effect *fx)
    {
        if (fx)
        {
            fx->effect.GetClock().Play();
        }
    }
    void el_effect_pause(EL_Effect *fx)
    {
        if (fx)
        {
            fx->effect.GetClock().Pause();
        }
    }
    void el_effect_stop(EL_Effect *fx)
    {
        if (fx)
        {
            fx->effect.GetClock().Stop();
        }
    }
    void el_effect_reset(EL_Effect *fx)
    {
        if (fx)
        {
            fx->effect.GetClock().Reset();
        }
    }
    void el_effect_set_time(EL_Effect *fx, float time)
    {
        if (fx)
        {
            fx->effect.GetClock().SetTime(time);
        }
    }
    float el_effect_get_time(EL_Effect *fx)
    {
        return fx ? fx->effect.GetClock().GetTime() : 0.0f;
    }
    EL_Bool el_effect_is_playing(EL_Effect *fx)
    {
        return (fx && fx->effect.GetClock().IsPlaying()) ? 1 : 0;
    }

    // ----------------------------------------------------------------------
    // Animations
    // ----------------------------------------------------------------------
    EL_Animation *el_animation_create(EL_AnimationPreset preset)
    {
        using namespace EdgeLighting;
        AnimationPtr a;
        switch (preset)
        {
        case EL_ANIM_NONE:
        {
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
        default:
        {
            setError("el_animation_create: unknown preset");
            return nullptr;
        }
        }
        return new EL_Animation{std::move(a)};
    }

    EL_Animation *el_animation_intensity_pulse(float duration, float min, float max)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::IntensityPulse>(duration, min, max)};
    }
    EL_Animation *el_animation_intensity_strobe(float duration, float offIntensity, float onIntensity)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::IntensityStrobe>(duration, offIntensity, onIntensity)};
    }
    EL_Animation *el_animation_intensity_fade_in(float target, float duration, int32_t easing)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::IntensityFadeIn>(target, duration, toEasing(easing))};
    }
    EL_Animation *el_animation_intensity_fade_out(float start, float duration, int32_t easing)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::IntensityFadeOut>(start, duration, toEasing(easing))};
    }
    EL_Animation *el_animation_glow_radius_breath(float duration, float minRadius, float maxRadius)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::GlowRadiusBreath>(duration, minRadius, maxRadius)};
    }
    EL_Animation *el_animation_bloom_pulse(float duration, float min, float max)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::BloomPulse>(duration, min, max)};
    }
    EL_Animation *el_animation_segment_travel(float duration, float length, float boost)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::SegmentTravel>(duration, length, boost)};
    }
    EL_Animation *el_animation_segment_bounce(float duration, float length, float boost)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::SegmentBounce>(duration, length, boost)};
    }
    EL_Animation *el_animation_outline_tracer(float duration, int32_t easing)
    {
        return new EL_Animation{std::make_shared<EdgeLighting::OutlineTracer>(duration, toEasing(easing))};
    }

    void el_animation_destroy(EL_Animation *anim) { delete anim; }

    EL_Result el_animation_apply(EL_Animation *anim, EL_Config *cfg, float elapsed)
    {
        if (!anim || !cfg)
        {
            setError("el_animation_apply: null argument");
            return EL_ERR_NULL_ARG;
        }
        if (!anim->ptr)
        {
            return EL_OK; // No-op animation (e.g. NONE).
        }
        try
        {
            EdgeLighting::Config c = toConfig(*cfg);
            anim->ptr->Apply(c, elapsed);
            fromConfig(c, *cfg);
            return EL_OK;
        }
        catch (const std::exception &e)
        {
            setError(e.what());
            return EL_ERR_EXCEPTION;
        }
    }

    EL_Bool el_animation_is_complete(EL_Animation *anim, float elapsed)
    {
        return (anim && anim->ptr && anim->ptr->IsComplete(elapsed)) ? 1 : 0;
    }

    EL_PlaybackMode el_animation_get_playback_mode(EL_Animation *anim)
    {
        if (!anim || !anim->ptr)
        {
            return EL_PLAYBACK_LOOP;
        }
        return anim->ptr->GetPlaybackMode() == EdgeLighting::PlaybackMode::ONE_SHOT
                   ? EL_PLAYBACK_ONE_SHOT
                   : EL_PLAYBACK_LOOP;
    }

    void el_animation_set_playback_mode(EL_Animation *anim, EL_PlaybackMode mode)
    {
        if (anim && anim->ptr)
        {
            anim->ptr->SetPlaybackMode(mode == EL_PLAYBACK_ONE_SHOT
                                          ? EdgeLighting::PlaybackMode::ONE_SHOT
                                          : EdgeLighting::PlaybackMode::LOOP);
        }
    }

    float el_animation_get_duration(EL_Animation *anim)
    {
        return (anim && anim->ptr) ? anim->ptr->GetDuration() : 0.0f;
    }

    void el_animation_set_duration(EL_Animation *anim, float seconds)
    {
        if (anim && anim->ptr)
        {
            anim->ptr->SetDuration(seconds);
        }
    }

    float el_animation_get_speed(EL_Animation *anim)
    {
        return (anim && anim->ptr) ? anim->ptr->GetSpeed() : 1.0f;
    }

    void el_animation_set_speed(EL_Animation *anim, float speed)
    {
        if (anim && anim->ptr)
        {
            anim->ptr->SetSpeed(speed);
        }
    }

} // extern "C"
