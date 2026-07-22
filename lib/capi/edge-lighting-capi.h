/**
 * @file edge-lighting-capi.h
 * @brief Flat C ABI over the EdgeLighting C++ renderer for FFI / C# interop.
 *
 * @section overview Overview
 *
 * This header is the only interop surface between native EdgeLighting and
 * managed callers (P/Invoke, ctypes, cgo, ...). Every symbol has C linkage,
 * uses only fixed-width scalar types + opaque pointers, and never leaks a
 * C++ exception - anything thrown at the boundary is caught and mapped to
 * @ref EL_ERROR_OUT_OF_MEMORY (on @c std::bad_alloc) or @ref EL_ERROR_INVALID_PARAMETER (on other C++ exceptions). Higher-level C++ features (RAII wrappers,
 * @c std::vector, @c glm) are hidden behind opaque handles.
 *
 * The C++ types the API mirrors live under @c lib/include/:
 * - @c EdgeLighting::EdgeLightingEffect (@c core/edge-lighting.h) - the
 *   orchestrator; one @c el_effect_handle_t wraps one instance plus its
 *   staging @c Config.
 * - @c EdgeLighting::Config (@c core/config.h) - geometry + per-renderer
 *   sub-configs; every @c el_effect_set_* mutates the effect's staging copy
 *   and every @c el_effect_get_* reads it back.
 * - @c EdgeLighting::Animation / @c FieldBoundAnimation
 *   (@c animation/animation.h, @c animation/field-bound-animation.h) -
 *   animations own their own play state; the flat animation factories below
 *   return one @c el_animation_handle_t per instance.
 * - @c EdgeLighting::Modulator (@c animation/modulator.h) - pure
 *   @c time -> float composables driving @c FieldBoundAnimation bindings.
 *
 * @section handles Opaque handles and ownership
 *
 * Three handle families:
 * - @c el_effect_handle_t - one per @ref el_effect_create call; destroy with
 *   @ref el_effect_destroy. Owns the GL resources allocated by
 *   @ref el_effect_init.
 * - @c el_animation_handle_t - one per @c el_animation_create* call; destroy
 *   with @ref el_animation_destroy. Attaching an animation to an effect does
 *   NOT transfer ownership - the caller must still destroy the handle. It is
 *   safe to destroy an attached animation; the effect drops its internal
 *   reference on the next @ref el_effect_detach_animation /
 *   @ref el_effect_detach_all_animations, so avoid leaking a still-attached
 *   handle across shutdown.
 * - @c el_modulator_handle_t - one per @c el_modulator_create* call; destroy
 *   with @ref el_modulator_destroy. Passing a modulator into a composite
 *   (@c Sequence / @c Multiplier / @c Adder / @c Remap) or into a
 *   @ref el_animation_from_modulator / @ref el_animation_add_field binding
 *   shares ownership - the composite retains a strong reference internally.
 *   The caller can (and should) destroy the outer @c el_modulator_handle_t
 *   once it is no longer needed to poke; the underlying modulator lives on
 *   until the last strong reference goes away.
 *
 * Handles are opaque pointer types - never dereference or pointer-arithmetic
 * them from the FFI side. Passing a null handle to any function returns
 * @ref EL_ERROR_INVALID_HANDLE; passing an already-destroyed handle is undefined
 * behaviour (mirror the same lifetime discipline you use for any handle).
 *
 * @section staging Staging config semantics
 *
 * The C++ effect exposes two configs internally - the "base" (what the last
 * @c SetConfig received) and the "active" (base + animation overlays, what
 * the renderers see). The C API exposes only the base as a *staging* config:
 *
 * - Every @c el_effect_set_* stashes a value in the effect's staging @c Config
 *   and calls @c SetConfig immediately so renderers pick it up next frame.
 * - Every @c el_effect_get_* reads that staging value back - not the
 *   animation-overlaid active value.
 * - @ref el_effect_capture re-syncs staging from the effect's authoritative
 *   base config; call it if you mutate the effect from C++ side channels
 *   (e.g. a preset applied inside the renderer that changes the base).
 *
 * @section errors Error model
 *
 * All fallible functions return an @ref el_result_e code. Value-returning
 * factories (@c el_effect_create / @c el_animation_create* /
 * @c el_modulator_create*) return @c NULL on failure. A caught exception at
 * the ABI boundary always maps to @ref EL_ERROR_OUT_OF_MEMORY (on @c std::bad_alloc) or @ref EL_ERROR_INVALID_PARAMETER (on other C++ exceptions); look at the native
 * log stream (see @c LOG_E) for the diagnostic message. Setters are also
 * idempotent - if the incoming value equals the staging value, no work is
 * done and @ref EL_SUCCESS is returned.
 *
 * @section threading Threading
 *
 * The renderer holds live GL state; every function that touches an effect
 * (create/init/update/render/set/get) must run on the thread that owns the
 * GL context. Animation and modulator factories touch no GL and may run on
 * any thread as long as the resulting handle is only passed into effect
 * calls on the GL thread.
 */
