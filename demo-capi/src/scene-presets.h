#ifndef _EDGE_LIGHTING_CAPI_DEMO_SCENE_PRESETS_H_
#define _EDGE_LIGHTING_CAPI_DEMO_SCENE_PRESETS_H_

#include "edge-lighting-capi.h"
#include <algorithm>

namespace EdgeLightingCapiDemo
{
    /// Whole-scene presets, mirroring demo/src/scene-presets.h through the C
    /// ABI only. Every constant below is a copy of one there: keep the two in
    /// step by hand, exactly as the animation preset tables are.
    ///
    /// This file is also the useful half of the fork. The C++ preset writes
    /// struct fields; this one has to reach the same scene through the
    /// published setters, which is the check that the flat surface can drive
    /// a whole rig and not just one slider at a time.

    namespace PictureLight
    {
        constexpr float REF_BAR_WIDTH = 580.0f;

        constexpr float BAR_WIDTH_FRAC = 0.45f;
        constexpr float TUBE_DIA_FRAC = 0.021f;
        constexpr float BAR_TOP_FRAC = 0.167f;

        /// Height of the rect the neon traces. The tube is a ROD: this
        /// collapses the rect to a single straight run whose top and bottom
        /// perimeter edges coincide, and el_effect_set_line_width gives the
        /// rod its diameter. A rect with real height separates into a hollow
        /// capsule outline - two filaments with a dark channel between them -
        /// which is a loop of neon tubing, not an LED bar. Not scaled by the
        /// bar, and deliberately not 0; see the C++ copy for the measurements.
        constexpr float ROD_RECT_HEIGHT = 1.0f;

        /// Flatter than the default: the rod wants a defined edge. Below ~2
        /// it blends into its own halo, above ~3 it reads as a painted stripe.
        constexpr float FILAMENT_FALLOFF = 2.5f;
        constexpr float NEON_INTENSITY = 0.80f;
        constexpr float GLOW_RADIUS = 15.0f;
        constexpr float BLOOM_STRENGTH = 0.10f;

        /// The library's SPOT_MAX_LIGHTS. This file cannot see
        /// spotlight-tuning.h - that is the point of the fork - so the
        /// ceiling is duplicated here and kept in step by hand, the same way
        /// buildSpotlightSection duplicates it.
        constexpr int LAMP_COUNT = 8;

        constexpr float BEAM_ANGLE = 30.0f;
        constexpr float THROW_LENGTH = 280.0f;
        constexpr float LAMP_INTENSITY = 0.70f;
        constexpr float COLOR_TEMP = 2900.0f;

        /// Aperture half-width as a fraction of the lamp SPACING - the ratio
        /// that decides whether the row reads as one pool or a string of
        /// beads. Measured at this value, the interior ripple between "under
        /// a lamp" and "between two lamps" is at most 0.9% of full scale.
        constexpr float APERTURE_PER_SPACING = 0.61f;

        constexpr float SOFTNESS = 1.0f;

        constexpr float TUBE_R = 1.00f;
        constexpr float TUBE_G = 0.72f;
        constexpr float TUBE_B = 0.45f;
    } // namespace PictureLight

