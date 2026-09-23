#ifndef _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
#define _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_

#include "core/edge-lighting.h"
#include "renderer/spotlight-tuning.h" // SPOT_MAX_LAMPS + SPOT_BLOOM_WINDOW_OUTER
#include "util/geometry-utils.h"       // sun placement, shared with the renderer
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
        std::cout << "  [SHIFT+O]      - Toggle Neon Resolution (full / half)\n";
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
                  << " | Wire: " << (config.debug.showWireframe ? "ON " : "OFF")
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

        /// Colours inline, so a layer with several of them stays one field per
        /// line like the rest of the dump.
        inline const char *ColorStr(const glm::vec4 &c, char *buf, size_t n)
        {
            snprintf(buf, n, "rgba(%.2f, %.2f, %.2f, %.2f)", c.r, c.g, c.b, c.a);
            return buf;
        }

        inline const char *ColorStr(const glm::vec3 &c, char *buf, size_t n)
        {
            snprintf(buf, n, "rgb(%.2f, %.2f, %.2f)", c.r, c.g, c.b);
            return buf;
        }

        /// The droplet band as a signed-distance span from the rect edge,
        /// positive outward. Mirrors GetBandExtent in droplets-renderer.cpp
        /// minus its guard: the guard is the renderer's own slack, not
        /// something the caller set.
        inline void GetBandSpan(const EdgeLighting::Config &config, float &lo, float &hi)
        {
            // The shader floors the width at 1 px and divides by it, so the
            // band of a 0-width config is 1 px, not 0.
            const float bw = std::max(config.droplets.bandWidth, 1.0f);
            const float offset = config.droplets.bandOffset;

            if (config.neon.glowSide == EdgeLighting::GlowSide::INSIDE)
            {
                lo = -(offset + bw);
                hi = -offset;
            }
            else if (config.neon.glowSide == EdgeLighting::GlowSide::OUTSIDE)
            {
                lo = offset;
                hi = offset + bw;
            }
            else
            {
                lo = offset - 0.5f * bw;
                hi = offset + 0.5f * bw;
            }
        }
    } // namespace Detail

    /// Full multi-line dump of everything that shapes the current frame.
    ///
    /// Deliberately separate from PrintCurrentConfig: that one is the \r status
    /// strip refreshed on every keypress, so it cannot grow. This is the "Dump
    /// Config" button's output - reach for it when a look needs reproducing or
    /// reporting, and print the fields that actually decide the pixels.
    ///
    /// Covers every registered renderer - neon, droplets, lens flare,
    /// spotlight, debug - in registration order, enabled or not. A disabled
    /// layer still prints in full: the dump's job is to let someone rebuild
    /// the whole state, and "what was the flare set to before I turned it off"
    /// is exactly the question it gets asked.
    ///
    /// Sizes are annotated with their units, because the px-vs-perimeter-fraction
    /// split is the thing that most often confuses a report: lineWidth /
    /// glowRadius / cutoffs are absolute px and hold their look at any rect
    /// size, while arc and segment positions and lengths are FRACTIONS of the
    /// perimeter, so their pixel extent scales with the geometry.
    ///
    /// Three cross-layer reads are spelled out rather than left implicit,
    /// because none of them is visible in the sub-config a reader would go
    /// looking in: the droplet band takes its SIDE from NeonConfig::glowSide,
    /// the flare's perimeter fraction runs along the OFFSET rect rather than
    /// the perimeter printed at the top, and an enabled clipped lamp holds
    /// SpotlightConfig::resolutionScale at 1.0 whatever the field says.
    ///
    /// Every position printed here is in APP coordinates (origin top-left, +y
    /// down) - the rect, the sun, the lamps and the clip area alike.
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
        // A cutoff on the side glowSide culls is ignored for the glow (the
        // renderer neutralises it - see neon.frag's band-distance block), so
        // say so here. Two dumps differing only in a subsumed cutoff describe
        // the same picture, and without this the reader has no way to tell.
        const char *inSub = (n.glowSide == EdgeLighting::GlowSide::OUTSIDE)
                                ? "   [subsumed by glowSide OUTSIDE]"
                                : "";
        const char *outSub = (n.glowSide == EdgeLighting::GlowSide::INSIDE)
                                 ? "   [subsumed by glowSide INSIDE]"
                                 : "";
        std::cout << "  insideCutoff     " << Detail::CutoffStr(n.insideCutoff, buf, sizeof buf)
                  << (n.insideCutoff.enable ? inSub : "") << "\n";
        std::cout << "  outsideCutoff    " << Detail::CutoffStr(n.outsideCutoff, buf, sizeof buf)
                  << (n.outsideCutoff.enable ? outSub : "") << "\n";
        std::cout << "  hueRotationRate  " << n.hueRotationRate << " cycles/s\n";
        std::cout << "  blendSpace       " << blendItems[static_cast<int>(n.blendSpace)] << "\n";
        std::cout << "  opaqueMode       " << opaqueItems[static_cast<int>(n.opaqueMode)];
        if (n.opaqueMode != OpaqueMode::NONE)
        {
            std::cout << ", colour (" << n.opaqueColor.r << ", " << n.opaqueColor.g
                      << ", " << n.opaqueColor.b << ", " << n.opaqueColor.a << ")"
                      << ", softness " << n.opaqueSoftness << " px";
            if (config.debug.opaqueOnly)
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

        std::cout << "\nResolution " << std::setprecision(2) << n.resolutionScale
                  << (n.resolutionScale < 1.0f ? " (scaled buffer + blit)" : " (full res, direct)")
                  << "   numSamples " << n.numSamples
                  << ", gradientLutSize " << n.gradientLutSize << "\n";

        // -------------------------------------------------------------------
        // Droplets. The band's SIDE is not in this sub-config at all - it is
        // read from NeonConfig::glowSide - so the span is spelled out in
        // signed distance from the rect edge (+ outward). That is the one form
        // that says where the band is without the reader having to carry the
        // cross-config read in their head.
        // -------------------------------------------------------------------
        const auto &dr = config.droplets;
        float bandLo = 0.0f, bandHi = 0.0f;
        Detail::GetBandSpan(config, bandLo, bandHi);
        int lanes = std::max(1, dr.lanes);

        std::cout << "\nDroplets   " << (dr.enable ? "ON" : "OFF") << "\n";
        std::cout << "  amount           " << std::setprecision(2) << dr.amount
                  << (dr.amount <= 0.0f ? "   (0 = condensation only)" : "") << "\n";
        std::cout << "  speed            " << dr.speed
                  << (dr.speed <= 0.0f ? "   (0 = rain frozen)" : "") << "\n";
        std::cout << "  lanes            " << lanes
                  << "   (lane width " << std::setprecision(1)
                  << std::max(dr.bandWidth, 1.0f) / static_cast<float>(lanes) << " px)\n";
        std::cout << "  bandWidth        " << dr.bandWidth << " px\n";
        std::cout << "  bandOffset       " << dr.bandOffset << " px\n";
        std::cout << "  band spans       " << bandLo << " .. " << bandHi
                  << " px from the edge (+ outward)\n";
        std::cout << "                   side from neon.glowSide = "
                  << sideItems[static_cast<int>(n.glowSide)] << "\n";
        std::cout << "  tint             " << Detail::ColorStr(dr.tint, buf, sizeof buf)
                  << "   (body only; rim and specular stay white)\n";

        // -------------------------------------------------------------------
        // Lens flare. perimeterPosition is a fraction like the neon's arcs and
        // segments, but NOT of the same curve: it advances along the OFFSET
        // rect, so `peri` above does not convert it once perimeterOffset is
        // non-zero. Print where the sun actually lands instead.
        // -------------------------------------------------------------------
        const auto &lf = config.lensFlare;

        // GetSunFragPosition is the renderer's own placement, so reusing it
        // here cannot drift from the picture. It returns gl_FragCoord space
        // (y-up), which needs a viewport this dump does not have - but the
        // viewport reaches it only as `viewportHeight - sunApp.y`, so passing 0
        // and negating recovers the app-space point (origin top-left, +y down)
        // that every other position in this dump is in.
        glm::vec2 sunFrag = GeometryUtils::GetSunFragPosition(lf, g, 0, 0);
        glm::vec2 sunApp(sunFrag.x, -sunFrag.y);

        std::cout << "\nLensFlare  " << (lf.enable ? "ON" : "OFF") << "\n";
        std::cout << "  perimeterPos     " << std::setprecision(4) << lf.perimeterPosition
                  << "   (sun at (" << std::setprecision(1) << sunApp.x << ", "
                  << sunApp.y << ") app px)\n";
        std::cout << "  perimeterOffset  " << lf.perimeterOffset << " px"
                  << "   (+ pushes the sun outward; travels the offset rect)\n";
        std::cout << "  size             " << std::setprecision(2) << lf.size
                  << "   (disc radius only; rays and ghost placement unaffected)\n";
        std::cout << "  color            " << Detail::ColorStr(lf.color, buf, sizeof buf)
                  << "   (sun core + rays; ghosts stay procedural)\n";
        std::cout << "  intensity        " << lf.intensity << "\n";
        std::cout << "  spread           " << lf.spread
                  << (lf.spread <= 0.0f ? "   (0 = ghosts and hex aperture gated off)" : "") << "\n";
        std::cout << "  ghostSpacing     " << lf.ghostSpacing << "\n";
        std::cout << "  ghostSize        " << lf.ghostSize << "\n";
        std::cout << "  ghostOffset      " << lf.ghostOffset
                  << "   (0 = centred cluster, negative pulls it toward the sun)\n";
        std::cout << "  ghostColor       " << Detail::ColorStr(lf.ghostColor, buf, sizeof buf)
                  << ", tint " << lf.ghostTint
                  << (lf.ghostTint <= 0.0f ? "   [tint 0: ghostColor unused, procedural rainbow]" : "")
                  << "\n";
        std::cout << "  flareCenter      (" << lf.flareCenter.x << ", " << lf.flareCenter.y << ")"
                  << "   normalised viewport, origin top-left\n";
        std::cout << "  rayDensity       " << lf.rayDensity
                  << "   (quantised to slots internally; not countable on screen)\n";
        std::cout << "  rotationRate     " << lf.rotationRate << " rev/s"
                  << "   (sun and rays only; ghosts stay on the axis)\n";
        std::cout << "  resolutionScale  " << lf.resolutionScale
                  << (lf.resolutionScale < 1.0f ? "   (scaled buffer + blit)"
                                                : "   (full res, direct)") << "\n";

        // -------------------------------------------------------------------
        // Spotlight. The odd one out: it reads nothing but its own sub-config,
        // so none of the geometry above moves it. Positions here and in the
        // clip area are APP coordinates - the same space as Geometry's
        // position: origin top-left, +y down.
        // -------------------------------------------------------------------
        const auto &sp = config.spotlight;
        const char *clipItems[] = {"KEEP_INSIDE", "KEEP_OUTSIDE"};
        size_t drawnLamps = std::min(sp.lamps.size(), static_cast<size_t>(SPOT_MAX_LAMPS));

        // Mirrors SpotlightRenderer's HasClippedLamp: the lamps that DRAW,
        // within the ceiling. One of these holds resolutionScale at 1.0.
        bool anyClippedLamp = false;
        for (size_t i = 0; i < drawnLamps; ++i)
        {
            const auto &l = sp.lamps[i];
            if (l.clipped && l.enable && l.intensity > 0.0f)
            {
                anyClippedLamp = true;
                break;
            }
        }
        // A zero-size KEEP_INSIDE area keeps nothing, and ClipArea has no
        // enable flag to say so - the one way an opted-in lamp goes dark with
        // nothing in the log. Worth a line in the dump for exactly that reason.
        bool clipBlanks = (sp.clipArea.mode == ClipMode::KEEP_INSIDE) &&
                          (sp.clipArea.width <= 0.0f || sp.clipArea.height <= 0.0f);

        std::cout << "\nSpotlight  " << (sp.enable ? "ON" : "OFF")
                  << "   lamps " << sp.lamps.size();
        if (sp.lamps.size() > drawnLamps)
        {
            std::cout << "   (" << sp.lamps.size() - drawnLamps
                      << " past SPOT_MAX_LAMPS " << SPOT_MAX_LAMPS << " ignored)";
        }
        std::cout << "\n           positions are APP px (origin top-left, +y down)"
                  << ", angle 0 = right, increasing CW\n";
        std::cout << "  resolutionScale  " << std::setprecision(2) << sp.resolutionScale
                  << (sp.resolutionScale < 1.0f ? "   (scaled buffer + blit)"
                                                : "   (full res, direct)");
        if (sp.resolutionScale < 1.0f && anyClippedLamp)
        {
            std::cout << "   [HELD AT 1.0: a clipped lamp is enabled]";
        }
        std::cout << "\n";

        std::cout << "  clipArea         " << std::setprecision(0)
                  << sp.clipArea.width << " x " << sp.clipArea.height
                  << " at (" << sp.clipArea.position.x << ", " << sp.clipArea.position.y << ")"
                  << ", corner " << sp.clipArea.cornerRadius
                  << ", soft " << std::setprecision(1) << sp.clipArea.edgeSoftness << " px"
                  << ", " << clipItems[static_cast<int>(sp.clipArea.mode)] << "\n";
        if (!anyClippedLamp)
        {
            std::cout << "                   [inert: no drawn lamp has clipped set]\n";
        }
        else if (clipBlanks)
        {
            std::cout << "                   [ZERO-SIZE KEEP_INSIDE: every clipped lamp draws nothing]\n";
        }

        std::cout << "\n  lamps (" << sp.lamps.size() << ")\n";
        for (size_t i = 0; i < sp.lamps.size(); ++i)
        {
            const auto &l = sp.lamps[i];
            bool dark = !l.enable || l.intensity <= 0.0f ||
                        (l.tint.r <= 0.0f && l.tint.g <= 0.0f && l.tint.b <= 0.0f);

            std::cout << "    [" << i << "] " << (l.enable ? "ON " : "OFF")
                      << " at (" << std::setprecision(0) << l.position.x << ", "
                      << l.position.y << ")"
                      << "  angle " << std::setprecision(1) << l.angle << " deg"
                      << "  beam " << l.beamAngle << " deg"
                      << "  throw " << std::setprecision(0) << l.throwLength << " px";
            if (i >= drawnLamps)
            {
                std::cout << "   [past SPOT_MAX_LAMPS: not drawn]";
            }
            else if (dark)
            {
                std::cout << "   [contributes nothing]";
            }
            std::cout << "\n";
            std::cout << "        aperture " << std::setprecision(1) << l.apertureWidth << " px"
                      << "  softness " << std::setprecision(2) << l.softness
                      << "  spreadFalloff " << l.spreadFalloff
                      << (l.spreadFalloff < 1.0f
                              ? " (reaches further than physical)"
                              : (l.spreadFalloff > 1.0f ? " (tighter pool)" : ""))
                      << "\n";
            std::cout << "        intensity " << l.intensity
                      << "  colorTemp " << std::setprecision(0) << l.colorTemp << " K"
                      << "  tint " << Detail::ColorStr(l.tint, buf, sizeof buf) << "\n";
            std::cout << "        bloom " << std::setprecision(2) << l.bloom
                      << "  radius " << std::setprecision(1) << l.bloomRadius << " px";
            if (l.bloom > 0.0f)
            {
                // The aperture bloom's SUPPORT, not its radius - that is what
                // decides the strip area, and a large one fills the frame on
                // its own. See docs/spotlight-renderer-plan.md.
                std::cout << " (support " << std::setprecision(0)
                          << l.bloomRadius * static_cast<float>(SPOT_BLOOM_WINDOW_OUTER) << " px)";
            }
            else
            {
                std::cout << " (bloom 0: term and its cost removed)";
            }
            std::cout << "  clip " << (l.clipped ? "ON" : "OFF");
            if (l.clipped && clipBlanks)
            {
                std::cout << "   [zero-size KEEP_INSIDE: draws nothing]";
            }
            std::cout << "\n";
        }

        const auto &d = config.debug;
        std::cout << "\nDebug      " << (d.enable ? "ON " : "OFF")
                  << "  gradientLUT " << (d.showGradientLUT ? "ON" : "OFF")
                  << ", colorStops " << (d.showColorStops ? "ON" : "OFF")
                  << ", opaqueOnly " << (d.opaqueOnly ? "ON" : "OFF")
                  << ", wireframe " << (d.showWireframe ? "ON" : "OFF") << "\n";
        std::cout << "=======================================================\n";
        std::cout << std::defaultfloat << std::flush;
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_UI_CONTROLS_H_
