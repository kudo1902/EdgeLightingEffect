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
#ifndef _EDGE_LIGHTING_C_H_
#define _EDGE_LIGHTING_C_H_

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

/** ABI version. Bump on any breaking change to a struct layout or signature.
 *  v10: animations became stateful. el_animation_apply lost its `elapsed`
 *       parameter; the animation carries its own state now. New lifecycle
 *       entry points (play/pause/stop/reset/update), state / end-action
 *       accessors, effect-level attach/detach (el_effect_attach_animation,
 *       el_effect_detach_animation, ...), and el_animation_capture_baseline
 *       for RESTORE. el_animation_is_complete(elapsed) removed - check
 *       el_animation_get_state instead. EL_ConfigField dropped its three
 *       NEON_SEGMENT_* values and renumbered NEON_ARC_START / _LENGTH;
 *       segment scalars now live in EL_SegmentField and bind via
 *       el_animation_add_segment_field so the segment index is required. */
#define EL_ABI_VERSION 10

/** Maximum colour stops per gradient - hard cap because the EL_NeonConfig
 *  struct's colorStops[] array is fixed-size at the ABI boundary. The core C++
 *  library imposes no cap; only the C surface does. */
#define EL_MAX_COLOR_STOPS 128

/** Maximum travelling segment boosts (mirrors MAX_SEGMENT_BOOSTS in neon-tuning.h). */
#define EL_MAX_SEGMENT_BOOSTS 8

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
        EL_BLEND_SPACE_HSV = 1,
        EL_BLEND_SPACE_HSL = 2
    } EL_BlendSpace;

    /** Identifies one of the built-in renderers for enable/disable toggling.
     *  Values match the registration order in @ref el_effect_create. */
    typedef enum EL_RendererKind
    {
        EL_RENDERER_WIREFRAME = 0,     ///< 1px line-loop debug box.
        EL_RENDERER_NEON = 1,          ///< Single-pass neon stroke.
        EL_RENDERER_OPTIMIZED_NEON = 2 ///< Half-resolution neon for edge devices.
    } EL_RendererKind;

    /** Playback mode of an animation (mirror EdgeLighting::PlaybackMode). */
    typedef enum EL_PlaybackMode
    {
        EL_PLAYBACK_LOOP = 0,    ///< Plays forever; never completes.
        EL_PLAYBACK_ONE_SHOT = 1 ///< Plays for the configured duration, then stops.
    } EL_PlaybackMode;

    /** Per-animation lifecycle state (mirror EdgeLighting::AnimationState). */
    typedef enum EL_AnimationState
    {
        EL_ANIM_STATE_STOPPED = 0, ///< Not advancing; Apply's behaviour depends on end action.
        EL_ANIM_STATE_PLAYING = 1, ///< Elapsed advances on Update; Apply writes current value.
        EL_ANIM_STATE_PAUSED = 2   ///< Elapsed frozen; Apply keeps writing the held value.
    } EL_AnimationState;

    /** What the animation writes into its field once it enters STOPPED
     *  (mirror EdgeLighting::EndAction). Only takes effect after the animation
     *  has actually run at least once; a never-played animation is a no-op
     *  regardless. To let the base config show through, detach the animation. */
    typedef enum EL_EndAction
    {
        EL_END_ACTION_HOLD_CURRENT = 0, ///< Settle at elapsed-at-stop. (Default.)
        EL_END_ACTION_HOLD_END = 1,     ///< Settle at ApplyAt(cfg, duration).
        EL_END_ACTION_HOLD_START = 2,   ///< Settle at ApplyAt(cfg, 0).
        EL_END_ACTION_RESTORE = 3       ///< Settle at pre-play snapshot (subclass-hooked; base is a no-op).
    } EL_EndAction;

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

    /** One travelling brightness peak. Several can be active at once; the shader
     *  sums their Gaussian bells per sample so peaks can overlap. */
    typedef struct EL_SegmentBoost
    {
        float position; ///< Centre in [0, 1) perimeter position.
        float length;   ///< Width as a fraction of the perimeter (~2σ).
        float boost;    ///< Peak brightness multiplier at the centre.
    } EL_SegmentBoost;

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
        EL_Bool opaque;
        float opaqueColorR; ///< Opaque-mode background fill colour, straight linear RGBA.
        float opaqueColorG;
        float opaqueColorB;
        float opaqueColorA; ///< Fill alpha; < 1 makes the surround a partial fill.
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
        int32_t segmentBoostCount; ///< Number of valid entries in segmentBoosts.
        EL_SegmentBoost segmentBoosts[EL_MAX_SEGMENT_BOOSTS];
        float arcStart;
        float arcLength;
    } EL_NeonConfig;

    /** Half-resolution optimised neon renderer settings (shares NeonConfig visuals). */
    typedef struct EL_OptimizedNeonConfig
    {
        EL_Bool enable;
        float resolutionScale;   ///< Internal FBO scale (0.5 = half res).
        int32_t numSamples;      ///< Gather samples per fragment (max 64).
        int32_t gradientLutSize; ///< Power-of-two LUT size, 32..256.
        EL_Bool showHalfRes;     ///< Debug: show raw half-res FBO (nearest upscale).
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
        EL_OptimizedNeonConfig optimizedNeon;
        EL_WireframeConfig wireframe;
    } EL_Config;

    /* --------------------------------------------------------------------------
     * Opaque handles
     * ------------------------------------------------------------------------ */
    typedef struct EL_Effect EL_Effect;
    typedef struct EL_Animation EL_Animation;
    typedef struct EL_Modulator EL_Modulator;

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

    /* --- Animation attachment (mirror EdgeLightingEffect::Attach / Detach) ---
     *
     * Attached animations are advanced automatically by @ref el_effect_update
     * (using the clock's delta, so a paused clock freezes them) and composited
     * onto the base config so @ref el_effect_render draws the animated result.
     * Once an animation is attached, the caller does NOT call
     * @ref el_animation_update or @ref el_animation_apply for it - those remain
     * available for advanced "manual driver" flows where the caller wants to
     * run an animation without going through the effect's manager.
     *
     * The C handle keeps a shared_ptr to the underlying animation, and Attach
     * takes another, so the animation stays alive as long as either side holds
     * it. Destroying the C handle after Attach is legal but leaves you no way
     * to Detach by identity (use @ref el_effect_detach_all_animations to clear).
     *
     * Recommended lifecycle:
     * @code
     *   EL_Animation *a = el_animation_intensity_fade_in(1.0f, 0.4f, EL_EASE_OUT_CUBIC);
     *   el_animation_set_end_action(a, EL_END_ACTION_HOLD_END);
     *   el_effect_attach_animation(fx, a);
     *   el_animation_play(a);
     *   // per frame - no per-animation calls needed:
     *   el_effect_update(fx, dt);
     *   el_effect_render(fx, w, h);
     *   // when done:
     *   el_effect_detach_animation(fx, a);
     *   el_animation_destroy(a);
     * @endcode
     */

    /** @brief Attach @p anim so the effect's Update advances it and composites
     *         it onto the base config. Ignores null; a duplicate attach of the
     *         same animation is silently ignored. */
    EL_API EL_Result el_effect_attach_animation(EL_Effect *fx, EL_Animation *anim);

    /** @brief Detach @p anim (by identity). The animation object stays alive
     *         as long as the caller holds the handle.
     *  @return 1 if the animation was attached and removed, 0 otherwise. */
    EL_API EL_Bool el_effect_detach_animation(EL_Effect *fx, EL_Animation *anim);

    /** @brief Detach every animation from the effect. */
    EL_API void el_effect_detach_all_animations(EL_Effect *fx);

    /** @brief Number of animations currently attached to the effect. */
    EL_API int32_t el_effect_get_animation_count(EL_Effect *fx);

    /** @brief True if @p anim is currently attached to the effect. */
    EL_API EL_Bool el_effect_contains_animation(EL_Effect *fx, EL_Animation *anim);

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
        EL_ANIM_HUE_REVERSE = 13,
        EL_ANIM_ARC_WIPE = 14 ///< Three-phase grow/chase/shrink wipe around perimeter.
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

    /**
     * @brief Three-phase arc wipe (grow / chase / shrink) around the perimeter.
     * @param duration  Total wipe time in seconds.
     * @param startPos  Perimeter position [0, 1) where the tail begins.
     * @param endPos    Perimeter position [0, 1) where both ends meet at the end.
     *                  Equal to @p startPos → full loop.
     * @param maxLength Arc length during the chase phase, in [0, 1).
     * @param easing    One of EL_Easing; shapes the wall-clock progression.
     */
    EL_API EL_Animation *el_animation_arc_wipe(float duration,
                                               float startPos,
                                               float endPos,
                                               float maxLength,
                                               int32_t easing /*EL_Easing*/);

    /** @brief Destroy an animation. Safe to pass null. */
    EL_API void el_animation_destroy(EL_Animation *anim);

    /* --- Lifecycle ------------------------------------------------------
     *
     * Animations are stateful. The recommended flow is to attach the animation
     * to an effect (@ref el_effect_attach_animation) and let @ref el_effect_update
     * drive Update+Apply for you - see the example there.
     *
     * The manual-driver entry points below (@ref el_animation_update /
     * @ref el_animation_apply) let you run an animation without going through
     * the effect - useful for composing your own manager, unit testing, or
     * driving an animation whose output you plan to consume outside the
     * effect's config. Skip them entirely for the standard flow.
     */

    /** @brief Enter the PLAYING state. From STOPPED, elapsed is reset to 0;
     *         from PAUSED, elapsed continues from its frozen value. No-op if
     *         already PLAYING. */
    EL_API void el_animation_play(EL_Animation *anim);

    /** @brief Freeze elapsed; Apply keeps writing the held value. No-op unless PLAYING. */
    EL_API void el_animation_pause(EL_Animation *anim);

    /** @brief Enter the STOPPED state. Elapsed at the moment of stop is captured
     *         for HOLD_CURRENT; the next Apply writes per @ref el_animation_set_end_action. */
    EL_API void el_animation_stop(EL_Animation *anim);

    /** @brief Zero elapsed and write the modulator's t=0 value into @p cfg.
     *         Does NOT change state. Useful as "rewind while playing" or
     *         "restore baseline while stopped". */
    EL_API EL_Result el_animation_reset(EL_Animation *anim, EL_Config *cfg);

    /** @brief Advance elapsed by @p dt seconds (multiplied by speed). No-op unless PLAYING.
     *         A ONE_SHOT crossing its duration transitions to STOPPED and captures
     *         the completion elapsed. */
    EL_API void el_animation_update(EL_Animation *anim, float dt);

    /** @brief Write the animation's current value into @p cfg (in place).
     *         PLAYING / PAUSED write ApplyAt at the current elapsed;
     *         STOPPED writes per @ref el_animation_set_end_action (or no-ops
     *         if the animation has never played). */
    EL_API EL_Result el_animation_apply(EL_Animation *anim, EL_Config *cfg);

    /* --- Elapsed / state introspection --- */

    /** @brief Current lifecycle state. */
    EL_API EL_AnimationState el_animation_get_state(EL_Animation *anim);

    /** @brief Current elapsed accumulator, in seconds. */
    EL_API float el_animation_get_elapsed(EL_Animation *anim);

    /** @brief Directly overwrite the elapsed accumulator. Does NOT change state
     *         or fire OnComplete; use for scrubbing / testing. */
    EL_API void el_animation_set_elapsed(EL_Animation *anim, float elapsed);

    /* --- End-action policy --- */

    /** @brief What Apply writes once the animation is STOPPED. */
    EL_API EL_EndAction el_animation_get_end_action(EL_Animation *anim);

    /** @brief Set what Apply writes once the animation is STOPPED. */
    EL_API void el_animation_set_end_action(EL_Animation *anim, EL_EndAction action);

    /* --- RESTORE snapshot entry point ---------------------------------
     *
     * Only meaningful when @ref el_animation_set_end_action is
     * EL_END_ACTION_RESTORE. Every built-in animation (presets + generic
     * FieldBoundAnimation) implements the underlying snapshot / restore
     * hooks, so RESTORE works out of the box after a single snapshot call.
     *
     * Call this BEFORE Play so the snapshot captures the pre-animation
     * value. The corresponding restore fires automatically from
     * @ref el_animation_apply while STOPPED - no explicit restore call. */

    /** @brief Snapshot the pre-play field value(s) into the animation. */
    EL_API void el_animation_capture_baseline(EL_Animation *anim, const EL_Config *cfg);

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

    /* --- Speed (playback-rate multiplier; 1.0 = normal) --- */

    /** @brief Current playback rate multiplier. */
    EL_API float el_animation_get_speed(EL_Animation *anim);

    /** @brief Set the playback rate multiplier (1.0 = normal, 0.5 = half, 2.0 = double). */
    EL_API void el_animation_set_speed(EL_Animation *anim, float speed);

    /* --------------------------------------------------------------------------
     * Modulators (compose your own animations)
     *
     * Mirrors EdgeLighting::Modulator - pure `time -> float` primitives that can
     * be composed (multiplied, added, sequenced) into arbitrarily complex signals.
     *
     * Ownership model
     * ---------------
     * All factories return an owning EL_Modulator* the caller must destroy.
     * Composition factories (multiplier/adder/remap/sequence_append) SHARE
     * ownership of their inputs - the caller may destroy their handle right after
     * composing, the composition keeps the underlying modulator alive internally.
     *
     * Typical use
     * -----------
     *   EL_Modulator *env    = el_modulator_ease(0, 1, 0.3f, EL_EASE_OUT_CUBIC, 0);
     *   EL_Modulator *osc    = el_modulator_oscillator(1, 0.6f, 1, 0, EL_WAVE_SINE);
     *   EL_Modulator *signal = el_modulator_multiplier(env, osc);
     *   el_modulator_destroy(env);
     *   el_modulator_destroy(osc);
     *
     *   EL_Animation *anim = el_animation_from_modulator(EL_FIELD_NEON_INTENSITY, signal);
     *   el_modulator_destroy(signal);
     *   el_animation_play(anim);
     *   // per frame: el_animation_update(anim, dt); el_animation_apply(anim, &cfg);
     * ------------------------------------------------------------------------ */

    /** Periodic waveform shapes for @ref el_modulator_oscillator. */
    typedef enum EL_Waveform
    {
        EL_WAVE_SINE = 0,     ///< Smooth periodic (default).
        EL_WAVE_TRIANGLE = 1, ///< Linear up + linear down.
        EL_WAVE_SQUARE = 2,   ///< Hard on/off - useful for strobes.
        EL_WAVE_SAWTOOTH = 3  ///< Linear ramp 0->1, snap back.
    } EL_Waveform;

    /** Scalar Config leaves that @ref el_animation_from_modulator and
     *  @ref el_animation_add_field can drive. Single-value NeonConfig
     *  scalars only. Vector / enum / geometry fields (colour stops,
     *  glowSide, position vec2) are not exposed yet.
     *
     *  Segment struct scalars (position / length / boost inside
     *  neon.segmentBoosts[i]) live in @ref EL_SegmentField - drive them via
     *  @ref el_animation_add_segment_field so the index is required at bind
     *  time.
     *
     *  New values are appended at the end so existing binaries stay
     *  ABI-compatible with headers that add fields - old code will simply
     *  return the default branch (no-op) for enum values it doesn't
     *  recognise. */
    typedef enum EL_ConfigField
    {
        EL_FIELD_NEON_INTENSITY = 0,
        EL_FIELD_NEON_LINE_WIDTH = 1,
        EL_FIELD_NEON_GLOW_RADIUS = 2,
        EL_FIELD_NEON_BLOOM_STRENGTH = 3,
        EL_FIELD_NEON_FILAMENT_FALLOFF = 4,
        EL_FIELD_NEON_GLOW_SIDE_SOFTNESS = 5,
        EL_FIELD_NEON_HUE_ROTATION_RATE = 6,
        EL_FIELD_NEON_ARC_START = 7,
        EL_FIELD_NEON_ARC_LENGTH = 8
    } EL_ConfigField;

    /** Which scalar to drive inside a neon.segmentBoosts entry.
     *  Paired with an index at bind time via
     *  @ref el_animation_add_segment_field. Mirrors
     *  EdgeLighting::SegmentField 1:1. */
    typedef enum EL_SegmentField
    {
        EL_SEGMENT_FIELD_POSITION = 0,
        EL_SEGMENT_FIELD_LENGTH = 1,
        EL_SEGMENT_FIELD_BOOST = 2
    } EL_SegmentField;

    /* --- Modulator factories ---
     * Each returns an owning handle (null on allocation failure). */

    /** @brief Returns @p value at every time. */
    EL_API EL_Modulator *el_modulator_constant(float value);

    /**
     * @brief Periodic waveform in [min, max].
     * @param frequency Cycles per second (Hz).
     * @param phase     Initial phase in cycles [0, 1).
     * @param waveform  One of EL_Waveform.
     */
    EL_API EL_Modulator *el_modulator_oscillator(
        float frequency, float min, float max, float phase, int32_t waveform /*EL_Waveform*/);

    /**
     * @brief One-shot or looping easing curve from @p from to @p to over @p duration seconds.
     * @param easing One of EL_Easing.
     */
    EL_API EL_Modulator *el_modulator_ease(
        float from, float to, float duration, int32_t easing /*EL_Easing*/, EL_Bool loop);

    /**
     * @brief Empty sequence. Fill via @ref el_modulator_sequence_append, one segment at a time.
     * @param loop If true, wraps back to the start after the final segment ends.
     */
    EL_API EL_Modulator *el_modulator_sequence(EL_Bool loop);

    /**
     * @brief Append a segment to a sequence created by @ref el_modulator_sequence.
     * @param seq      The sequence handle.
     * @param segment  Modulator that runs during this segment. Shared ownership.
     * @param duration Segment length in seconds.
     * @return EL_ERR_NULL_ARG on null inputs; otherwise EL_OK.
     */
    EL_API EL_Result el_modulator_sequence_append(
        EL_Modulator *seq, EL_Modulator *segment, float duration);

    /** @brief Product of two modulators: a(t) * b(t). */
    EL_API EL_Modulator *el_modulator_multiplier(EL_Modulator *a, EL_Modulator *b);

    /** @brief Sum of two modulators: a(t) + b(t). */
    EL_API EL_Modulator *el_modulator_adder(EL_Modulator *a, EL_Modulator *b);

    /** @brief Rescale @p inner's output into [outMin, outMax]. */
    EL_API EL_Modulator *el_modulator_remap(EL_Modulator *inner, float outMin, float outMax);

    /** @brief Destroy a modulator handle. Safe with null; underlying object lives on
     *         as long as any composition still refers to it. */
    EL_API void el_modulator_destroy(EL_Modulator *mod);

    /** @brief Evaluate at @p time. Useful for testing. Returns 0 on null. */
    EL_API float el_modulator_evaluate(EL_Modulator *mod, float time);

    /**
     * @brief Wrap a modulator into an EL_Animation that writes into @p field.
     * @param field  One of EL_ConfigField.
     * @param mod    Modulator to evaluate each Apply. Shared ownership.
     * @return An EL_Animation the caller destroys with @ref el_animation_destroy.
     *         Loops forever (playback mode LOOP) - set one-shot via
     *         @ref el_animation_set_playback_mode if desired.
     */
    EL_API EL_Animation *el_animation_from_modulator(int32_t field /*EL_ConfigField*/, EL_Modulator *mod);

    /**
     * @brief Create an empty field-bound animation, ready to accept multiple
     *        @ref el_animation_add_field bindings.
     * @details All bindings share the animation's elapsed and duration, so they
     *          stay phase-locked - use this to drive several fields off one
     *          clock (e.g. a "breathing" effect that pulses intensity, glow,
     *          and bloom together). For independent parallel animations, keep
     *          them as separate handles and update each with its own
     *          @ref el_animation_apply call.
     * @return An EL_Animation the caller destroys with @ref el_animation_destroy.
     */
    EL_API EL_Animation *el_animation_empty(void);

    /**
     * @brief Append a scalar (field, modulator) binding to a field-bound animation.
     * @param anim  Animation created via @ref el_animation_empty or
     *              @ref el_animation_from_modulator. Not a preset animation
     *              (@ref el_animation_create) - those bind their own fields
     *              internally and return @c EL_ERR_NULL_ARG here.
     * @param field One of EL_ConfigField.
     * @param mod   Modulator to evaluate each Apply. Shared ownership.
     */
    EL_API EL_Result el_animation_add_field(EL_Animation *anim,
                                            int32_t field /*EL_ConfigField*/,
                                            EL_Modulator *mod);

    /**
     * @brief Append a segment (index, field, modulator) binding to a
     *        field-bound animation. Drives one scalar inside
     *        @c cfg.neon.segmentBoosts[index], auto-growing the vector to
     *        that slot.
     * @param anim  Animation created via @ref el_animation_empty or
     *              @ref el_animation_from_modulator. Not a preset animation.
     * @param index Which entry in segmentBoosts to drive.
     * @param field One of EL_SegmentField (POSITION / LENGTH / BOOST).
     * @param mod   Modulator to evaluate each Apply. Shared ownership.
     * @details Use multiple calls with different indices to drive several
     *          segments off one phase-locked clock.
     */
    EL_API EL_Result el_animation_add_segment_field(EL_Animation *anim,
                                                    int32_t index,
                                                    int32_t field /*EL_SegmentField*/,
                                                    EL_Modulator *mod);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EDGE_LIGHTING_C_H_
