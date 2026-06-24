/**
 * @file edge-lighting-c.h
 * @brief Flat C ABI over the EdgeLighting C++ renderer for FFI / C# interop.
 *
 * Design notes
 * ------------
 * - The naming here is deliberately `el_snake_case` (C convention) rather than
 *   the project's PascalCase, because this is a C ABI consumed by P/Invoke and
 *   other FFIs that expect C-style symbols.
 * - This library is **context-less**: the host (e.g. a C# app using OpenTK /
 *   Silk.NET) owns the window and GL context. Call @ref el_load_gl once with
 *   the host's GL proc loader, make the context current, then drive the effect.
 * - All structs are plain-old-data with 4-byte fields only (no padding), so a
 *   matching `[StructLayout(LayoutKind.Sequential)]` struct on the managed side
 *   marshals with zero copies.
 * - No C++ exception ever crosses this boundary; every fallible call returns an
 *   @ref EL_Result and stashes a human-readable message in @ref el_last_error.
 *
 * Lifecycle
 * ---------
 * @code
 *   el_load_gl(getProcAddr);              // once per process, after ctx current
 *   EL_Effect* fx = el_effect_create();   // registers all renderers
 *   el_effect_initialize(fx);             // compiles shaders (needs current ctx)
 *   EL_Config cfg; el_config_default(&cfg);
 *   cfg.neon.enable = 1;
 *   el_effect_set_config(fx, &cfg);
 *   // per frame:
 *   el_effect_update(fx, dt);
 *   el_effect_render(fx, width, height);
 *   // teardown:
 *   el_effect_destroy(fx);
 * @endcode
 */
#ifndef EDGE_LIGHTING_C_H
#define EDGE_LIGHTING_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* --------------------------------------------------------------------------
 * Export / visibility
 * ------------------------------------------------------------------------ */
#if defined(_WIN32)
#define EL_API __declspec(dllexport)
#else
#define EL_API __attribute__((visibility("default")))
#endif

/** ABI version. Bump on any breaking change to a struct layout or signature. */
#define EL_ABI_VERSION 1

/** Maximum colour stops per gradient (mirrors NeonConfig::MAX_COLOR_STOPS). */
#define EL_MAX_COLOR_STOPS 16

/** 32-bit boolean (0 = false, non-zero = true) for unambiguous marshalling. */
typedef int32_t EL_Bool;

/* --------------------------------------------------------------------------
 * Result codes
 * ------------------------------------------------------------------------ */
typedef enum EL_Result
{
    EL_OK = 0,                ///< Success.
    EL_ERR_NULL_ARG = 1,      ///< A required pointer argument was null.
    EL_ERR_GL_NOT_LOADED = 2, ///< el_load_gl was not called (or failed).
    EL_ERR_INIT_FAILED = 3,   ///< Renderer initialisation failed (shader compile/link).
    EL_ERR_EXCEPTION = 4      ///< A C++ exception was caught; see el_last_error.
} EL_Result;

/* --------------------------------------------------------------------------
 * Enums (mirror EdgeLighting::*)
 * ------------------------------------------------------------------------ */
typedef enum EL_Winding
{
    EL_WINDING_CLOCKWISE = 0,
    EL_WINDING_COUNTER_CLOCKWISE = 1
} EL_Winding;

typedef enum EL_GlowSide
{
    EL_GLOW_SIDE_BOTH = 0,
    EL_GLOW_SIDE_INSIDE = 1,
    EL_GLOW_SIDE_OUTSIDE = 2
} EL_GlowSide;

typedef enum EL_BlendSpace
{
    EL_BLEND_SPACE_RGB = 0,
    EL_BLEND_SPACE_HSV = 1
} EL_BlendSpace;

/** Identifies one of the built-in renderers for enable/disable toggling.
 *  Values match the registration order in @ref el_effect_create. */
typedef enum EL_RendererKind
{
    EL_RENDERER_WIREFRAME = 0,      ///< 1px line-loop debug box.
    EL_RENDERER_NEON = 1,           ///< Single-pass neon stroke.
    EL_RENDERER_MULTIPASS_NEON = 2, ///< Multi-pass (FBO + separable blur) neon.
    EL_RENDERER_OPTIMIZED_NEON = 3  ///< Half-resolution neon for edge devices.
} EL_RendererKind;

