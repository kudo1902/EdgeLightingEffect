/**
 * @file el-effect.h
 * @brief Effect lifecycle, config setters/getters, clock control, and
 *        animation management for the EdgeLighting C API.
 */
#ifndef _EL_EFFECT_H_
#define _EL_EFFECT_H_

#include "el-types.h"

#ifdef __cplusplus
extern "C"
{
#endif

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
     *  Independent on/off switches for the neon, optimized, and debug
     *  layers. Any subset can be enabled at once - they composite additively.
     *  @{ */

    EL_API el_result_e el_effect_set_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_neon_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Draw the baked gradient LUT as a debug strip across the rectangle.
     *  @note Drawn by the debug layer, so it needs @ref EL_RENDERER_DEBUG in
     *        the registration mask (@ref EL_RENDERER_ALL includes it). Without
     *        that layer the flag is still stored and read back, but nothing is
     *        drawn. Declared here, alongside the neon layer it annotates. */
    EL_API el_result_e el_effect_set_show_gradient_lut(el_effect_handle_t effect, el_bool_t show);
    EL_API el_result_e el_effect_get_show_gradient_lut(el_effect_handle_t effect, el_bool_t *outShow);

    /** @brief Draw a coloured dot at each colour-stop position on the perimeter.
     *  @note Same debug layer as @ref el_effect_set_show_gradient_lut. */
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
     *  @param length   Width as a perimeter fraction (~2sigma of the Gaussian).
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

    /** @name Preserved segment boosts (id-addressed, override-proof)
     *  @details A **second, independent** pool of hotspots, addressed by a stable
     *  id instead of by array index. Its whole reason to exist: the index-based
     *  segment API above makes the array index the identity, so a
     *  @c el_effect_clear_segment_boosts, a shrinking @c el_effect_set_segment_boost_count,
     *  or an index-based animation can wipe or overwrite an entry another caller
     *  cared about. Preserved entries live outside that pool - none of those bulk
     *  operations touch them.
     *
     *  Usage: @ref el_effect_acquire_preserved_segment once to reserve an entry
     *  and get its id, then only ever mutate through that id. An id is stable
     *  for its entry's lifetime and unique among the entries that are live at
     *  any moment, so while you hold a live id a write/read by it can never land
     *  on another owner's entry. Ids are not permanently unique, though:
     *  @ref el_effect_release_preserved_segment can free an id for a later
     *  acquire to reuse, so stop using an id the moment you release it. The
     *  renderer composites the preserved and transient pools together (preserved
     *  take shader-slot priority), capped at @c NeonConfig::MAX_SEGMENT_BOOSTS_CAP
     *  total.
     *  @{ */

    /** @brief Reserve a preserved segment and return its id (stable for the
     *         entry's lifetime; may be reused by a later acquire once released).
     *  @param outId Receives the id (always >= 1) on success.
     *  @return @ref EL_ERROR_INVALID_PARAMETER if the preserved pool is already
     *          at @c NeonConfig::MAX_SEGMENT_BOOSTS_CAP. The new entry starts
     *          with default params (boost 0) - set them via
     *          @ref el_effect_set_preserved_segment. */
    EL_API el_result_e el_effect_acquire_preserved_segment(el_effect_handle_t effect, uint32_t *outId);

    /** @brief Write the scalar fields of the preserved entry owning @p id.
     *  @return @ref EL_ERROR_INVALID_PARAMETER if no preserved entry has @p id. */
    EL_API el_result_e el_effect_set_preserved_segment(el_effect_handle_t effect, uint32_t id,
                                                       float position, float length, float boost);
    /** @brief Read the scalar fields of the preserved entry owning @p id. */
    EL_API el_result_e el_effect_get_preserved_segment(el_effect_handle_t effect, uint32_t id,
                                                       float *outPosition, float *outLength, float *outBoost);

    /** @brief Remove the preserved entry owning @p id; others keep their ids.
     *  @return @ref EL_ERROR_INVALID_PARAMETER if no preserved entry has @p id. */
    EL_API el_result_e el_effect_release_preserved_segment(el_effect_handle_t effect, uint32_t id);

    /** @brief Number of live entries in the preserved pool. */
    EL_API el_result_e el_effect_get_preserved_segment_count(el_effect_handle_t effect, int32_t *outCount);

    /** @brief Drop every preserved entry (transient @c segmentBoosts untouched). */
    EL_API el_result_e el_effect_clear_preserved_segments(el_effect_handle_t effect);

    // Preserved segment gradient (blend space + colour stops), by id. By-id
    // mirror of the transient el_effect_set_segment_blend_space /
    // el_effect_set_segment_color_stop family above. A preserved entry with its
    // own stops shows that gradient across its span; with no stops it inherits
    // the base NeonConfig gradient. Every call resolves id to the owning entry
    // (immune to reindexing); an id with no live entry returns
    // EL_ERROR_INVALID_PARAMETER.

    /** @brief Set the blend space used for the preserved entry's own stops.
     *  @details Ignored at render time when the entry has no stops. */
    EL_API el_result_e el_effect_set_preserved_segment_blend_space(el_effect_handle_t effect,
                                                                   uint32_t id, el_blend_space_e blendSpace);
    EL_API el_result_e el_effect_get_preserved_segment_blend_space(el_effect_handle_t effect,
                                                                   uint32_t id, el_blend_space_e *outBlendSpace);

    /** @brief Resize the preserved entry's own colour-stops list (0 = inherit
     *         the base gradient). */
    EL_API el_result_e el_effect_set_preserved_segment_color_stop_count(el_effect_handle_t effect,
                                                                        uint32_t id, int32_t count);
    EL_API el_result_e el_effect_get_preserved_segment_color_stop_count(el_effect_handle_t effect,
                                                                        uint32_t id, int32_t *outCount);

    /** @brief Write one colour stop inside a preserved entry's stops list. */
    EL_API el_result_e el_effect_set_preserved_segment_color_stop(el_effect_handle_t effect,
                                                                  uint32_t id, int32_t stopIndex,
                                                                  float position, float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_preserved_segment_color_stop(el_effect_handle_t effect,
                                                                  uint32_t id, int32_t stopIndex,
                                                                  float *outPosition, float *outR, float *outG, float *outB, float *outA);

    /** @brief Drop the preserved entry's own colour stops (revert to inherit-base). */
    EL_API el_result_e el_effect_clear_preserved_segment_color_stops(el_effect_handle_t effect, uint32_t id);

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

    /** @name Neon resolution scale (formerly the "optimized" renderer)
     *
     *  DEPRECATED NAMES, LIVE FUNCTIONS. There is no separate half-res
     *  renderer any longer: the neon layer draws at a resolution scale, where
     *  1.0 is the full-resolution path (straight onto the target, no offscreen
     *  buffer, no blit) and anything below it renders into a scaled buffer and
     *  bilinear-blits back. These calls are kept, signatures unchanged, and
     *  now read and write that one layer's settings. Prefer them for the perf
     *  knobs; the "optimized" in each name is history.
     *
     *  The three knob pairs map straight onto the merged fields. The
     *  enable pair is the one asymmetry, because the flag it used to own no
     *  longer exists:
     *
     *  - @c set(..., true)  enables the neon layer AND, if it is currently at
     *                       full resolution, moves it to 0.5 (the scale the
     *                       old half-res renderer defaulted to). A scale
     *                       already below 1.0 is left as it is.
     *  - @c set(..., false) returns the layer to full resolution. It does NOT
     *                       disable the neon: under the old ABI this call
     *                       silenced one of two renderers, so clearing the
     *                       enable here would blank the effect for a host that
     *                       is merely switching paths. Use
     *                       @ref el_effect_set_neon_renderer_enabled to turn
     *                       the layer off.
     *  - @c get(...)        reports true when the neon layer is enabled and
     *                       below full resolution.
     *  @{ */

    EL_API el_result_e el_effect_set_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_optimized_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Set the neon layer's resolution scale (1.0 = full res and no
     *         offscreen buffer; 0.5 = half, 0.25 = quarter). Clamped to
     *         (0, 1] at draw time - values above 1.0 do not supersample. */
    EL_API el_result_e el_effect_set_optimized_resolution_scale(el_effect_handle_t effect, float scale);
    EL_API el_result_e el_effect_get_optimized_resolution_scale(el_effect_handle_t effect, float *outScale);

    /** @brief Set the per-fragment gather sample count (clamped to the
     *         shader's compile-time maximum). Lower = faster. */
    EL_API el_result_e el_effect_set_optimized_num_samples(el_effect_handle_t effect, int32_t samples);
    EL_API el_result_e el_effect_get_optimized_num_samples(el_effect_handle_t effect, int32_t *outSamples);

    /** @brief Set the precomputed gradient LUT size (power-of-two, 32-256). */
    EL_API el_result_e el_effect_set_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t size);
    EL_API el_result_e el_effect_get_optimized_gradient_lut_size(el_effect_handle_t effect, int32_t *outSize);

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

    /** @name Lens flare
     *  Sun + hex-aperture lens flare drawn as a single fullscreen pass. The
     *  sun rides the rect perimeter (same parameter space as segments / arcs)
     *  so it moves with the geometry.
     *  @{ */

    EL_API el_result_e el_effect_set_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Sun position as a perimeter progress in [0, 1).
     *  @details 0 = top-left corner; winding follows the geometry's winding. */
    EL_API el_result_e el_effect_set_lens_flare_perimeter_position(el_effect_handle_t effect, float position);
    EL_API el_result_e el_effect_get_lens_flare_perimeter_position(el_effect_handle_t effect, float *outPosition);

    /** @brief Signed offset in pixels along the edge normal at the sun's
     *         perimeter position. Positive = outward (away from rect centre),
     *         negative = inward. */
    EL_API el_result_e el_effect_set_lens_flare_perimeter_offset(el_effect_handle_t effect, float offset);
    EL_API el_result_e el_effect_get_lens_flare_perimeter_offset(el_effect_handle_t effect, float *outOffset);

    /** @brief Size scale for the sun core + rays (1.0 = reference look). */
    EL_API el_result_e el_effect_set_lens_flare_size(el_effect_handle_t effect, float size);
    EL_API el_result_e el_effect_get_lens_flare_size(el_effect_handle_t effect, float *outSize);

    /** @brief Sun tint (linear RGBA, HDR allowed - the reference uses (1.4, 1.2, 1.0)). */
    EL_API el_result_e el_effect_set_lens_flare_color(el_effect_handle_t effect,
                                                      float r, float g, float b, float a);
    EL_API el_result_e el_effect_get_lens_flare_color(el_effect_handle_t effect,
                                                      float *outR, float *outG, float *outB, float *outA);

    /** @brief Master brightness multiplier. */
    EL_API el_result_e el_effect_set_lens_flare_intensity(el_effect_handle_t effect, float intensity);
    EL_API el_result_e el_effect_get_lens_flare_intensity(el_effect_handle_t effect, float *outIntensity);

    /** @brief Ghost / hex-aperture strength (0 = disc only, 1 = reference look). */
    EL_API el_result_e el_effect_set_lens_flare_spread(el_effect_handle_t effect, float spread);
    EL_API el_result_e el_effect_get_lens_flare_spread(el_effect_handle_t effect, float *outSpread);

    /** @brief Stretch of the ghost placement along the sun-to-centre axis
     *         (1.0 = reference spacing).
     *  @details Reference spacing scales with the sun-to-centre distance, so
     *           ghosts crowd together when the sun sits near a screen edge
     *           (e.g. top-centre) and spread out at a corner. Raise this to
     *           push them apart. Placement only - colour and size are
     *           unchanged. */
    EL_API el_result_e el_effect_set_lens_flare_ghost_spacing(el_effect_handle_t effect, float ghostSpacing);
    EL_API el_result_e el_effect_get_lens_flare_ghost_spacing(el_effect_handle_t effect, float *outGhostSpacing);

    /** @brief Uniform ghost size / falloff exponent shared by every ghost
     *         (default 2.2 = reference average).
     *  @details The reference gave each ghost a random size; this fixes them
     *           all to one value so they read as the same size. Larger =
     *           bigger, softer ghosts. */
    EL_API el_result_e el_effect_set_lens_flare_ghost_size(el_effect_handle_t effect, float ghostSize);
    EL_API el_result_e el_effect_get_lens_flare_ghost_size(el_effect_handle_t effect, float *outGhostSize);

    /** @brief Signed shift of the ghost cluster along the sun-to-centre axis
     *         (0.0 = reference, negative pulls toward the sun / border).
     *  @details dist 0 is the screen centre and dist ~ -1 sits on the sun, so
     *           negative values move the ghosts off centre and up against the
     *           border edge where the sun rides. Default is -1.5. */
    EL_API el_result_e el_effect_set_lens_flare_ghost_offset(el_effect_handle_t effect, float ghostOffset);
    EL_API el_result_e el_effect_get_lens_flare_ghost_offset(el_effect_handle_t effect, float *outGhostOffset);

    /** @brief Colour the ghosts lean toward when the tint amount > 0 (linear RGB). */
    EL_API el_result_e el_effect_set_lens_flare_ghost_color(el_effect_handle_t effect,
                                                            float r, float g, float b);
    EL_API el_result_e el_effect_get_lens_flare_ghost_color(el_effect_handle_t effect,
                                                            float *outR, float *outG, float *outB);

    /** @brief Blend from the procedural ghost rainbow (0.0) to a single ghost
     *         colour for every ghost (1.0). Hue only - brightness unaffected. */
    EL_API el_result_e el_effect_set_lens_flare_ghost_tint(el_effect_handle_t effect, float ghostTint);
    EL_API el_result_e el_effect_get_lens_flare_ghost_tint(el_effect_handle_t effect, float *outGhostTint);

    /** @brief Ghost convergence / reference point in normalised screen coords
     *         (0..1, origin top-left, y-down); (0.5, 0.5) = screen centre.
     *  @details The ghosts pivot about this point and their sun-to-centre axis
     *           runs through it instead of the screen centre. The sun's own
     *           rays and vignette are unaffected. */
    EL_API el_result_e el_effect_set_lens_flare_flare_center(el_effect_handle_t effect, float x, float y);
    EL_API el_result_e el_effect_get_lens_flare_flare_center(el_effect_handle_t effect, float *outX, float *outY);

    /** @brief Angular density of the ray pattern in [0, 1].
     *  @details 0 = a single broad ray, 1 = the densest sunburst. The value
     *           is quantised to an integer slot count internally so the
     *           pattern closes cleanly at the 2 PI wrap. NOT a literal count
     *           of visible rays - per-ray length randomisation hides some
     *           slots as short stubs. */
    EL_API el_result_e el_effect_set_lens_flare_ray_density(el_effect_handle_t effect, float rayDensity);
    EL_API el_result_e el_effect_get_lens_flare_ray_density(el_effect_handle_t effect, float *outRayDensity);

    /** @brief Sun / ray rotation rate in revolutions per second (0 = static).
     *  @details Ghost groups stay anchored on the sun-to-centre axis; only
     *           the sun disc and rays spin. */
    EL_API el_result_e el_effect_set_lens_flare_rotation_rate(el_effect_handle_t effect, float rate);
    EL_API el_result_e el_effect_get_lens_flare_rotation_rate(el_effect_handle_t effect, float *outRate);

    /** @} */

    /** @name Optimized (half-res) lens flare
     *  A half-resolution lens-flare variant that renders the flare into a
     *  scaled FBO and bilinear-blits back to full res. All visual parameters
     *  are shared with the main lens-flare layer (set them via the
     *  @c el_effect_set_lens_flare_* functions above); only the perf knobs
     *  live here. Enabling this and the full-res lens flare at the same time
     *  draws the flare twice.
     *  @{ */

    EL_API el_result_e el_effect_set_optimized_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_optimized_lens_flare_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Set the internal FBO scale factor (0.5 = half, 0.25 = quarter). */
    EL_API el_result_e el_effect_set_optimized_lens_flare_resolution_scale(el_effect_handle_t effect, float scale);
    EL_API el_result_e el_effect_get_optimized_lens_flare_resolution_scale(el_effect_handle_t effect, float *outScale);

    /** @} */

    /** @name Wireframe overlay
     *  Debug: 1 px line loop around the target rectangle.
     *  @{ */

    EL_API el_result_e el_effect_set_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_wireframe_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Set the wireframe line colour (linear RGBA in [0, 1]).
     *  @note The box is an overlay of the debug layer now, not a renderer of
     *        its own, so it needs @ref EL_RENDERER_DEBUG (or the deprecated
     *        @ref EL_RENDERER_WIREFRAME alias) in the registration mask, and
     *        it is additionally gated by @ref el_effect_set_debug_enabled if
     *        that is turned off. It now draws OVER the neon rather than under
     *        it. These functions write @c DebugConfig. */
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

    /** @brief Initialise every renderer layer under the current GL context.
     *  @details Convenience wrapper for @ref el_effect_init_with_renderers with
     *           @ref EL_RENDERER_ALL - registers the full stack (neon, debug,
     *           neon, optimized, droplets, lens flare).
     *  @returns @ref EL_ERROR_INIT_FAILED if a renderer fails to initialise
     *           (usually a shader compile / link error - see native log). */
    EL_API el_result_e el_effect_init(el_effect_handle_t effect);

    /** @brief Initialise a selected subset of renderer layers under the current GL context.
     *  @param effect       Effect handle.
     *  @param rendererMask OR of @ref el_renderer_flags_e bits naming the
     *                      layers to register. @ref EL_RENDERER_NONE registers
     *                      nothing (render becomes a no-op until layers are
     *                      added C++-side); @ref EL_RENDERER_ALL is the full
     *                      stack.
     *  @details Layers are always registered in the fixed compositing order
     *           regardless of the mask, so droplets still refract the neon
     *           beneath them and the lens flare still sits on top. An omitted
     *           layer is never constructed and pays no GL cost. Prefer this
     *           over @ref el_effect_init when a host only needs some layers
     *           (e.g. neon alone) and wants to skip the others' shader compiles.
     *  @returns @ref EL_ERROR_INIT_FAILED if an included renderer fails to
     *           initialise (usually a shader compile / link error - see native
     *           log). */
    EL_API el_result_e el_effect_init_with_renderers(el_effect_handle_t effect, uint32_t rendererMask);

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

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_EFFECT_H_