    /// Imitate a linear LED picture light: a warm bar mounted high on the
    /// wall, throwing a broad short wash down over whatever hangs below.
    ///
    /// The tube is the neon layer, with @c geometry set to the bar's AXIS (a
    /// straight run @c ROD_RECT_HEIGHT tall) and @c line_width giving that run
    /// the tube's diameter, so the filament is a solid rod with round caps.
    /// The wash is a ROW of spotlights rather than one, because a picture
    /// light is a LINE source and a single cone reads as a triangle.
    ///
    /// Also turns off droplets, the lens flare and the debug overlays: this
    /// fixture uses none of them, and a wireframe box around a 12 px tube is
    /// just noise. Nothing in this library occludes light, so there is no
    /// shadow under whatever the wash falls on.
    ///
    /// @param viewportW,viewportH  The size the effect is rendered at, which
    ///                             is the app-coordinate space both
    ///                             el_effect_set_geometry and
    ///                             el_effect_set_spotlight_placement use.
    inline void ApplyPictureLight(el_effect_handle_t effect,
                                  float viewportW, float viewportH)
    {
        namespace PL = PictureLight;

        const float barW = std::max(viewportW * PL::BAR_WIDTH_FRAC, 64.0f);
        const float tubeDia = std::max(barW * PL::TUBE_DIA_FRAC, 4.0f);
        const float barX = (viewportW - barW) * 0.5f;
        const float barY = viewportH * PL::BAR_TOP_FRAC;

        // Px-valued numbers were tuned at REF_BAR_WIDTH; scale by the bar so
        // the wash stays in proportion to the fixture at any viewport size.
        const float s = barW / PL::REF_BAR_WIDTH;

        // --- Geometry: the rect is the tube's AXIS, not its outline ---
        // Offset y so the rod's axis lands where a tubeDia-tall bar's centre
        // line would, keeping the rod at barY .. barY + tubeDia and the lamp
        // row below it unchanged.
        el_effect_set_geometry(effect, barW, PL::ROD_RECT_HEIGHT, barX,
                               barY + (tubeDia - PL::ROD_RECT_HEIGHT) * 0.5f,
                               PL::ROD_RECT_HEIGHT * 0.5f);

        // --- The tube ---
        el_effect_set_neon_renderer_enabled(effect, 1);
        el_effect_set_opaque_mode(effect, EL_OPAQUE_MODE_NONE);
        // The rod's diameter. Already proportional to the bar, so it does
        // NOT take the s scale a second time.
        el_effect_set_line_width(effect, tubeDia);
        el_effect_set_filament_falloff(effect, PL::FILAMENT_FALLOFF);
        el_effect_set_intensity(effect, PL::NEON_INTENSITY);
        el_effect_set_glow_radius(effect, PL::GLOW_RADIUS * s);
        el_effect_set_bloom_strength(effect, PL::BLOOM_STRENGTH);
        el_effect_set_glow_side(effect, EL_GLOW_SIDE_BOTH);
        el_effect_set_inside_cutoff(effect, 0, 0.0f, 0.0f);
        el_effect_set_outside_cutoff(effect, 0, 0.0f, 0.0f);
        // A lamp does not cycle hue, and the default rate is 0.5.
        el_effect_set_hue_rotation_rate(effect, 0.0f);

        el_effect_set_color_stop_count(effect, 1);
        el_effect_set_color_stop(effect, 0, 0.0f,
                                 PL::TUBE_R, PL::TUBE_G, PL::TUBE_B, 1.0f);

        // Full perimeter, evenly lit: an arc slice or a travelling segment
        // left over from another scene would put a bright spot on the tube.
        el_effect_set_arc_count(effect, 1);
        el_effect_set_arc(effect, 0, 0.0f, 1.0f, 1.0f, EL_BLEND_SPACE_RGB);
        el_effect_clear_segment_boosts(effect);

        // --- The wash ---
        el_effect_set_spotlight_renderer_enabled(effect, 1);
        el_effect_set_spotlight_resolution_scale(effect, 1.0f);
        el_effect_set_spotlight_count(effect, PL::LAMP_COUNT);

        const float spacing = barW / static_cast<float>(PL::LAMP_COUNT);
        for (int i = 0; i < PL::LAMP_COUNT; i++)
        {
            // Half-spacing in from each end, so the row is centred on the bar
            // and no lamp sits on a cap.
            const float x = barX + (static_cast<float>(i) + 0.5f) * spacing;
            el_effect_set_spotlight_placement(effect, i, x, barY + tubeDia, 90.0f);
            el_effect_set_spotlight_beam(effect, i, PL::BEAM_ANGLE,
                                         PL::THROW_LENGTH * s,
                                         spacing * PL::APERTURE_PER_SPACING,
                                         PL::SOFTNESS);
            // Bloom 0: the neon halo already owns the near field, and one
            // bloom per lamp would read as a string of beads. The radius is
            // unused while bloom is 0 and stays at the C++ default.
            el_effect_set_spotlight_look(effect, i, PL::LAMP_INTENSITY,
                                         0.0f, 20.0f, PL::COLOR_TEMP);
            el_effect_set_spotlight_tint(effect, i, 1.0f, 1.0f, 1.0f);
            el_effect_set_spotlight_enabled(effect, i, 1);
        }

        // --- Layers this fixture does not use ---
        el_effect_set_droplets_renderer_enabled(effect, 0);
        el_effect_set_lens_flare_renderer_enabled(effect, 0);
        el_effect_set_debug_show_wireframe(effect, 0);
        el_effect_set_debug_show_gradient_lut(effect, 0);
        el_effect_set_debug_show_color_stops(effect, 0);
    }

} // namespace EdgeLightingCapiDemo

#endif
