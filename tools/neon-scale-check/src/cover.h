#ifndef _NEON_SCALE_CHECK_COVER_H_
#define _NEON_SCALE_CHECK_COVER_H_

// The `cover` scene set: animated scenes for the glow coverage table
// (docs/neon-glow-cover-resolution-plan.md, section 2.3 and step 0).
//
// Unlike the page's twelve scenes (scenes.h), these MOVE: arcs and segments
// change length, and some segments travel, every frame - so every frame is
// the first after a config change, which the neon shades directly (pass 1b
// and the ring, no field) after re-baking the pieces of the coverage table
// the change reached. That is the path a table change shows up on. Each scene
// carries its own frame size, since the table's width is sized from the rect
// and the halo width: 1920 x 1080, plus one 3840 x 2160 frame for a thin glow
// on a TV-sized rect.
//
// The first ten are the plan's prototype scenes verbatim, so its numbers
// carry over; the rest add what its calibration needs (short and long
// segments with boosts up to 2, overlapping ones, sharp and large corners,
// the other winding, arcs with colour stops). Change one and every stored
// `cover` directory stops describing it.

#include "core/config.h"

#include <cmath>
#include <vector>

namespace NeonScaleCheck
{
    using namespace EdgeLighting;

    typedef struct CoverScene
    {
        const char *id;
        int width;               ///< Frame width, px.
        int height;              ///< Frame height, px.
        void (*apply)(Config &); ///< The scene, over CoverBase.
        bool arcsMove;           ///< Every arc's length moves, each on its own phase.
        bool segmentsMove;       ///< Every segment's length moves likewise.
        bool segmentsTravel;     ///< Every segment also travels round the ring.
    } CoverScene;

    /// A rect at (@p x, @p y), @p w x @p h, the neon on and the colour
    /// cross-fade off, so a frame depends on the config and the clock alone.
    inline Config CoverRect(float x, float y, float w, float h)
    {
        Config c;
        c.neon.enable = true;
        c.neon.colorTransitionDuration = 0.0f;
        c.debug.showWireframe = false;
        c.geometry.position = glm::vec2(x, y);
        c.geometry.width = w;
        c.geometry.height = h;
        return c;
    }

    /// @p n arcs sharing @p length of the ring, evenly spaced.
    inline void CoverArcs(Config &c, int n, float length)
    {
        c.neon.arcs.clear();
        for (int i = 0; i < n; ++i)
        {
            Arc a;
            a.start = float(i) / float(n) + 0.03f;
            a.length = length / float(n);
            a.intensity = 0.7f + 0.1f * float(i % 4);
            c.neon.arcs.push_back(a);
        }
    }

    /// @p n segments of @p length, evenly spaced.
    inline void CoverSegments(Config &c, int n, float length)
    {
        for (int i = 0; i < n; ++i)
        {
            SegmentBoost s;
            s.position = (float(i) + 0.5f) / float(n);
            s.length = length;
            s.boost = 0.8f + 0.1f * float(i % 3);
            c.neon.segmentBoosts.push_back(s);
        }
    }

    inline SegmentBoost CoverSegment(float position, float length, float boost)
    {
        SegmentBoost s;
        s.position = position;
        s.length = length;
        s.boost = boost;
        return s;
    }