#ifndef _EDGE_LIGHTING_CAPI_H_
#define _EDGE_LIGHTING_CAPI_H_

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

    /* ======================================================================
     * Result codes and shared scalar types
     * ==================================================================== */

    /** @brief Return code shared by every fallible @c el_* function.
     *  @details @ref EL_SUCCESS is 0 so callers can use the classic
     *           @c if (rc) branch-on-error idiom. Non-zero values are
     *           distinct so a caller can decide whether to retry, surface,
     *           or ignore. */
    typedef enum el_result_e
    {
        EL_SUCCESS = 0,                  /**< Success. */
        EL_ERROR_INVALID_HANDLE = -1,    /**< A required handle (effect/animation/modulator) was null or destroyed. */
        EL_ERROR_INIT_FAILED = -2,       /**< Renderer/GL initialisation failed (see native log). */
        EL_ERROR_OUT_OF_MEMORY = -3,     /**< Allocation failed (@c std::bad_alloc caught at the ABI boundary). */
        EL_ERROR_INVALID_PARAMETER = -4, /**< A non-handle argument was null, out of range, or the wrong shape; also the catch-all for other C++ exceptions caught at the ABI boundary. */
        EL_ERROR_FILE_NOT_FOUND = -5,    /**< A referenced file could not be opened. Reserved for future file-loading APIs. */
        EL_ERROR_UNSUPPORTED_FORMAT = -6 /**< A file / asset was found but its format is not supported. Reserved for future file-loading APIs. */
    } el_result_e;

    /** @brief Fixed-width boolean for ABI stability. 0 = false, non-zero = true.
     *  @details Use anywhere a semantic boolean crosses the FFI boundary -
     *           avoids the platform-dependent width of a bare @c int and
     *           self-documents the intent. */
    typedef int32_t el_bool_t;

    /* ======================================================================
     * Enums
     *
     * All enums below are ABI-stable: @c static_assert s at the top of
     * edge-lighting-capi.cpp fire at compile time if a matching C++ enum ever
     * reorders. When adding new values, append them at the end.
     * ==================================================================== */

    /** @brief Direction of traversal around the rectangle perimeter.
     *  @details Mirrors @c EdgeLighting::Winding. Both start at the top-left
     *           corner - clockwise goes top->right->bottom->left, counter
     *           goes left->bottom->right->top. Affects the mapping of colour
     *           stops / segment positions to on-screen pixels. */
    typedef enum el_winding_e
    {
        EL_WINDING_CLOCKWISE = 0,        /**< Top-left, clockwise. */
        EL_WINDING_COUNTER_CLOCKWISE = 1 /**< Top-left, counter-clockwise. */
    } el_winding_e;

    /** @brief Which side of the line the neon glow spills onto.
     *  @details Mirrors @c EdgeLighting::GlowSide. */
    typedef enum el_glow_side_e
    {
        EL_GLOW_SIDE_BOTH = 0,   /**< Glow on both sides (default neon look). */
        EL_GLOW_SIDE_INSIDE = 1, /**< Glow only inside the rectangle. */
        EL_GLOW_SIDE_OUTSIDE = 2 /**< Glow only outside the rectangle. */
    } el_glow_side_e;

    /** @brief Where the opaque-mode fill covers pixels.
     *  @details Mirrors @c EdgeLighting::OpaqueMode. The fill is a rounded
     *           band sized by @ref el_effect_set_inside_cutoff /
     *           @ref el_effect_set_outside_cutoff; the neon emission still
     *           composites on top inside the glow band. */
    typedef enum el_opaque_mode_e
    {
        EL_OPAQUE_MODE_NONE = 0,    /**< No opaque pass; effect composites transparently. */
        EL_OPAQUE_MODE_OUTSIDE = 1, /**< Fill outer half of the band: 0 <= d <= outsideCutoff. */
        EL_OPAQUE_MODE_INSIDE = 2,  /**< Fill inner half of the band: -insideCutoff <= d <= 0. */
        EL_OPAQUE_MODE_BOTH = 3,    /**< Fill the whole band: -insideCutoff <= d <= +outsideCutoff. */
        EL_OPAQUE_MODE_ALL = 4      /**< Fill the whole viewport. */
    } el_opaque_mode_e;

    /** @brief Colour space used when interpolating between colour stops.
     *  @details Mirrors @c EdgeLighting::BlendSpace. HSV/HSL avoid the muddy
     *           mid-tones that a straight-line RGB interpolation produces
     *           when the endpoints are far apart on the colour wheel. */
    typedef enum el_blend_space_e
    {
        EL_BLEND_SPACE_RGB = 0, /**< Linear RGB. */
        EL_BLEND_SPACE_HSV = 1, /**< HSV, then back to RGB. */
        EL_BLEND_SPACE_HSL = 2  /**< HSL, then back to RGB. */
    } el_blend_space_e;

    /** @brief One-cycle vs. forever behaviour for an animation.
     *  @details Mirrors @c EdgeLighting::PlaybackMode. Duration means "length
     *           of one cycle" in both modes. See @ref el_end_action_e for
     *           what happens once a one-shot completes. */
    typedef enum el_playback_mode_e
    {
        EL_PLAYBACK_LOOP = 0,    /**< Elapsed wraps at duration; never completes. */
        EL_PLAYBACK_ONE_SHOT = 1 /**< Runs for exactly one cycle, then stops. */
    } el_playback_mode_e;

    /** @brief Runtime play state of an animation.
     *  @details Mirrors @c EdgeLighting::AnimationState. Every animation
     *           carries its own state independent of the effect clock. */
    typedef enum el_animation_state_e
    {
        EL_ANIM_STATE_STOPPED = 0, /**< Initial state and after stop/completion. */
        EL_ANIM_STATE_PLAYING = 1, /**< Elapsed advances; Apply writes current value. */
        EL_ANIM_STATE_PAUSED = 2   /**< Elapsed frozen; Apply still writes the frozen value. */
    } el_animation_state_e;

    /** @brief What the animation writes once it enters the STOPPED state.
     *  @details Mirrors @c EdgeLighting::EndAction. A never-played animation
     *           is a no-op regardless of end action - the field stays at its
     *           base value. */
    typedef enum el_end_action_e
    {
        EL_END_ACTION_HOLD_CURRENT = 0, /**< Freeze at the value at stop time (default). */
        EL_END_ACTION_HOLD_END = 1,     /**< Snap to the modulator's value at t=duration. */
        EL_END_ACTION_HOLD_START = 2,   /**< Snap to the modulator's value at t=0. */
        EL_END_ACTION_RESTORE = 3       /**< Restore the pre-play baseline; needs @ref el_animation_capture_baseline. */
    } el_end_action_e;

    /** @brief Easing curves available to @c Ease / factory animations.
     *  @details Mirrors @c EdgeLighting::EasingFunction curves. */
    typedef enum el_easing_e
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
    } el_easing_e;

    /** @brief Named preset for @ref el_animation_create.
     *  @details Each preset builds a pre-tuned animation targeting a
     *           specific field (intensity, glow radius, arc, ...). Prefer the
     *           dedicated @c el_animation_create_* factories when you want
     *           to override the internal parameters. */
    typedef enum el_animation_preset_e
    {
        EL_ANIM_NONE = 0,            /**< Empty animation; useful as a placeholder. */
        EL_ANIM_BREATHING = 1,       /**< Soft intensity sine, ~1 Hz. */
        EL_ANIM_STROBE = 2,          /**< Square-wave on/off strobe. */
        EL_ANIM_HEARTBEAT = 3,       /**< Two-bump asymmetric pulse. */
        EL_ANIM_SHIMMER = 4,         /**< High-frequency low-amplitude wobble. */
        EL_ANIM_AURORA = 5,          /**< Slow hue-drift + gentle bloom breath. */
        EL_ANIM_REVERSE_SWEEP = 6,   /**< Hue direction reverses each cycle. */
        EL_ANIM_FADE_IN = 7,         /**< One-shot ease from 0 to full intensity. */
        EL_ANIM_SEGMENT_TRAVEL = 8,  /**< Single travelling segment, wraps forever. */
        EL_ANIM_SEGMENT_BOUNCE = 9,  /**< Single segment ping-ponging on the perimeter. */
        EL_ANIM_COMET = 10,          /**< Bright head with a soft trailing tail. */
        EL_ANIM_OUTLINE_TRACER = 11, /**< Arc paints in around the perimeter. */
        EL_ANIM_FADE_OUT = 12,       /**< One-shot ease from full intensity to 0. */
        EL_ANIM_HUE_REVERSE = 13,    /**< Hue rate reverses under a smooth envelope. */
        EL_ANIM_ARC_WIPE = 14        /**< Arc grows from a start position to an end. */
    } el_animation_preset_e;

    /** @brief Periodic shape for @ref el_modulator_create_oscillator.
     *  @details Mirrors @c EdgeLighting::Waveform. */
    typedef enum el_waveform_e
    {
        EL_WAVE_SINE = 0,     /**< Smooth periodic (default). */
        EL_WAVE_TRIANGLE = 1, /**< Linear up + linear down. */
        EL_WAVE_SQUARE = 2,   /**< Hard on/off - useful for strobes. */
        EL_WAVE_SAWTOOTH = 3  /**< Linear ramp 0->1, snap back. */
    } el_waveform_e;

    /** @brief Scalar @c NeonConfig leaves that a field-bound animation can drive.
     *  @details Mirrors @c EdgeLighting::AnimatableField. Non-scalar fields
     *           (segment/arc entries, colour stops) have their own enums
     *           below - use them with the corresponding
     *           @c el_animation_add_*_field function. */
    typedef enum el_config_field_e
    {
        EL_FIELD_NEON_INTENSITY = 0,          /**< @c NeonConfig::intensity */
        EL_FIELD_NEON_LINE_WIDTH = 1,         /**< @c NeonConfig::lineWidth */
        EL_FIELD_NEON_GLOW_RADIUS = 2,        /**< @c NeonConfig::glowRadius */
        EL_FIELD_NEON_BLOOM_STRENGTH = 3,     /**< @c NeonConfig::bloomStrength */
        EL_FIELD_NEON_FILAMENT_FALLOFF = 4,   /**< @c NeonConfig::filamentFalloff */
        EL_FIELD_NEON_GLOW_SIDE_SOFTNESS = 5, /**< @c NeonConfig::glowSideSoftness */
        EL_FIELD_NEON_HUE_ROTATION_RATE = 6   /**< @c NeonConfig::hueRotationRate */
    } el_config_field_e;

    /** @brief Scalar inside a @c NeonConfig::segmentBoosts entry.
     *  @details Mirrors @c EdgeLighting::SegmentField. Paired with an index
     *           at bind time via @ref el_animation_add_segment_field. */
    typedef enum el_segment_field_e
    {
        EL_SEGMENT_FIELD_POSITION = 0, /**< Centre of the segment on the perimeter [0,1). */
        EL_SEGMENT_FIELD_LENGTH = 1,   /**< Segment width as a perimeter fraction (~2σ). */
        EL_SEGMENT_FIELD_BOOST = 2     /**< Peak brightness (absolute, added to base arc). */
    } el_segment_field_e;

    /** @brief Scalar inside a @c NeonConfig::arcs entry.
     *  @details Mirrors @c EdgeLighting::ArcField. Paired with an index at
     *           bind time via @ref el_animation_add_arc_field. */
    typedef enum el_arc_field_e
    {
        EL_ARC_FIELD_START = 0,    /**< Start of the arc on the perimeter [0,1). */
        EL_ARC_FIELD_LENGTH = 1,   /**< Length of the arc as a perimeter fraction [0,1]. */
        EL_ARC_FIELD_INTENSITY = 2 /**< Per-arc brightness multiplier. */
    } el_arc_field_e;

    /** @brief Scalar inside a single colour stop.
     *  @details Mirrors @c EdgeLighting::ColorStopField. Paired with the
     *           containing entry's index at bind time via
     *           @ref el_animation_add_arc_stop_field /
     *           @ref el_animation_add_segment_stop_field. */
    typedef enum el_color_stop_field_e
    {
        EL_STOP_FIELD_POSITION = 0, /**< Normalised offset within the segment/arc span. */
        EL_STOP_FIELD_R = 1,        /**< Red channel. */
        EL_STOP_FIELD_G = 2,        /**< Green channel. */
        EL_STOP_FIELD_B = 3,        /**< Blue channel. */
        EL_STOP_FIELD_A = 4         /**< Alpha channel. */
    } el_color_stop_field_e;

    /* ======================================================================
     * Opaque handles
     * ==================================================================== */

    /** @brief Handle to a live @c EdgeLightingEffect + its staging @c Config. */
    typedef struct el_effect_handle_impl *el_effect_handle_t;
    /** @brief Handle to a live @c Animation. Multiple animations can be
     *         attached to one effect; each carries its own play state. */
    typedef struct el_animation_handle_impl *el_animation_handle_t;
    /** @brief Handle to a live @c Modulator (pure time->float source). */
    typedef struct el_modulator_handle_impl *el_modulator_handle_t;

    /* ======================================================================
     * Effect - config setters and getters
     *
     * Every setter mutates the effect's staging @c Config and immediately
     * calls @c SetConfig on the underlying effect. Every getter reads that
     * staging copy - not the animation-overlaid active config. Getters
     * always require a non-null @p out* pointer.
     *
     * Setters are idempotent: assigning the same value twice is a cheap
     * no-op and still returns @ref EL_SUCCESS.
     * ==================================================================== */

    /** @name Geometry
     *  Rectangle position, size, corner radius, and winding.
     *  @{ */

    /** @brief Set the target rectangle geometry.
     *  @param effect       Effect handle.
     *  @param width        Rectangle width in pixels (must be >= 0).
     *  @param height       Rectangle height in pixels (must be >= 0).
     *  @param posX         Top-left X in viewport coordinates.
     *  @param posY         Top-left Y in viewport coordinates.
     *  @param cornerRadius Corner radius in pixels; 0 = sharp corners. */
    EL_API el_result_e el_effect_set_geometry(el_effect_handle_t effect,
                                              float width, float height, float posX, float posY, float cornerRadius);
    /** @brief Read the target rectangle geometry back from the staging config. */
    EL_API el_result_e el_effect_get_geometry(el_effect_handle_t effect,
                                              float *outWidth, float *outHeight, float *outPosX, float *outPosY,
                                              float *outCornerRadius);

    /** @brief Set the traversal direction around the perimeter (CW / CCW). */
    EL_API el_result_e el_effect_set_winding(el_effect_handle_t effect, el_winding_e winding);
    EL_API el_result_e el_effect_get_winding(el_effect_handle_t effect, el_winding_e *outWinding);

    /** @} */

    /** @name Renderer toggles
     *  Independent on/off switches for the neon, optimized, and wireframe
     *  layers. Any subset can be enabled at once - they composite additively.
     *  @{ */

    EL_API el_result_e el_effect_set_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Draw the baked gradient LUT as a debug strip across the rectangle. */
    EL_API el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t effect, el_bool_t show);
    EL_API el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow);

    /** @brief Draw a coloured dot at each colour-stop position on the perimeter. */
    EL_API el_result_e el_effect_set_show_color_stops(el_effect_handle_t effect, el_bool_t show);
    EL_API el_result_e el_effect_get_show_color_stops(el_effect_handle_t effect, el_bool_t *outShow);

    /** @} */

    /** @name Compositing
     *  How the effect blends with the framebuffer.
     *  @{ */

    /** @brief Where the opaque-mode fill covers pixels.
     *  @details See @ref el_opaque_mode_e. NONE keeps the effect purely
     *           transparent; the other modes rasterise a coloured band shaped
     *           by @ref el_effect_set_inside_cutoff /
     *           @ref el_effect_set_outside_cutoff and filled with
     *           @ref el_effect_set_opaque_color. */
    EL_API el_result_e el_effect_set_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e mode);
    EL_API el_result_e el_effect_get_opaque_mode(el_effect_handle_t effect, el_opaque_mode_e *outMode);

    /** @brief Set the background fill colour used when opaque compositing is on.
     *  @details Linear RGBA in [0,1]. Only the @c rgb channels are read
     *           today; @c a is reserved for a future partial-fill pass. */
    EL_API el_result_e el_effect_set_opaque_color(el_effect_handle_t effect,
                                                  float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_opaque_color(el_effect_handle_t effect,
                                                  float *outR, float *outG, float *outB, float *outA);

    /** @brief Feather width in pixels at the opaque fill's cutoff boundaries.
     *  @details Applied only when @c opaqueMode != NONE. 0 = hard fill edge;
     *           larger values soften where the fill fades to background. Kept
     *           independent of the per-side cutoff softness so the emission
     *           and the fill can taper at different rates. */
    EL_API el_result_e el_effect_set_opaque_softness(el_effect_handle_t effect, float softness);
    EL_API el_result_e el_effect_get_opaque_softness(el_effect_handle_t effect, float *outSoftness);

    /** @brief Inside cutoff (rect interior side): hard geometric limit for the neon glow.
     *  @details @c enable = 0 leaves the interior uncapped (natural halo/bloom
     *           decay bounds the emission). @c size is the pixel distance from
     *           the rect edge to the cutoff boundary along the interior side
     *           (always positive). @c softness is the feather width in pixels
     *           at the boundary (0 = hard, larger = smoother fade). Also caps
     *           the geometric footprint of @c INSIDE / @c BOTH opaque fills. */
    EL_API el_result_e el_effect_set_inside_cutoff(el_effect_handle_t effect,
                                                   el_bool_t enable, float size, float softness);
    EL_API el_result_e el_effect_get_inside_cutoff(el_effect_handle_t effect,
                                                   el_bool_t *outEnable, float *outSize, float *outSoftness);

    /** @brief Outside cutoff (rect exterior side). Mirror of @ref el_effect_set_inside_cutoff.
     *  @details Also caps @c OUTSIDE / @c BOTH opaque fills and sizes the neon
     *           draw quad so far-exterior pixels are rasteriser-culled. */
    EL_API el_result_e el_effect_set_outside_cutoff(el_effect_handle_t effect,
                                                    el_bool_t enable, float size, float softness);
    EL_API el_result_e el_effect_get_outside_cutoff(el_effect_handle_t effect,
                                                    el_bool_t *outEnable, float *outSize, float *outSoftness);

    /** @} */

    /** @name Neon filament and glow
     *  Line width, brightness, and glow shaping.
     *  @{ */

    /** @brief Set the bright-filament line width in pixels.
     *  @details Peak brightness stays constant - this only changes the
     *           thickness of the emitting line. */
    EL_API el_result_e el_effect_set_line_width(el_effect_handle_t effect, float width);
    EL_API el_result_e el_effect_get_line_width(el_effect_handle_t effect, float *outWidth);

    /** @brief Set the filament brightness falloff exponent.
     *  @details 1.0 is a clean Gaussian roll-off (default). Lower values
     *           give heavier tails (softer peak), higher values sharpen the
     *           edge until the line reads as a hard plateau. */
    EL_API el_result_e el_effect_set_filament_falloff(el_effect_handle_t effect, float falloff);
    EL_API el_result_e el_effect_get_filament_falloff(el_effect_handle_t effect, float *outFalloff);

    /** @brief Set the master brightness multiplier for the arc emission.
     *  @details Applies uniformly to filament + halo + bloom. Use per-arc
     *           intensity to fade a single slice while others stay lit.
     *           Segment boosts deliberately bypass this multiplier. */
    EL_API el_result_e el_effect_set_intensity(el_effect_handle_t effect, float intensity);
    EL_API el_result_e el_effect_get_intensity(el_effect_handle_t effect, float *outIntensity);

    /** @brief Set the halo reach in pixels (spread of the coloured glow). */
    EL_API el_result_e el_effect_set_glow_radius(el_effect_handle_t effect, float radius);
    EL_API el_result_e el_effect_get_glow_radius(el_effect_handle_t effect, float *outRadius);

    /** @brief Set the wide background bloom strength (0 = halo only). */
    EL_API el_result_e el_effect_set_bloom_strength(el_effect_handle_t effect, float strength);
    EL_API el_result_e el_effect_get_bloom_strength(el_effect_handle_t effect, float *outStrength);

    /** @brief Restrict the glow to one side of the line, or let it spill both ways. */
    EL_API el_result_e el_effect_set_glow_side(el_effect_handle_t effect, el_glow_side_e side);
    EL_API el_result_e el_effect_get_glow_side(el_effect_handle_t effect, el_glow_side_e *outSide);

    /** @brief Set the softness of the one-sided cut in pixels.
     *  @details Ignored when the glow side is BOTH. 0 = hard edge. */
    EL_API el_result_e el_effect_set_glow_side_softness(el_effect_handle_t effect, float softness);
    EL_API el_result_e el_effect_get_glow_side_softness(el_effect_handle_t effect, float *outSoftness);

    /** @} */

    /** @name Colour and animation
     *  Base gradient blend space, hue rotation, transition duration.
     *  @{ */

    /** @brief Set the base gradient's colour-interpolation space. */
    EL_API el_result_e el_effect_set_blend_space(el_effect_handle_t effect, el_blend_space_e space);
    EL_API el_result_e el_effect_get_blend_space(el_effect_handle_t effect, el_blend_space_e *outSpace);

    /** @brief Set the hue-rotation rate in revolutions per second (0 = static). */
    EL_API el_result_e el_effect_set_hue_rotation_rate(el_effect_handle_t effect, float rate);
    EL_API el_result_e el_effect_get_hue_rotation_rate(el_effect_handle_t effect, float *outRate);

    /** @brief Set the cross-fade duration (seconds) applied when colour stops
     *         or blend space change. 0 = instant snap. */
    EL_API el_result_e el_effect_set_color_transition_duration(el_effect_handle_t effect, float seconds);
    EL_API el_result_e el_effect_get_color_transition_duration(el_effect_handle_t effect, float *outSeconds);

    /** @} */

    /** @name Base colour stops (perimeter gradient)
     *  1 stop = solid colour, 2 = gradient, 3+ = multi-stop circular gradient.
     *  Stops are addressed by index [0, count). The typical write sequence is
     *  @ref el_effect_set_color_stop_count then per-index
     *  @ref el_effect_set_color_stop. Clearing drops the vector entirely.
     *  @{ */

    /** @brief Resize the base colour-stops vector.
     *  @details Growing seeds new entries with defaults (position=0, opaque
     *           white); shrinking truncates. */
    EL_API el_result_e el_effect_set_color_stop_count(el_effect_handle_t effect, int32_t count);
    EL_API el_result_e el_effect_get_color_stop_count(el_effect_handle_t effect, int32_t *outCount);

    /** @brief Write one colour stop.
     *  @param index    Stop index in [0, count).
     *  @param position Normalised perimeter position in [0, 1].
     *  @param r,g,b,a  Linear RGBA in [0, 1]. */
    EL_API el_result_e el_effect_set_color_stop(el_effect_handle_t effect, int32_t index,
                                                float position, float r, float g, float b, float a);
    /** @brief Read one colour stop. Returns @ref EL_ERROR_INVALID_PARAMETER on OOB index. */
    EL_API el_result_e el_effect_get_color_stop(el_effect_handle_t effect, int32_t index,
                                                float *outPosition, float *outR, float *outG, float *outB, float *outA);
    /** @brief Drop every base colour stop. */
    EL_API el_result_e el_effect_clear_color_stops(el_effect_handle_t effect);

    /** @} */

    /** @name Segment boosts (travelling Gaussian hotspots)
     *  Multiple hotspots layered on top of the base neon. Each has its own
     *  position, length, boost, colour stops, and blend space. Segments
     *  compose additively - they can shine on a dark arc. See
     *  @c EdgeLighting::SegmentBoost for the full semantics.
     *  @{ */

    /** @brief Resize the segment-boosts vector.
     *  @details Cap is @c NeonConfig::MAX_SEGMENT_BOOSTS_CAP; values above
     *           that return @ref EL_ERROR_INVALID_PARAMETER. */
    EL_API el_result_e el_effect_set_segment_boost_count(el_effect_handle_t effect, int32_t count);
    EL_API el_result_e el_effect_get_segment_boost_count(el_effect_handle_t effect, int32_t *outCount);

    /** @brief Write the scalar fields of one segment boost.
     *  @param position Perimeter position of the segment centre in [0, 1).
     *  @param length   Width as a perimeter fraction (~2σ of the Gaussian).
     *  @param boost    Peak brightness added on top of the base arc. */
    EL_API el_result_e el_effect_set_segment_boost(el_effect_handle_t effect, int32_t index,
                                                   float position, float length, float boost);
    EL_API el_result_e el_effect_get_segment_boost(el_effect_handle_t effect, int32_t index,
                                                   float *outPosition, float *outLength, float *outBoost);
    /** @brief Drop every segment boost. */
    EL_API el_result_e el_effect_clear_segment_boosts(el_effect_handle_t effect);

    /** @brief Set the blend space used for the segment's own colour stops.
     *  @details Ignored when the segment has no colour stops (the base
     *           gradient's blend space applies). */
    EL_API el_result_e el_effect_set_segment_blend_space(el_effect_handle_t effect,
                                                         int32_t segmentIndex, el_blend_space_e blendSpace);
    EL_API el_result_e el_effect_get_segment_blend_space(el_effect_handle_t effect,
                                                         int32_t segmentIndex, el_blend_space_e *outBlendSpace);

    /** @brief Resize a segment's own colour-stops vector (empty = inherit
     *         the base gradient at each perimeter sample). */
    EL_API el_result_e el_effect_set_segment_color_stop_count(el_effect_handle_t effect,
                                                              int32_t segmentIndex, int32_t count);
    EL_API el_result_e el_effect_get_segment_color_stop_count(el_effect_handle_t effect,
                                                              int32_t segmentIndex, int32_t *outCount);

    /** @brief Write one colour stop inside a segment's own stops list. */
    EL_API el_result_e el_effect_set_segment_color_stop(el_effect_handle_t effect,
                                                        int32_t segmentIndex, int32_t stopIndex,
                                                        float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_segment_color_stop(el_effect_handle_t effect,
                                                        int32_t segmentIndex, int32_t stopIndex,
                                                        float *outPosition, float *outR, float *outG, float *outB, float *outA);
    /** @brief Drop the segment's own colour stops (revert to inherit-base). */
    EL_API el_result_e el_effect_clear_segment_color_stops(el_effect_handle_t effect, int32_t segmentIndex);

    /** @} */

    /** @name Arcs (perimeter slices that are "on")
     *  Multiple arcs can coexist. Overlap resolves winner-take-all in the
     *  shader (largest mask*intensity owns the emission at each sample).
     *  Each arc's intensity is independent of @ref el_effect_set_intensity.
     *  See @c EdgeLighting::Arc for the full semantics.
     *  @{ */

    /** @brief Resize the arcs vector.
     *  @details Cap is @c NeonConfig::MAX_ARCS_CAP; values above that
     *           return @ref EL_ERROR_INVALID_PARAMETER. */
    EL_API el_result_e el_effect_set_arc_count(el_effect_handle_t effect, int32_t count);
    EL_API el_result_e el_effect_get_arc_count(el_effect_handle_t effect, int32_t *outCount);

    /** @brief Write one arc's scalars and blend space.
     *  @param start      Start of the arc on the perimeter, in [0, 1).
     *  @param length     Perimeter fraction lit; wraps over 0/1.
     *  @param intensity  Per-arc brightness multiplier.
     *  @param blendSpace Blend space for the arc's own colour stops
     *                    (ignored if the arc has none). */
    EL_API el_result_e el_effect_set_arc(el_effect_handle_t effect, int32_t index,
                                         float start, float length, float intensity, el_blend_space_e blendSpace);
    EL_API el_result_e el_effect_get_arc(el_effect_handle_t effect, int32_t index,
                                         float *outStart, float *outLength, float *outIntensity, el_blend_space_e *outBlendSpace);
    /** @brief Drop every arc. The neon layer goes fully dark until at least
     *         one arc is re-added or the effect's default @c {0, 1, 1} arc
     *         is re-created via @ref el_effect_set_arc_count. */
    EL_API el_result_e el_effect_clear_arcs(el_effect_handle_t effect);

    /** @brief Resize an arc's own colour-stops vector (empty = inherit the
     *         base gradient at each perimeter sample). */
    EL_API el_result_e el_effect_set_arc_color_stop_count(el_effect_handle_t effect,
                                                          int32_t arcIndex, int32_t count);
    EL_API el_result_e el_effect_get_arc_color_stop_count(el_effect_handle_t effect,
                                                          int32_t arcIndex, int32_t *outCount);

    /** @brief Write one colour stop inside an arc's own stops list. */
    EL_API el_result_e el_effect_set_arc_color_stop(el_effect_handle_t effect,
                                                    int32_t arcIndex, int32_t stopIndex,
                                                    float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_arc_color_stop(el_effect_handle_t effect,
                                                    int32_t arcIndex, int32_t stopIndex,
                                                    float *outPosition, float *outR, float *outG, float *outB, float *outA);
    /** @brief Drop the arc's own colour stops (revert to inherit-base). */
    EL_API el_result_e el_effect_clear_arc_color_stops(el_effect_handle_t effect, int32_t arcIndex);

    /** @} */

    /** @name Optimized (half-res) renderer
     *  A half-resolution neon variant that renders into a scaled FBO and
     *  bilinear-blits back to full res. Visual parameters are shared with
     *  the main neon layer; only the perf knobs live here.
     *  @{ */

    EL_API el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Set the internal FBO scale factor (0.5 = half, 0.25 = quarter). */
    EL_API el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t effect, float scale);
    EL_API el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t effect, float *outScale);

    /** @brief Set the per-fragment gather sample count (clamped to the
     *         shader's compile-time maximum). Lower = faster. */
    EL_API el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t effect, int32_t samples);
    EL_API el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t effect, int32_t *outSamples);

    /** @brief Set the precomputed gradient LUT size (power-of-two, 32-256). */
    EL_API el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t size);
    EL_API el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize);

    /** @brief Show the raw half-res FBO (nearest-upscale) instead of the
     *         final bilinear-blitted result. Diagnostic only. */
    EL_API el_result_e el_effect_set_optimized_show_half_res(el_effect_handle_t effect, el_bool_t show);
    EL_API el_result_e el_effect_get_optimized_show_half_res(el_effect_handle_t effect, el_bool_t *outShow);

    /** @} */

    /** @name Rain-on-glass droplets
     *  Fullscreen "wet window pane" that snapshots the framebuffer under it
     *  each frame, then repaints it with a frost blur and grid-hashed
     *  trickling droplets that refract the capture sharply. Register order in
     *  @ref el_effect_init places droplets last, so the neon layers show
     *  through the pane. Mirrors @c EdgeLighting::DropletsConfig.
     *  @{ */

    EL_API el_result_e el_effect_set_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_droplets_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Rain amount [0, 1] - density of all droplet layers. */
    EL_API el_result_e el_effect_set_droplets_amount(el_effect_handle_t effect, float amount);
    EL_API el_result_e el_effect_get_droplets_amount(el_effect_handle_t effect, float *outAmount);

    /** @brief Trickle speed multiplier (1 = reference pace; 0 freezes rain). */
    EL_API el_result_e el_effect_set_droplets_speed(el_effect_handle_t effect, float speed);
    EL_API el_result_e el_effect_get_droplets_speed(el_effect_handle_t effect, float *outSpeed);

    /** @brief Number of droplet lanes across the band (clamped to >= 1).
     *  @details 1 = drops as wide as the band, 2 = two lanes of half-width
     *           drops. Droplet size follows @ref el_effect_set_droplets_band_width,
     *           so drops fit the band at any thickness. */
    EL_API el_result_e el_effect_set_droplets_lanes(el_effect_handle_t effect, int lanes);
    EL_API el_result_e el_effect_get_droplets_lanes(el_effect_handle_t effect, int *outLanes);

    /** @brief Band thickness in pixels - the droplets' entire world.
     *  @details The side of the rect edge the band occupies comes from the
     *           neon glow side, not from here. */
    EL_API el_result_e el_effect_set_droplets_band_width(el_effect_handle_t effect, float bandWidth);
    EL_API el_result_e el_effect_get_droplets_band_width(el_effect_handle_t effect, float *outBandWidth);

    /** @brief Gap in pixels between the rect edge and the band's inner boundary. */
    EL_API el_result_e el_effect_set_droplets_band_offset(el_effect_handle_t effect, float bandOffset);
    EL_API el_result_e el_effect_get_droplets_band_offset(el_effect_handle_t effect, float *outBandOffset);

    /** @brief Drop colour multiplier (linear RGBA in [0, 1]; only @c rgb is
     *         read today, @c a is reserved). Tints the faint drop body only -
     *         the rim and specular highlights stay white. */
    EL_API el_result_e el_effect_set_droplets_tint(el_effect_handle_t effect,
                                                   float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_droplets_tint(el_effect_handle_t effect,
                                                   float *outR, float *outG, float *outB, float *outA);

    /** @} */

    /** @name Wireframe overlay
     *  Debug: 1 px line loop around the target rectangle.
     *  @{ */

    EL_API el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Set the wireframe line colour (linear RGBA in [0, 1]). */
    EL_API el_result_e el_effect_set_wireframe_color(el_effect_handle_t effect,
                                                     float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_wireframe_color(el_effect_handle_t effect,
                                                     float *outR, float *outG, float *outB, float *outA);

    /** @} */

    /* ======================================================================
     * Effect lifecycle
     * ==================================================================== */

    /** @name Lifecycle
     *  Create, initialise, tick, render, and destroy an effect. Every call
     *  in this section must run on the thread that owns the GL context.
     *  @{ */

    /** @brief Allocate a new effect handle with default staging config.
     *  @returns A fresh handle on success, @c NULL on allocation failure.
     *  @note The effect has no GL resources yet - call @ref el_effect_init
     *        once a GL context is current before calling render/update. */
    EL_API el_effect_handle_t el_effect_create(void);

    /** @brief Destroy an effect handle.
     *  @details Releases GL resources allocated by @ref el_effect_init and
     *           any C++ owned state. Passing @c NULL returns
     *           @ref EL_ERROR_INVALID_HANDLE. */
    EL_API el_result_e el_effect_destroy(el_effect_handle_t effect);

    /** @brief Initialise the effect's renderers under the current GL context.
     *  @returns @ref EL_ERROR_INIT_FAILED if a renderer fails to initialise
     *           (usually a shader compile / link error - see native log). */
    EL_API el_result_e el_effect_init(el_effect_handle_t effect);

    /** @brief Pull the effect's base config back into the effect's staging config.
     *  @details Snapshots the last-authored values (what @c SetConfig
     *           received) - does NOT include animation overlays. Use this
     *           to re-sync the staging config after external mutation
     *           (e.g. presets applied inside the effect). */
    EL_API el_result_e el_effect_capture(el_effect_handle_t effect);

    /** @brief Apply staging config then tick clock, animations, and renderers.
     *  @param deltaTime Seconds since the last @c update call. Values <= 0
     *                   still process animations but advance no time. */
    EL_API el_result_e el_effect_update(el_effect_handle_t effect, float deltaTime);

    /** @brief Draw every enabled renderer at the given viewport size. */
    EL_API el_result_e el_effect_render(el_effect_handle_t effect,
                                        int32_t viewportWidth, int32_t viewportHeight);

    /** @} */

    /** @name Clock control
     *  The effect's shared clock feeds all attached animations. Pausing it
     *  freezes every animation simultaneously without changing their
     *  individual states.
     *  @{ */

    /** @brief Start the effect's clock (default state after init).
     *  @details Attached animations advance in lockstep with
     *           @ref el_effect_update while the clock is playing. */
    EL_API el_result_e el_effect_clock_play(el_effect_handle_t effect);

    /** @brief Freeze the effect's clock.
     *  @details @ref el_effect_update still runs but the reported deltaTime
     *           to animations is 0 - the shader also stops advancing its
     *           internal time (hue rotation, etc.). */
    EL_API el_result_e el_effect_clock_pause(el_effect_handle_t effect);

    /** @brief Report whether the effect's clock is currently playing. */
    EL_API el_result_e el_effect_clock_is_playing(el_effect_handle_t effect,
                                                  el_bool_t *outPlaying);

    /** @} */

    /** @name Animation management
     *  Attach animation handles to the effect. Attach does NOT transfer
     *  ownership - the caller still owns the handle and must destroy it.
     *  Order of attach is the order of Apply per frame (later writes win).
     *  @{ */

    /** @brief Attach an animation. Duplicates and null handles are ignored. */
    EL_API el_result_e el_effect_attach_animation(el_effect_handle_t effect, el_animation_handle_t anim);
    /** @brief Detach an animation by identity. */
    EL_API el_result_e el_effect_detach_animation(el_effect_handle_t effect, el_animation_handle_t anim);
    /** @brief Detach every animation currently attached to the effect. */
    EL_API el_result_e el_effect_detach_all_animations(el_effect_handle_t effect);
    /** @brief Number of animations currently attached. */
    EL_API el_result_e el_effect_get_animation_count(el_effect_handle_t effect, int32_t *outCount);
    /** @brief Whether @p anim is currently attached to @p effect. */
    EL_API el_result_e el_effect_contains_animation(el_effect_handle_t effect,
                                                    el_animation_handle_t anim, el_bool_t *outContains);

    /** @} */

    /* ======================================================================
     * Animation lifecycle and factories
     * ==================================================================== */

    /** @name Preset factory
     *  Build a named preset animation. Prefer the dedicated factories below
     *  when you want to override the internal parameters.
     *  @{ */

    /** @brief Build an animation from an @ref el_animation_preset_e.
     *  @returns A fresh animation handle on success, @c NULL on failure. */
    EL_API el_animation_handle_t el_animation_create(el_animation_preset_e preset);

    /** @brief Destroy an animation handle. Detach first if attached. */
    EL_API el_result_e el_animation_destroy(el_animation_handle_t anim);

    /** @} */

    /** @name Neon animation factories
     *  Each factory returns a preconfigured animation ready to
     *  @ref el_effect_attach_animation and @ref el_animation_play.
     *  All handles must be destroyed with @ref el_animation_destroy.
     *  @{ */

    /** @brief Sinusoidal intensity pulse looping at @p duration seconds.
     *  @param duration     Period of one full pulse (seconds).
     *  @param minIntensity Trough intensity multiplier.
     *  @param maxIntensity Crest intensity multiplier. */
    EL_API el_animation_handle_t el_animation_create_intensity_pulse(float duration,
                                                                     float minIntensity, float maxIntensity);

    /** @brief Square-wave intensity strobe.
     *  @param duration     Period of one on+off cycle (seconds).
     *  @param offIntensity Intensity during the "off" half.
     *  @param onIntensity  Intensity during the "on" half. */
    EL_API el_animation_handle_t el_animation_create_intensity_strobe(float duration,
                                                                      float offIntensity, float onIntensity);

    /** @brief One-shot intensity fade-in.
     *  @param targetIntensity Final intensity value.
     *  @param duration        Length of the transition (seconds).
     *  @param easing          Easing curve applied to the interpolation. */
    EL_API el_animation_handle_t el_animation_create_intensity_fade_in(float targetIntensity,
                                                                       float duration, el_easing_e easing);

    /** @brief One-shot intensity fade-out from @p startIntensity to 0. */
    EL_API el_animation_handle_t el_animation_create_intensity_fade_out(float startIntensity,
                                                                        float duration, el_easing_e easing);

    /** @brief Sinusoidal glow-radius breath looping at @p duration seconds. */
    EL_API el_animation_handle_t el_animation_create_glow_radius_breath(float duration,
                                                                        float minRadius, float maxRadius);

    /** @brief Sinusoidal bloom-strength pulse looping at @p duration seconds. */
    EL_API el_animation_handle_t el_animation_create_bloom_pulse(float duration,
                                                                 float minStrength, float maxStrength);

    /** @brief Hue-rotation reversal: rate ramps from -peak to +peak (or v.v.)
     *         under a square-shaped envelope. */
    EL_API el_animation_handle_t el_animation_create_hue_rotation_reverse(float peakRate,
                                                                          float duration);

    /** @brief Hue-rotation reversal shaped by a smooth ease-in/ease-out curve. */
    EL_API el_animation_handle_t el_animation_create_hue_rotation_ease_reverse(float peakRate,
                                                                               float duration);

    /** @brief Single segment travelling once around the perimeter. */
    EL_API el_animation_handle_t el_animation_create_segment_travel(float duration,
                                                                    float length, float boost);

    /** @brief Single segment ping-ponging back and forth on the perimeter. */
    EL_API el_animation_handle_t el_animation_create_segment_bounce(float duration,
                                                                    float length, float boost);

    /** @brief One-shot outline tracer - arc[0] grows from 0 length to full. */
    EL_API el_animation_handle_t el_animation_create_outline_tracer(float duration,
                                                                    el_easing_e easing);

    /** @brief One-shot arc wipe - arc[0] grows from @p startPosition to
     *         @p endPosition, capped at @p maxLength. */
    EL_API el_animation_handle_t el_animation_create_arc_wipe(float duration,
                                                              float startPosition, float endPosition, float maxLength,
                                                              el_easing_e easing);

    /** @} */

    /** @name Animation control
     *  Play, pause, stop, reset, update, and apply. Each animation carries
     *  its own play state and elapsed accumulator - pausing one does not
     *  affect any other. Explicit Update / Apply are only needed for
     *  detached animations; attached animations are driven automatically by
     *  @ref el_effect_update.
     *  @{ */

    /** @brief Enter the PLAYING state.
     *  @details From STOPPED, elapsed is reset to 0 (a completed one-shot
     *           restarts from the beginning). From PAUSED, elapsed continues
     *           - there is no separate "resume". From PLAYING, this is a
     *           no-op. */
    EL_API el_result_e el_animation_play(el_animation_handle_t anim);

    /** @brief Freeze elapsed at its current value. Apply keeps writing it.
     *  @details Valid from PLAYING only; no-op otherwise. */
    EL_API el_result_e el_animation_pause(el_animation_handle_t anim);

    /** @brief Enter the STOPPED state.
     *  @details Symmetric with natural one-shot completion. The next Apply
     *           dispatches on the animation's end-action. */
    EL_API el_result_e el_animation_stop(el_animation_handle_t anim);

    /** @brief Zero elapsed and write the modulator's t=0 value into the
     *         effect's staging config. Does NOT change state. */
    EL_API el_result_e el_animation_reset(el_animation_handle_t anim, el_effect_handle_t effect);

    /** @brief Advance the animation by @p dt seconds (only if PLAYING). */
    EL_API el_result_e el_animation_update(el_animation_handle_t anim, float dt);

    /** @brief Write the animation's current value into the effect's staging
     *         config. Called automatically by @ref el_effect_update for
     *         attached animations. */
    EL_API el_result_e el_animation_apply(el_animation_handle_t anim, el_effect_handle_t effect);

    /** @} */

    /** @name Animation introspection and tuning
     *  Read/write play state, elapsed, progress, duration, speed, and end
     *  action. Setters take effect on the next frame.
     *  @{ */

    /** @brief Read the animation's current @ref el_animation_state_e. */
    EL_API el_result_e el_animation_get_state(el_animation_handle_t anim,
                                              el_animation_state_e *outState);

    /** @brief Read the animation's elapsed accumulator (seconds). */
    EL_API el_result_e el_animation_get_elapsed(el_animation_handle_t anim, float *outElapsed);
    /** @brief Directly overwrite the elapsed accumulator.
     *  @details Useful for scrubbing / testing. Does NOT change state or
     *           trigger completion callbacks - the next @ref el_animation_update
     *           tick will complete a one-shot whose elapsed has crossed
     *           its duration. Negative values are clamped to 0. */
    EL_API el_result_e el_animation_set_elapsed(el_animation_handle_t anim, float elapsed);

    /** @brief Normalised playback position (elapsed / duration) in [0, 1].
     *         Returns 0 when the animation's duration is 0 (its modulator
     *         owns its own periodicity). */
    EL_API el_result_e el_animation_get_progress(el_animation_handle_t anim, float *outProgress);
    /** @brief Set the normalised playback position; @p progress is clamped to
     *         [0, 1] before scaling. No-op when the animation's duration is 0.
     *         Same "does not change state" caveat as @ref el_animation_set_elapsed. */
    EL_API el_result_e el_animation_set_progress(el_animation_handle_t anim, float progress);

    /** @brief Read the animation's end-action policy. */
    EL_API el_result_e el_animation_get_end_action(el_animation_handle_t anim,
                                                   el_end_action_e *outAction);
    /** @brief Set the animation's end-action policy.
     *  @details Takes effect on the frame after stop - the currently running
     *           value is untouched. See @ref el_end_action_e for the menu. */
    EL_API el_result_e el_animation_set_end_action(el_animation_handle_t anim,
                                                   el_end_action_e action);

    /** @brief Snapshot the effect's staging config for later RESTORE.
     *  @details Must be called BEFORE @ref el_animation_play if the animation
     *           uses @ref EL_END_ACTION_RESTORE. Silently no-ops on
     *           animations whose subclass does not implement CaptureBaseline. */
    EL_API el_result_e el_animation_capture_baseline(el_animation_handle_t anim,
                                                     el_effect_handle_t effect);

    /** @brief Read the animation's playback mode (loop vs. one-shot). */
    EL_API el_result_e el_animation_get_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e *outMode);
    /** @brief Set the animation's playback mode. Does NOT touch duration. */
    EL_API el_result_e el_animation_set_playback_mode(el_animation_handle_t anim,
                                                      el_playback_mode_e mode);

    /** @brief Read the length of one animation cycle (seconds). */
    EL_API el_result_e el_animation_get_duration(el_animation_handle_t anim, float *outSeconds);
    /** @brief Set the length of one animation cycle (seconds).
     *  @details 0 means the internal modulator owns its own periodicity. */
    EL_API el_result_e el_animation_set_duration(el_animation_handle_t anim, float seconds);

    /** @brief Read the playback speed multiplier (1.0 = normal). */
    EL_API el_result_e el_animation_get_speed(el_animation_handle_t anim, float *outSpeed);
    /** @brief Set the playback speed multiplier.
     *  @details 1.0 = normal, 2.0 = double, 0.5 = half. 0 keeps the state
     *           PLAYING but freezes elapsed - use @ref el_animation_pause if
     *           you want the state to reflect it. Negative values are
     *           clamped to 0. */
    EL_API el_result_e el_animation_set_speed(el_animation_handle_t anim, float speed);

    /** @} */

    /* ======================================================================
     * Animation callbacks (C function pointer + user data)
     * ==================================================================== */

    /** @brief Fired once when a ONE_SHOT animation completes its cycle.
     *  @details Called on the effect thread from inside
     *           @ref el_effect_update. Never fires for LOOP animations. */
    typedef void (*el_animation_on_completed_callback)(void *userData);

    /** @brief Fired on every animation state transition.
     *  @param previous The state before the transition.
     *  @param current  The state after the transition.
     *  @param userData Opaque pointer supplied at registration. */
    typedef void (*el_animation_on_state_changed_callback)(el_animation_state_e previous,
                                                           el_animation_state_e current,
                                                           void *userData);

    /** @brief Set (or clear, with @c callback == NULL) the completion callback. */
    EL_API el_result_e el_animation_set_on_complete_callback(el_animation_handle_t anim,
                                                             el_animation_on_completed_callback callback, void *userData);
    /** @brief Set (or clear, with @c callback == NULL) the state-changed callback. */
    EL_API el_result_e el_animation_set_on_state_changed_callback(el_animation_handle_t anim,
                                                                  el_animation_on_state_changed_callback callback, void *userData);

    /* ======================================================================
     * Field-bound animations
     *
     * A field-bound animation carries a list of (field, modulator) bindings.
     * Each frame, every binding's modulator is evaluated with the animation's
     * elapsed and the result is written to the field. Use this for "one
     * animation, several fields moving together" - phase-locked pulses on
     * intensity + glow + bloom, coordinated segment motion, etc.
     * ==================================================================== */

    /** @name Field-bound animation construction
     *  @{ */

    /** @brief One-binding shortcut: build a field-bound animation that drives
     *         one scalar leaf with @p mod.
     *  @param field Scalar field to drive.
     *  @param mod   Modulator supplying the values. Passing @c NULL returns
     *               @c NULL.
     *  @returns A fresh animation handle on success, @c NULL on failure. */
    EL_API el_animation_handle_t el_animation_from_modulator(el_config_field_e field,
                                                             el_modulator_handle_t mod);

    /** @brief Empty field-bound animation. Extend with the @c el_animation_add_*
     *         family. Duration defaults to 0 (modulator owns periodicity)
     *         and mode to LOOP. */
    EL_API el_animation_handle_t el_animation_create_field_bound(void);

    /** @brief Add a scalar-leaf binding to a field-bound animation. */
    EL_API el_result_e el_animation_add_field(el_animation_handle_t anim,
                                              el_config_field_e field, el_modulator_handle_t mod);

    /** @brief Add a segment-field binding.
     *  @param index Segment slot. Auto-grows @c segmentBoosts at write time. */
    EL_API el_result_e el_animation_add_segment_field(el_animation_handle_t anim,
                                                      int32_t index, el_segment_field_e field, el_modulator_handle_t mod);

    /** @brief Add an arc-field binding.
     *  @param index Arc slot. Auto-grows @c arcs at write time. */
    EL_API el_result_e el_animation_add_arc_field(el_animation_handle_t anim,
                                                  int32_t index, el_arc_field_e field, el_modulator_handle_t mod);

    /** @brief Add a binding into one channel of one colour stop inside one arc.
     *  @param arcIndex   Arc slot.
     *  @param stopIndex  Stop slot within that arc.
     *  @param field      Which channel (position / R / G / B / A) to drive. */
    EL_API el_result_e el_animation_add_arc_stop_field(el_animation_handle_t anim,
                                                       int32_t arcIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                       el_modulator_handle_t mod);

    /** @brief Add a binding into one channel of one colour stop inside one segment. */
    EL_API el_result_e el_animation_add_segment_stop_field(el_animation_handle_t anim,
                                                           int32_t segmentIndex, int32_t stopIndex, el_color_stop_field_e field,
                                                           el_modulator_handle_t mod);

    /** @} */

    /* ======================================================================
     * Modulator factories
     *
     * A modulator is a pure @c time -> float function. Compose them with
     * Sequence / Multiplier / Adder / Remap to build arbitrarily complex
     * signals without touching the animation subclasses. Sharing ownership
     * across composites is safe - the composite retains a strong reference
     * internally, so destroying the outer handle keeps the composite alive.
     * ==================================================================== */

    /** @name Modulator factories
     *  Every factory returns @c NULL on failure. All non-null return values
     *  must eventually be destroyed with @ref el_modulator_destroy - the
     *  destroy call releases the outer handle; the underlying modulator is
     *  kept alive by any composite still referring to it.
     *  @{ */

    /** @brief Always returns @p value. */
    EL_API el_modulator_handle_t el_modulator_create_constant(float value);

    /** @brief Periodic waveform in [@p minValue, @p maxValue].
     *  @param frequency Cycles per second (Hz).
     *  @param minValue  Lower bound of the output range.
     *  @param maxValue  Upper bound of the output range.
     *  @param phase     Initial phase offset in cycles (typically [0, 1)).
     *  @param waveform  Shape of the oscillation. */
    EL_API el_modulator_handle_t el_modulator_create_oscillator(float frequency,
                                                                float minValue, float maxValue, float phase, el_waveform_e waveform);

    /** @brief One-shot (or looping) eased interpolation from @p from to @p to.
     *  @param duration Length of the transition (seconds).
     *  @param easing   Easing curve.
     *  @param loop     If true (non-zero), the transition repeats when time
     *                  exceeds duration. */
    EL_API el_modulator_handle_t el_modulator_create_ease(float from, float to,
                                                          float duration, el_easing_e easing, el_bool_t loop);

    /** @brief Empty sequence; append stages with @ref el_modulator_sequence_append.
     *  @param loop If true, the sequence loops back to its first stage once
     *              the last stage ends; otherwise the final value is held. */
    EL_API el_modulator_handle_t el_modulator_create_sequence(el_bool_t loop);

    /** @brief Append @p stage to the tail of @p seq, running for @p duration seconds.
     *  @details The sequence retains a strong reference to @p stage; the
     *           caller can destroy the outer stage handle immediately. */
    EL_API el_result_e el_modulator_sequence_append(el_modulator_handle_t seq,
                                                    el_modulator_handle_t stage, float duration);

    /** @brief Product modulator: evaluates as @p a(t) * @p b(t). */
    EL_API el_modulator_handle_t el_modulator_create_multiplier(el_modulator_handle_t a,
                                                                el_modulator_handle_t b);

    /** @brief Sum modulator: evaluates as @p a(t) + @p b(t). */
    EL_API el_modulator_handle_t el_modulator_create_adder(el_modulator_handle_t a,
                                                           el_modulator_handle_t b);

    /** @brief Rescale @p inner's output into [@p outMin, @p outMax].
     *  @details Handy for driving several fields from one canonical [0, 1]
     *           signal (e.g. an @c Ease) at different ranges. */
    EL_API el_modulator_handle_t el_modulator_create_remap(el_modulator_handle_t inner,
                                                           float outMin, float outMax);

    /** @brief Destroy a modulator handle.
     *  @details Releases the outer handle only. If the underlying modulator
     *           is still referenced by a composite or an animation binding
     *           it keeps living; otherwise it is freed here. */
    EL_API el_result_e el_modulator_destroy(el_modulator_handle_t mod);

    /** @brief Evaluate a modulator at time @p time and write the result to
     *         @p outValue. Useful for debugging / previewing signals. */
    EL_API el_result_e el_modulator_evaluate(el_modulator_handle_t mod,
                                             float time, float *outValue);

    /** @} */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EDGE_LIGHTING_CAPI_H_
