#ifndef _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
#define _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_

#include "core/edge-lighting.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstdio>

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
        std::cout << "  [N]            - Toggle Neon\n";
        std::cout << "  [G]            - Toggle Wireframe Bounding Box\n";
        std::cout << "  [D]            - Toggle Water Droplets Pane\n";
        std::cout << "  [W]            - Toggle Winding (CW / CCW)\n";
        std::cout << "  [SPACE]        - Pause / Resume Animation\n";
        std::cout << "  [ESC]          - Exit\n";
        std::cout << "=========================================\n\n";
    }

    /// One-line, in-place (\r) status strip. Fired on every hotkey, so it must
    /// stay single-line and terse - see PrintFullConfig for the dump.
    inline void PrintCurrentConfig(const EdgeLighting::Config &config, bool isPlaying)
    {
        std::string neonStr = config.neon.enable ? "ON " : "OFF";
        std::string windingStr = (config.geometry.winding == EdgeLighting::Winding::CLOCKWISE) ? "CW" : "CCW";
        const char *blendItems[] = {"RGB", "HSV", "HSL"};
        std::string blendStr = blendItems[static_cast<int>(config.neon.blendSpace)];

        std::cout << "\r[Neon] LineW: " << std::setw(3) << config.neon.lineWidth
                  << " | En: " << neonStr
                  << " | Int: " << std::fixed << std::setprecision(2) << std::setw(4) << config.neon.intensity
                  << " | GlowR: " << std::setprecision(0) << std::setw(3) << config.neon.glowRadius
                  << " | Bloom: " << std::setprecision(2) << std::setw(4) << config.neon.bloomStrength
                  << " | HueRate: " << std::setprecision(2) << std::setw(4) << config.neon.hueRotationRate
                  << " | " << windingStr
                  << " | " << blendStr
                  << " | Anim: " << (isPlaying ? "PLAY" : "PAUS")
                  << " | Wire: " << (config.wireframe.enable ? "ON " : "OFF")
                  << "      " << std::flush;
    }

    namespace Detail
    {
        inline const char *CutoffStr(const EdgeLighting::Cutoff &c, char *buf, size_t n)
        {
            if (!c.enable)
            {
                snprintf(buf, n, "off");
            }
            else
            {
                snprintf(buf, n, "on  size %.1f px, soft %.1f px", c.size, c.softness);
            }
            return buf;
        }
    } // namespace Detail

    /// Full multi-line dump of everything that shapes the current frame.
    ///
    /// Deliberately separate from PrintCurrentConfig: that one is the \r status
    /// strip refreshed on every keypress, so it cannot grow. This is the "Dump
    /// Config" button's output - reach for it when a look needs reproducing or
    /// reporting, and print the fields that actually decide the pixels.
    ///
    /// Sizes are annotated with their units, because the px-vs-perimeter-fraction
    /// split is the thing that most often confuses a report: lineWidth /
    /// glowRadius / cutoffs are absolute px and hold their look at any rect
    /// size, while arc and segment positions and lengths are FRACTIONS of the
    /// perimeter, so their pixel extent scales with the geometry.
    inline void PrintFullConfig(const EdgeLighting::Config &config, bool isPlaying)
    {
        using namespace EdgeLighting;
        const char *blendItems[] = {"RGB", "HSV", "HSL"};
        const char *sideItems[] = {"BOTH", "INSIDE", "OUTSIDE"};
        const char *opaqueItems[] = {"NONE", "OUTSIDE", "INSIDE", "BOTH", "ALL"};
        char buf[96];

        const auto &g = config.geometry;
        const auto &n = config.neon;

        // Perimeter and the smaller half-extent: the two numbers that turn a
        // fraction into pixels and that bound how far an inner glow can reach
        // before the interior saturates.
        float rad = std::max(0.0f, std::min(g.cornerRadius, std::min(g.width, g.height) * 0.5f));
        float peri = 2.0f * (g.width - 2.0f * rad) + 2.0f * (g.height - 2.0f * rad) +
                     2.0f * 3.14159265358979f * rad;
        float halfMin = std::min(g.width, g.height) * 0.5f;

        std::cout << "\n===================== Config dump =====================\n";
        std::cout << std::fixed;

        std::cout << "Geometry   " << std::setprecision(0)
                  << g.width << " x " << g.height
                  << " at (" << g.position.x << ", " << g.position.y << ")"
                  << ", corner " << g.cornerRadius
                  << ", " << ((g.winding == Winding::CLOCKWISE) ? "CW" : "CCW") << "\n";
        std::cout << "           perimeter " << std::setprecision(1) << peri << " px"
                  << "   (1% of perimeter = " << peri * 0.01f << " px)\n";
        std::cout << "           half-min extent " << halfMin << " px"
                  << "   - inner glow saturates past this\n";

        std::cout << "\nNeon       " << (n.enable ? "ON" : "OFF")
                  << "   animation " << (isPlaying ? "PLAYING" : "PAUSED") << "\n";
        std::cout << "  lineWidth        " << std::setprecision(2) << n.lineWidth << " px\n";
        std::cout << "  filamentFalloff  " << n.filamentFalloff
                  << "   (N = " << 2.0f * n.filamentFalloff
                  << "; lower = softer and much longer tail)\n";
        std::cout << "  intensity        " << n.intensity << "\n";
        std::cout << "  glowRadius       " << n.glowRadius << " px"
                  << (n.glowRadius <= 0.0f ? "   (0 = filament only, halo/bloom gated off)" : "")
                  << "\n";
        std::cout << "  bloomStrength    " << n.bloomStrength << "\n";
        std::cout << "  glowSide         " << sideItems[static_cast<int>(n.glowSide)]
                  << ", softness " << n.glowSideSoftness << " px\n";
        std::cout << "  insideCutoff     " << Detail::CutoffStr(n.insideCutoff, buf, sizeof buf) << "\n";
        std::cout << "  outsideCutoff    " << Detail::CutoffStr(n.outsideCutoff, buf, sizeof buf) << "\n";
        std::cout << "  hueRotationRate  " << n.hueRotationRate << " cycles/s\n";
        std::cout << "  blendSpace       " << blendItems[static_cast<int>(n.blendSpace)] << "\n";
        std::cout << "  opaqueMode       " << opaqueItems[static_cast<int>(n.opaqueMode)];
        if (n.opaqueMode != OpaqueMode::NONE)
        {
            std::cout << ", colour (" << n.opaqueColor.r << ", " << n.opaqueColor.g
                      << ", " << n.opaqueColor.b << ", " << n.opaqueColor.a << ")"
                      << ", softness " << n.opaqueSoftness << " px";
            if (n.opaqueOnly)
            {
                std::cout << ", OPAQUE ONLY (neon emission suppressed)";
            }
        }
        std::cout << "\n";

        std::cout << "\n  colorStops (" << n.colorStops.size() << ")\n";
        for (size_t i = 0; i < n.colorStops.size(); ++i)
        {
            const auto &s = n.colorStops[i];
            std::cout << "    [" << i << "] pos " << std::setprecision(3) << s.position
                      << "  rgba(" << std::setprecision(2) << s.color.r << ", " << s.color.g
                      << ", " << s.color.b << ", " << s.color.a << ")\n";
        }

        std::cout << "\n  arcs (" << config.neon.arcs.size() << ")"
                  << "   start/length are PERIMETER FRACTIONS\n";
        for (size_t i = 0; i < config.neon.arcs.size(); ++i)
        {
            const auto &a = config.neon.arcs[i];
            std::cout << "    [" << i << "] start " << std::setprecision(4) << a.start
                      << "  length " << a.length
                      << " (= " << std::setprecision(1) << a.length * peri << " px)"
                      << "  intensity " << std::setprecision(2) << a.intensity
                      << "  stops " << a.colorStops.size() << "\n";
        }

        std::cout << "\n  segmentBoosts (" << n.segmentBoosts.size()
                  << ")   position/length are PERIMETER FRACTIONS\n";
        for (size_t i = 0; i < n.segmentBoosts.size(); ++i)
        {
            const auto &s = n.segmentBoosts[i];
            std::cout << "    [" << i << "] pos " << std::setprecision(4) << s.position
                      << "  length " << s.length
                      << " (= " << std::setprecision(1) << s.length * peri << " px)"
                      << "  boost " << std::setprecision(2) << s.boost
                      << "  stops " << s.colorStops.size() << "\n";
        }
        if (!n.preservedSegmentBoosts.empty())
        {
            std::cout << "    + " << n.preservedSegmentBoosts.size() << " preserved\n";
        }

        const auto &o = config.optimizedNeon;
        std::cout << "\nOptimized  " << (o.enable ? "ON" : "OFF")
                  << "   resolutionScale " << std::setprecision(2) << o.resolutionScale
                  << ", numSamples " << o.numSamples
                  << ", gradientLutSize " << o.gradientLutSize
                  << (o.showHalfRes ? ", showHalfRes" : "") << "\n";

        std::cout << "Wireframe  " << (config.wireframe.enable ? "ON" : "OFF") << "\n";
        std::cout << "=======================================================\n";
        std::cout << std::defaultfloat << std::flush;
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
