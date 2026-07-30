/**
 * @file el-types.h
 * @brief Shared scalar types, enums, and opaque handles for the EdgeLighting C API.
 */
#ifndef _EL_TYPES_H_
#define _EL_TYPES_H_

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
        EL_FIELD_NEON_HUE_ROTATION_RATE = 6,  /**< @c NeonConfig::hueRotationRate */

        /* Lens flare scalar leaves (LensFlareConfig). Drive whichever lens
         * flare renderer is enabled (full-res or half-res optimized). Numbered
         * from 100 to leave a gap after the neon block, so future neon fields
         * can append at 7+ without disturbing these values. */
        EL_FIELD_LENS_FLARE_PERIMETER_POSITION = 100, /**< @c LensFlareConfig::perimeterPosition */
        EL_FIELD_LENS_FLARE_PERIMETER_OFFSET = 101,   /**< @c LensFlareConfig::perimeterOffset */
        EL_FIELD_LENS_FLARE_SIZE = 102,               /**< @c LensFlareConfig::size */
        EL_FIELD_LENS_FLARE_INTENSITY = 103,          /**< @c LensFlareConfig::intensity */
        EL_FIELD_LENS_FLARE_SPREAD = 104,             /**< @c LensFlareConfig::spread */
        EL_FIELD_LENS_FLARE_GHOST_SPACING = 105,      /**< @c LensFlareConfig::ghostSpacing */
        EL_FIELD_LENS_FLARE_GHOST_SIZE = 106,         /**< @c LensFlareConfig::ghostSize */
        EL_FIELD_LENS_FLARE_GHOST_OFFSET = 107,       /**< @c LensFlareConfig::ghostOffset */
        EL_FIELD_LENS_FLARE_GHOST_TINT = 108,         /**< @c LensFlareConfig::ghostTint */
        EL_FIELD_LENS_FLARE_RAY_DENSITY = 109,        /**< @c LensFlareConfig::rayDensity */
        EL_FIELD_LENS_FLARE_ROTATION_RATE = 110       /**< @c LensFlareConfig::rotationRate */
    } el_config_field_e;

    /** @brief Scalar inside a @c NeonConfig::segmentBoosts entry.
     *  @details Mirrors @c EdgeLighting::SegmentField. Paired with an index
     *           at bind time via @ref el_animation_add_segment_field. */
    typedef enum el_segment_field_e
    {
        EL_SEGMENT_FIELD_POSITION = 0, /**< Centre of the segment on the perimeter [0,1). */
        EL_SEGMENT_FIELD_LENGTH = 1,   /**< Segment width as a perimeter fraction (~2sigma). */
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

    /** @brief Bitmask selecting which renderer layers @ref el_effect_init_with_renderers
     *         registers on the effect.
     *  @details OR the flags for the layers you want. Registration always
     *           happens in the fixed compositing order (wireframe, neon,
     *           optimized, droplets, lens flare) regardless of how the bits
     *           are combined - the mask only decides inclusion, not order.
     *           A layer that is not included is never constructed, so it pays
     *           no GL cost (no shader compile, no FBO allocation); its
     *           @c el_effect_set_*_renderer_enabled flag still writes to the
     *           staging config but has no visual effect. */
    typedef enum el_renderer_flags_e
    {
        EL_RENDERER_NONE = 0,                /**< Register no renderers. */
        EL_RENDERER_WIREFRAME = 1 << 0,      /**< 1 px debug line loop. */
        EL_RENDERER_NEON = 1 << 1,           /**< Single-pass neon stroke. */
        EL_RENDERER_NEON_OPTIMIZED = 1 << 2, /**< Half-res neon variant. */
        EL_RENDERER_DROPLETS = 1 << 3,       /**< Rain-on-glass droplets. */
        EL_RENDERER_LENS_FLARE = 1 << 4,     /**< Sun + hex-aperture lens flare. */
        EL_RENDERER_ALL = 0x7FFFFFFF         /**< Every renderer (what @ref el_effect_init uses). */
    } el_renderer_flags_e;

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

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_TYPES_H_
