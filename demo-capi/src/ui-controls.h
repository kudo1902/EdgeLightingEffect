#ifndef _EDGE_LIGHTING_CAPI_DEMO_UI_CONTROLS_H_
#define _EDGE_LIGHTING_CAPI_DEMO_UI_CONTROLS_H_

#include "edge-lighting-capi.h"

#include <cstdio>

namespace EdgeLightingCapiDemo
{
    inline void PrintControls()
    {
        std::printf("\n=========================================\n");
        std::printf("     Neon Edge Lighting (capi demo)\n");
        std::printf("=========================================\n");
        std::printf("Controls:\n");
        std::printf("  [R / F]        - Inc / Dec Neon Line Width\n");
        std::printf("  [I / O]        - Inc / Dec Neon Intensity\n");
        std::printf("  [[ / ]]        - Dec / Inc Neon Glow Radius\n");
        std::printf("  [P / L]        - Inc / Dec Neon Hue Sweep Speed\n");
        std::printf("  [N]            - Toggle Neon\n");
        std::printf("  [G]            - Toggle Wireframe Bounding Box\n");
        std::printf("  [Shift+O]      - Toggle Optimized Neon\n");
        std::printf("  [W]            - Toggle Winding (CW / CCW)\n");
        std::printf("  [SPACE]        - Pause / Resume Clock\n");
        std::printf("  [ESC]          - Exit\n");
        std::printf("=========================================\n\n");
    }

    inline void PrintCurrentStatus(el_effect_handle_t effect)
    {
        if (!effect)
        {
            return;
        }
        float lineW = 0, intensity = 0, glowR = 0, bloom = 0, hueRate = 0;
        el_effect_get_line_width(effect, &lineW);
        el_effect_get_intensity(effect, &intensity);
        el_effect_get_glow_radius(effect, &glowR);
        el_effect_get_bloom_strength(effect, &bloom);
        el_effect_get_hue_rotation_rate(effect, &hueRate);
        el_winding_e winding = EL_WINDING_CLOCKWISE;
        el_effect_get_winding(effect, &winding);
        el_bool_t neonOn = 0, wireOn = 0, playing = 0;
        el_effect_get_neon_renderer_enabled(effect, &neonOn);
        el_effect_get_wireframe_renderer_enabled(effect, &wireOn);
        el_effect_clock_is_playing(effect, &playing);
        el_blend_space_e blend = EL_BLEND_SPACE_RGB;
        el_effect_get_blend_space(effect, &blend);

        const char *blendNames[] = {"RGB", "HSV", "HSL"};
        std::printf("\r[Neon] LW: %.0f | En: %s | Int: %.2f | GR: %.0f | Bl: %.2f | HR: %.2f | %s | %s | %s | Wire: %s      ",
                    lineW, neonOn ? "ON " : "OFF", intensity, glowR, bloom, hueRate,
                    winding == EL_WINDING_CLOCKWISE ? "CW " : "CCW",
                    blendNames[blend],
                    playing ? "PLAY" : "PAUS",
                    wireOn ? "ON " : "OFF");
        std::fflush(stdout);
    }
} // namespace EdgeLightingCapiDemo

#endif // _EDGE_LIGHTING_CAPI_DEMO_UI_CONTROLS_H_