/** Playback mode of an animation (mirror EdgeLighting::PlaybackMode). */
typedef enum EL_PlaybackMode
{
    EL_PLAYBACK_LOOP = 0,    ///< Plays forever; never completes.
    EL_PLAYBACK_ONE_SHOT = 1 ///< Plays for the configured duration, then stops.
} EL_PlaybackMode;

/** Easing curves (mirror EdgeLighting::EasingFunction::*). */
typedef enum EL_Easing
{
    EL_EASE_LINEAR = 0,
    EL_EASE_IN_QUAD = 1,
    EL_EASE_OUT_QUAD = 2,
    EL_EASE_INOUT_QUAD = 3,
    EL_EASE_IN_CUBIC = 4,
    EL_EASE_OUT_CUBIC = 5,
    EL_EASE_INOUT_CUBIC = 6,
    EL_EASE_IN_SINE = 7,
    EL_EASE_OUT_SINE = 8,
    EL_EASE_INOUT_SINE = 9,
    EL_EASE_IN_EXPO = 10,
    EL_EASE_OUT_EXPO = 11,
    EL_EASE_INOUT_EXPO = 12
} EL_Easing;

/* --------------------------------------------------------------------------
 * Config structs (flat POD mirrors of EdgeLighting::Config)
 * ------------------------------------------------------------------------ */

/** One colour stop along the perimeter. RGBA in [0,1]. */
typedef struct EL_ColorStop
{
    float position; ///< Normalised position along the perimeter [0,1].
    float r;
    float g;
    float b;
    float a;
} EL_ColorStop;

/** Geometry of the target rounded rectangle. */
typedef struct EL_RectGeometry
{
    float width;
    float height;
    float posX;         ///< Top-left X in viewport coordinates.
    float posY;         ///< Top-left Y in viewport coordinates.
    float cornerRadius; ///< Corner radius in pixels (0 = sharp).
    int32_t winding;    ///< EL_Winding.
} EL_RectGeometry;

/** Single-pass neon renderer settings. */
typedef struct EL_NeonConfig
{
    EL_Bool enable;
    float lineWidth;
    float intensity;
    float glowRadius;
    float bloomStrength;
    int32_t glowSide; ///< EL_GlowSide.
    float glowSideSoftness;
    int32_t blendSpace;     ///< EL_BlendSpace.
    int32_t colorStopCount; ///< Number of valid entries in colorStops.
    EL_ColorStop colorStops[EL_MAX_COLOR_STOPS];
    float hueRotationRate;
    float segmentPosition;
    float segmentLength;
    float segmentBoost;
    float arcStart;
    float arcLength;
} EL_NeonConfig;

/** Multi-pass (gradient-to-FBO + separable blur) neon renderer settings. */
typedef struct EL_MultiPassNeonConfig
{
    EL_Bool enable;
    EL_Bool showPerimeterGradient; ///< Debug: show baked perimeter gradient FBO.
    EL_Bool showFullGradient;      ///< Debug: show full-screen gradient FBO.
    float lineWidth;
    float intensity;
    float glowRadius;
    float bloomStrength;
    int32_t glowSide; ///< EL_GlowSide.
    float glowSideSoftness;
    int32_t blendSpace;     ///< EL_BlendSpace.
    int32_t colorStopCount; ///< Number of valid entries in colorStops.
    EL_ColorStop colorStops[EL_MAX_COLOR_STOPS];
    float hueRotationRate;
} EL_MultiPassNeonConfig;

/** Half-resolution optimised neon renderer settings (shares NeonConfig visuals). */
typedef struct EL_OptimizedNeonConfig
{
    EL_Bool enable;
    float resolutionScale;  ///< Internal FBO scale (0.5 = half res).
    int32_t numSamples;     ///< Gather samples per fragment (max 64).
    int32_t gradientLutSize; ///< Power-of-two LUT size, 32..256.
    EL_Bool showHalfRes;    ///< Debug: show raw half-res FBO (nearest upscale).
} EL_OptimizedNeonConfig;

/** Wireframe debug overlay settings. */
typedef struct EL_WireframeConfig
{
    EL_Bool enable;
    float r;
    float g;
    float b;
    float a;
} EL_WireframeConfig;

