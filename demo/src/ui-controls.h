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
        std::cout << "      Stroke + Wireframe Demo\n";
        std::cout << "=========================================\n";
        std::cout << "Controls:\n";
        std::cout << "  [R / F]        - Inc / Dec Stroke Thickness\n";

        std::cout << "  [I / O]        - Inc / Dec Stroke Intensity\n";
        std::cout << "  [T]            - Cycle Alignment (CENTER / INNER / OUTER)\n";
        std::cout << "  [H / Shift+H]  - Inc / Dec Fade Range (feather)\n";
        std::cout << "  [B]            - Cycle Fade Mode (SINGLE / DOUBLE)\n";
        std::cout << "  [, / .]        - Dec / Inc Line Count\n";
        std::cout << "  [M]            - Cycle Animation (STATIC / MOVING / FLASHING)\n";
        std::cout << "  [U / Y]        - Inc / Dec Segment Length (moving)\n";
        std::cout << "  [P / L]        - Inc / Dec Speed (moving)\n";
        std::cout << "  [SPACE]        - Pause / Resume Animation\n";
        std::cout << "  [N]            - Toggle Neon Mode (alt. rendering)\n";
        std::cout << "  [J]            - Toggle Particle Trail\n";
        std::cout << "  [V]            - Toggle Glow\n";
        std::cout << "  [[ / ]]        - Inc / Dec Glow Size\n";
        std::cout << "  [; / ']        - Inc / Dec Glow Intensity\n";
        std::cout << "  [G]            - Toggle Wireframe Bounding Box\n";
        std::cout << "  [1 / 2]        - Dec / Inc Start Pos\n";
        std::cout << "  [3 / 4]        - Dec / Inc End Pos\n";
        std::cout << "  [W]            - Toggle Winding (CW / CCW)\n";
        std::cout << "  [ESC]          - Exit\n";
        std::cout << "=========================================\n\n";
    }

    inline void PrintCurrentConfig(const EdgeLighting::Config &config, bool isPlaying)
    {
        std::string sideStr = (config.stroke.alignment == EdgeLighting::StrokeAlignment::CENTER) ? "CEN" : (config.stroke.alignment == EdgeLighting::StrokeAlignment::INNER) ? "IN"
                                                                                                                                                                             : "OUT";
        std::string modeStr = (config.stroke.animation == EdgeLighting::StrokeAnimation::STATIC) ? "ST" : (config.stroke.animation == EdgeLighting::StrokeAnimation::MOVING) ? "MV"
                                                                                                                                                                             : "FL";
        std::string fadeStr = (config.stroke.fadeMode == EdgeLighting::FadeMode::SINGLE) ? "SGL" : "DBL";


        std::string glowStr = config.stroke.glowEnable
                                  ? "ON sz:" + std::to_string((int)config.stroke.glowSize) + " int:" + std::to_string((int)(config.stroke.glowIntensity * 100))
                                  : "OFF";
        std::string particleStr = config.particles.enable ? "ON" : "OFF";
        std::string neonStr = config.neon.enable ? "NEON" : "SDF";
        std::string windingStr = (config.geometry.winding == EdgeLighting::Winding::CLOCKWISE) ? "CW" : "CCW";
        std::string blendStr = (config.stroke.blendSpace == EdgeLighting::BlendSpace::HSV) ? "HSV" : "RGB";

        std::cout << "\r[Stroke] W: " << std::setw(3) << config.stroke.thickness
                  << " | Int: " << std::fixed << std::setprecision(1) << std::setw(3) << config.stroke.intensity
                  << " | " << sideStr
                  << " | Fade: " << std::setprecision(1) << std::setw(3) << config.stroke.fadeRange
                  << " | " << modeStr
                  << " Ln: " << config.stroke.lineCount
                  << " Seg: " << std::setprecision(2) << std::setw(4) << config.stroke.segmentLength
                  << " Spd: " << std::setprecision(2) << std::setw(4) << config.stroke.speed
                  << " | Fd: " << fadeStr
                  << " | Glow: " << glowStr
                  << " | Ptcl: " << particleStr
                  << " | Mode: " << neonStr
                  << " | " << windingStr
                  << " | " << blendStr
                  << " S:" << std::setprecision(2) << config.path.startPos
                  << " E:" << std::setprecision(2) << config.path.endPos
                  << " | Anim: " << (isPlaying ? "PLAY" : "PAUS")
                  << " | Wire: " << (config.wireframe.enable ? "ON " : "OFF")
                  << "      " << std::flush;
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
