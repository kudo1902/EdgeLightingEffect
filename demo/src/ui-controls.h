#ifndef _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
#define _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_

#include "core/edge-lighting.h"
#include <iostream>
#include <iomanip>

namespace EdgeLightingDemo
{

    inline void PrintControls()
    {
        std::cout << "\n=========================================\n";
        std::cout << "        Neon Edge Lighting Demo\n";
        std::cout << "=========================================\n";
        std::cout << "Controls:\n";
        std::cout << "  [R / F]        - Inc / Dec Neon Line Width\n";
        std::cout << "  [I / O]        - Inc / Dec Neon Intensity\n";
        std::cout << "  [[ / ]]        - Dec / Inc Neon Glow Radius\n";
        std::cout << "  [P / L]        - Inc / Dec Neon Sweep Speed\n";
        std::cout << "  [N]            - Toggle Single-pass Neon\n";
        std::cout << "  [Shift+N]      - Toggle Multi-pass Neon\n";
        std::cout << "  [G]            - Toggle Wireframe Bounding Box\n";
        std::cout << "  [W]            - Toggle Winding (CW / CCW)\n";
        std::cout << "  [SPACE]        - Pause / Resume Animation\n";
        std::cout << "  [ESC]          - Exit\n";
        std::cout << "=========================================\n\n";
    }

    inline void PrintCurrentConfig(const EdgeLighting::Config &config, bool isPlaying)
    {
        std::string neonStr = config.neon.enable ? "ON " : "OFF";
        std::string mpNeonStr = config.multipassNeon.enable ? "ON " : "OFF";
        std::string windingStr = (config.geometry.winding == EdgeLighting::Winding::CLOCKWISE) ? "CW" : "CCW";
        std::string blendStr = (config.neon.blendSpace == EdgeLighting::BlendSpace::HSV) ? "HSV" : "RGB";

        std::cout << "\r[Neon] LineW: " << std::setw(3) << config.neon.lineWidth
                  << " | En: " << neonStr
                  << " | Int: " << std::fixed << std::setprecision(2) << std::setw(4) << config.neon.intensity
                  << " | GlowR: " << std::setprecision(0) << std::setw(3) << config.neon.glowRadius
                  << " | Bloom: " << std::setprecision(2) << std::setw(4) << config.neon.bloomStrength
                  << " | Spd: " << std::setprecision(2) << std::setw(4) << config.neon.sweepSpeed
                  << " | MP: " << mpNeonStr
                  << " | " << windingStr
                  << " | " << blendStr
                  << " | Anim: " << (isPlaying ? "PLAY" : "PAUS")
                  << " | Wire: " << (config.wireframe.enable ? "ON " : "OFF")
                  << "      " << std::flush;
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