/** Top-level configuration: one sub-config per renderer. */
typedef struct EL_Config
{
    EL_RectGeometry geometry;
    EL_NeonConfig neon;
    EL_MultiPassNeonConfig multipassNeon;
    EL_OptimizedNeonConfig optimizedNeon;
    EL_WireframeConfig wireframe;
} EL_Config;

/* --------------------------------------------------------------------------
 * Opaque handles
 * ------------------------------------------------------------------------ */
typedef struct EL_Effect EL_Effect;
typedef struct EL_Animation EL_Animation;

/** GL function loader (e.g. glfwGetProcAddress / SDL_GL_GetProcAddress). */
typedef void *(*EL_GLGetProcAddress)(const char *name);

/* --------------------------------------------------------------------------
 * Library-level
 * ------------------------------------------------------------------------ */

/** ABI version this binary was built with (compare against EL_ABI_VERSION). */
EL_API uint32_t el_abi_version(void);

/**
 * @brief Bootstrap the library's GL function pointers.
 * @param getProc Host proc loader. Must be valid with a current GL context.
 * @return EL_OK, or EL_ERR_GL_NOT_LOADED if the loader could not resolve GL.
 * @note Call once per process after making a GL context current, before any
 *       @ref el_effect_initialize. On Linux (native GLES) this is a no-op that
 *       only records that GL is ready.
 */
EL_API EL_Result el_load_gl(EL_GLGetProcAddress getProc);

/**
 * @brief Last error message for the current thread.
 * @return Pointer to a thread-local, null-terminated string. Valid until the
 *         next failing call on the same thread. Never null.
 */
EL_API const char *el_last_error(void);

/** @brief Populate @p outCfg with the library's default configuration. */
EL_API EL_Result el_config_default(EL_Config *outCfg);

/* --------------------------------------------------------------------------
 * Effect lifecycle
 * ------------------------------------------------------------------------ */

/** @brief Create an effect with all renderers registered (none initialised yet). */
EL_API EL_Effect *el_effect_create(void);

/** @brief Destroy an effect. Safe to pass null. */
EL_API void el_effect_destroy(EL_Effect *fx);

/**
 * @brief Initialise all renderers (compiles shaders, builds geometry).
 * @note Requires @ref el_load_gl and a current GL context on this thread.
 */
EL_API EL_Result el_effect_initialize(EL_Effect *fx);

/** @brief Replace the active configuration and notify all renderers. */
EL_API EL_Result el_effect_set_config(EL_Effect *fx, const EL_Config *cfg);

/** @brief Copy the active configuration into @p outCfg. */
EL_API EL_Result el_effect_get_config(EL_Effect *fx, EL_Config *outCfg);

/** @brief Advance animation time by @p deltaTime seconds. */
EL_API EL_Result el_effect_update(EL_Effect *fx, float deltaTime);

/** @brief Render all enabled renderers into the current framebuffer. */
EL_API EL_Result el_effect_render(EL_Effect *fx, int32_t viewportWidth, int32_t viewportHeight);

/* --- Per-renderer enable/disable (convenience over get_config + set_config) --- */

/** @brief Enable or disable one of the built-in renderers. */
EL_API EL_Result el_effect_set_renderer_enabled(EL_Effect *fx, EL_RendererKind kind, EL_Bool enabled);

/** @brief True if the given renderer is currently enabled. */
EL_API EL_Bool el_effect_is_renderer_enabled(EL_Effect *fx, EL_RendererKind kind);

/* --- Clock --- */
EL_API void el_effect_play(EL_Effect *fx);
EL_API void el_effect_pause(EL_Effect *fx);
EL_API void el_effect_stop(EL_Effect *fx);
EL_API void el_effect_reset(EL_Effect *fx);
EL_API void el_effect_set_time(EL_Effect *fx, float time);
EL_API float el_effect_get_time(EL_Effect *fx);
EL_API EL_Bool el_effect_is_playing(EL_Effect *fx);

/* --------------------------------------------------------------------------
 * Neon animations
 * ------------------------------------------------------------------------ */