    static const CoverScene COVER_SCENES[] = {
        {"mid_8arcs", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverArcs(c, 8, 0.6f);
         }, true, false, false},
        {"mid_8segs", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverSegments(c, 8, 0.1f);
         }, false, true, false},
        {"mid_1arc", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverArcs(c, 1, 0.5f);
         }, true, false, false},
        {"mid_mixed_travel", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverArcs(c, 4, 0.7f);
             CoverSegments(c, 4, 0.06f);
         }, true, false, true},
        {"big_thin", 1920, 1080, [](Config &c) {
             c = CoverRect(40, 40, 1840, 1000);
             c.neon.glowRadius = 2.0f;
             c.neon.lineWidth = 2.0f;
             CoverArcs(c, 4, 0.6f);
             CoverSegments(c, 2, 0.03f);
         }, true, true, false},
        {"big_default", 1920, 1080, [](Config &c) {
             c = CoverRect(40, 40, 1840, 1000);
             CoverArcs(c, 3, 0.7f);
             CoverSegments(c, 3, 0.08f);
         }, true, true, false},
        {"small", 1920, 1080, [](Config &c) {
             c = CoverRect(860, 480, 200, 120);
             CoverArcs(c, 2, 0.6f);
             CoverSegments(c, 1, 0.15f);
         }, true, true, false},
        {"circle", 1920, 1080, [](Config &c) {
             c = CoverRect(710, 290, 500, 500);
             c.geometry.cornerRadius = 250.0f;
             CoverArcs(c, 3, 0.6f);
             CoverSegments(c, 2, 0.1f);
         }, true, true, false},
        {"band_cut", 1920, 1080, [](Config &c) {
             c = CoverRect(40, 40, 1840, 1000);
             c.neon.glowSide = GlowSide::OUTSIDE;
             c.neon.outsideCutoff = {true, 20.0f, 4.0f};
             CoverArcs(c, 2, 0.8f);
             CoverSegments(c, 2, 0.05f);
         }, true, false, true},
        {"uhd_thin", 3840, 2160, [](Config &c) {
             c = CoverRect(120, 80, 3600, 2000);
             c.neon.glowRadius = 2.0f;
             c.neon.lineWidth = 2.0f;
             CoverArcs(c, 4, 0.6f);
             CoverSegments(c, 2, 0.02f);
         }, true, true, false},
        // Short segments, boosts up to 2: the narrowest bells a segment
        // pre-table has to resolve.
        {"seg_short", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             c.neon.segmentBoosts = {CoverSegment(0.20f, 0.01f, 2.0f), CoverSegment(0.55f, 0.02f, 1.5f),
                                     CoverSegment(0.80f, 0.01f, 1.0f)};
         }, false, true, true},
        // Long segments over partial arcs, overlapping and abutting pairs.
        {"seg_long", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverArcs(c, 2, 0.7f);
             c.neon.segmentBoosts = {CoverSegment(0.30f, 0.20f, 2.0f), CoverSegment(0.34f, 0.05f, 1.5f),
                                     CoverSegment(0.62f, 0.06f, 1.0f), CoverSegment(0.68f, 0.06f, 0.5f)};
         }, true, true, false},
        // Segments sitting on, then travelling past, the corners.
        {"seg_corners", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             c.neon.segmentBoosts = {CoverSegment(0.0f, 0.04f, 2.0f), CoverSegment(0.25f, 0.04f, 2.0f),
                                     CoverSegment(0.5f, 0.04f, 2.0f), CoverSegment(0.75f, 0.04f, 2.0f)};
         }, false, false, true},
        {"sharp", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             c.geometry.cornerRadius = 0.0f;
             CoverArcs(c, 3, 0.7f);
             CoverSegments(c, 2, 0.08f);
         }, true, true, false},
        {"big_radius", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             c.geometry.cornerRadius = 200.0f;
             CoverArcs(c, 3, 0.6f);
             CoverSegments(c, 2, 0.1f);
         }, true, true, false},
        {"ccw", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             c.geometry.winding = Winding::COUNTER_CLOCKWISE;
             CoverArcs(c, 3, 0.6f);
             CoverSegments(c, 3, 0.06f);
         }, true, true, true},
        // Partial arcs with colour stops of their own.
        {"arc_stops", 1920, 1080, [](Config &c) {
             c = CoverRect(480, 270, 960, 540);
             CoverArcs(c, 3, 0.75f);
             c.neon.arcs[0].colorStops = {ColorStop{0.0f, glm::vec4(0.0f, 0.9f, 1.0f, 1.0f)},
                                          ColorStop{1.0f, glm::vec4(1.0f, 0.2f, 0.6f, 1.0f)}};
             c.neon.arcs[2].colorStops = {ColorStop{0.0f, glm::vec4(1.0f, 0.8f, 0.1f, 1.0f)},
                                          ColorStop{1.0f, glm::vec4(0.2f, 1.0f, 0.3f, 1.0f)}};
         }, true, false, false},
    };

    /// The scales and frames `generate --set cover` renders.
    static const float COVER_SCALES[] = {1.0f, 0.5f, 0.25f};
    static const int COVER_FRAMES = 6;

    /// @p scene's config for frame @p frame at @p scale: the hue rotating at
    /// 0.3, and each moving light's length (and position, travelling) on its
    /// own phase.
    inline Config CoverFrameConfig(const CoverScene &scene, int frame, float scale)
    {
        Config c;
        scene.apply(c);
        c.neon.resolutionScale = scale;
        c.neon.hueRotationRate = 0.3f;
        const float f = float(frame);
        for (size_t i = 0; scene.arcsMove && i < c.neon.arcs.size(); ++i)
        {
            c.neon.arcs[i].length *= 0.6f + 0.4f * std::sin(0.7f * f + float(i));
        }
        for (size_t i = 0; scene.segmentsMove && i < c.neon.segmentBoosts.size(); ++i)
        {
            c.neon.segmentBoosts[i].length *= 0.7f + 0.3f * std::sin(0.9f * f + float(i));
        }
        for (size_t i = 0; scene.segmentsTravel && i < c.neon.segmentBoosts.size(); ++i)
        {
            c.neon.segmentBoosts[i].position = std::fmod(c.neon.segmentBoosts[i].position + 0.013f * f, 1.0f);
        }
        return c;
    }
}

#endif
