#ifndef _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
#define _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_

#include "core/edge-lighting.h"
#include <iostream>
#include <iomanip>

namespace EdgeLightingDemo
{

    inline void PrintControls()
    {
        std::cout << "\n=============================================\n";
        std::cout << "      Animated Edge Lighting Effect Demo\n";
        std::cout << "=============================================\n";
        std::cout << "Controls:\n";
        std::cout << "  [1]            - Color Mode: Static Color\n";
        std::cout << "  [2]            - Color Mode: Gradient Color\n";
        std::cout << "  [3]            - Color Mode: Ambient Rainbow\n";
        std::cout << "  [SPACE]        - Play / Pause Animation\n";
        std::cout << "  [UP / DOWN]    - Increase / Decrease Speed\n";
        std::cout << "  [LEFT / RIGHT] - Decrease / Increase Line Length\n";
        std::cout << "  [W / S]        - Increase / Decrease Glow Width\n";
        std::cout << "  [A / D]        - Increase / Decrease Line Width\n";
        std::cout << "  [K / J]        - Increase / Decrease Corner Radius (0 for sharp corners)\n";
        std::cout << "  [I / O]        - Increase / Decrease Master Intensity\n";
        std::cout << "  [Shift+I/O]    - Increase / Decrease Particle Intensity\n";
        std::cout << "  [P]            - Toggle Particle System\n";
        std::cout << "  [L]            - Cycle Multi-Lights Count (1 to 4)\n";
        std::cout << "  [C]            - Cycle Primary Color Theme\n";
        std::cout << "  [R]            - Reset to Default Settings\n";
        std::cout << "  [ESC]          - Exit\n";
        std::cout << "=============================================\n\n";
    }

    inline void PrintCurrentConfig(const EdgeLighting::Config &config, bool isPlaying)
    {
        std::string modeStr;
        switch (config.colorMode)
        {
        case EdgeLighting::ColorMode::STATIC:
            modeStr = "Static";
            break;
        case EdgeLighting::ColorMode::GRADIENT:
            modeStr = "Gradient";
            break;
        case EdgeLighting::ColorMode::AMBIENT_RAINBOW:
            modeStr = "Ambient Rainbow";
            break;
        }

        std::cout << "\r[Config] State: " << (isPlaying ? "PLAY" : "PAUSE")
                  << " | Mode: " << std::left << std::setw(15) << modeStr
                  << " | Speed: " << std::fixed << std::setprecision(1) << std::setw(4) << config.speed
                  << " | Length: " << std::setw(4) << config.lineLength
                  << " | Glow: " << std::setw(4) << config.glowWidth
                  << " | LineWidth: " << std::setw(4) << config.lineWidth
                  << " | CornerRad: " << std::setw(4) << config.borderRadius
                  << " | Intensity: " << std::setprecision(1) << std::setw(3) << config.intensity
                  << " | PtclIntensity: " << std::setprecision(1) << std::setw(3) << config.particleIntensity
                  << " | Lights: " << std::setw(2) << config.lightCount
                  << " | Particles: " << (config.enableParticles ? "ON " : "OFF")
                  << "      " << std::flush;
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