/** Ready-made animation presets (mirror the demo's AnimationPreset showcase). */
typedef enum EL_AnimationPreset
{
    EL_ANIM_NONE = 0,
    EL_ANIM_BREATHING = 1,
    EL_ANIM_STROBE = 2,
    EL_ANIM_HEARTBEAT = 3,
    EL_ANIM_SHIMMER = 4,
    EL_ANIM_AURORA = 5,
    EL_ANIM_REVERSE_SWEEP = 6,
    EL_ANIM_FADE_IN = 7,
    EL_ANIM_SEGMENT_TRAVEL = 8,
    EL_ANIM_SEGMENT_BOUNCE = 9,
    EL_ANIM_COMET = 10,
    EL_ANIM_OUTLINE_TRACER = 11,
    EL_ANIM_FADE_OUT = 12,
    EL_ANIM_HUE_REVERSE = 13
} EL_AnimationPreset;

/**
 * @brief Build a ready-made preset animation. Returns null for EL_ANIM_NONE.
 * @note Destroy with @ref el_animation_destroy.
 */
EL_API EL_Animation *el_animation_create(EL_AnimationPreset preset);

/* --- Parametric factories (mirror animation/neon-animations.h) --- */
/* For periodic animations, `duration` is seconds per cycle.                  */
EL_API EL_Animation *el_animation_intensity_pulse(float duration, float min, float max);
EL_API EL_Animation *el_animation_intensity_strobe(float duration, float offIntensity, float onIntensity);
EL_API EL_Animation *el_animation_intensity_fade_in(float target, float duration, int32_t easing /*EL_Easing*/);
EL_API EL_Animation *el_animation_intensity_fade_out(float start, float duration, int32_t easing /*EL_Easing*/);
EL_API EL_Animation *el_animation_glow_radius_breath(float duration, float minRadius, float maxRadius);
EL_API EL_Animation *el_animation_bloom_pulse(float duration, float min, float max);
EL_API EL_Animation *el_animation_hue_rotation_reverse(float baseRate, float duration);
EL_API EL_Animation *el_animation_hue_rotation_ease_reverse(float maxRate, float duration);
EL_API EL_Animation *el_animation_segment_travel(float duration, float length, float boost);
EL_API EL_Animation *el_animation_segment_bounce(float duration, float length, float boost);
EL_API EL_Animation *el_animation_outline_tracer(float duration, int32_t easing /*EL_Easing*/);

/** @brief Destroy an animation. Safe to pass null. */
EL_API void el_animation_destroy(EL_Animation *anim);

/**
 * @brief Apply the animation at @p elapsed seconds into @p cfg (in place).
 * @param elapsed Seconds since the animation began (not absolute clock time).
 * @details Typical use: pass `el_effect_get_time(fx) - startTime`, write the
 *          result back with @ref el_effect_set_config.
 */
EL_API EL_Result el_animation_apply(EL_Animation *anim, EL_Config *cfg, float elapsed);

/* --- Playback mode (loop vs one-shot) --- */

/** @brief Current playback mode. */
EL_API EL_PlaybackMode el_animation_get_playback_mode(EL_Animation *anim);

/** @brief Set the playback mode. Does NOT touch the duration. */
EL_API void el_animation_set_playback_mode(EL_Animation *anim, EL_PlaybackMode mode);

/* --- Duration (wall-clock seconds; consulted only in ONE_SHOT) --- */

/** @brief Configured wall-clock duration in seconds. */
EL_API float el_animation_get_duration(EL_Animation *anim);

/**
 * @brief Set the wall-clock duration in seconds. Does NOT touch the mode.
 * @details Subclasses with duration-dependent internal modulators (e.g. eased
 *          fades) rebuild them in lockstep so the visual matches the latch.
 */
EL_API void el_animation_set_duration(EL_Animation *anim, float seconds);

/** @brief True if mode is ONE_SHOT and @p elapsed has reached the duration. */
EL_API EL_Bool el_animation_is_complete(EL_Animation *anim, float elapsed);

/* --- Speed (playback-rate multiplier; 1.0 = normal) --- */

/** @brief Current playback rate multiplier. */
EL_API float el_animation_get_speed(EL_Animation *anim);

/** @brief Set the playback rate multiplier (1.0 = normal, 0.5 = half, 2.0 = double). */
EL_API void el_animation_set_speed(EL_Animation *anim, float speed);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // EDGE_LIGHTING_C_H
