#ifndef _NEON_SCALE_CHECK_SCENES_H_
#define _NEON_SCALE_CHECK_SCENES_H_

// The twelve scenes of docs/neon-resolution-scale-comparison.html.
//
// These are the page's scenes exactly. The first version of the page was made
// by a harness nobody kept, and its blurbs left out every colour, so each
// definition below was recovered by rendering it at scale 1.0 and matching the
// page's stored image: all twelve reproduce it within 1 level (2 on a handful
// of values), the variance between the Apple M2 Pro that made it and the AMD
// Radeon Pro 5300M that recovered it. Change one and every stored image and
// number on the page stops describing it.
//
// Each scene also carries where the page looks at it: the crop rect its tiles
// show, and the column its cross-section is taken down.

#include "core/config.h"

namespace NeonScaleCheck
{
    using namespace EdgeLighting;

    typedef struct Scene
    {
        const char *id;
        int crop[4];       ///< Page crop rect: x, y, width, height, in frame px (top-left origin).
        int profileX;      ///< Column the page's cross-section runs down.
        int profileY0;     ///< First row of that cross-section; it is 73 rows long.
        int profileEdgeY;  ///< Row of the rect edge the cross-section crosses.
        void (*apply)(Config &);
    } Scene;

    inline ColorStop Stop(float position, float r, float g, float b)
    {
        return ColorStop{position, glm::vec4(r, g, b, 1.0f)};
    }

    /// The setting every scene starts from: a 640 x 360 rect at (320, 180) in a
    /// 1280 x 720 frame, the neon on and everything time-dependent frozen, so
    /// a render depends on the config alone.
    inline Config BaseConfig()
    {
        Config c;
        c.geometry.width = 640.0f;
        c.geometry.height = 360.0f;
        c.geometry.position = glm::vec2(320.0f, 180.0f);
        c.neon.enable = true;
        c.neon.hueRotationRate = 0.0f;
        c.neon.colorTransitionDuration = 0.0f;
        c.debug.showWireframe = false;
        return c;
    }

    static const Scene SCENES[] = {
        {"default", {272, 132, 128, 96}, 640, 144, 180, [](Config &) {}},
        {"hairline", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.colorStops = {Stop(0.0f, 0.2f, 0.9f, 1.0f), Stop(0.5f, 1.0f, 0.3f, 0.9f)};
             c.neon.lineWidth = 1.0f;
             c.neon.filamentFalloff = 2.0f;
             c.neon.glowRadius = 3.0f;
             c.neon.bloomStrength = 0.15f;
         }},
        {"crisp_tube", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.colorStops = {Stop(0.0f, 1.0f, 0.25f, 0.55f)};
             c.neon.lineWidth = 8.0f;
             c.neon.filamentFalloff = 4.0f;
             c.neon.glowRadius = 4.0f;
             c.neon.bloomStrength = 0.2f;
         }},
        {"soft_wash", {240, 100, 192, 144}, 640, 144, 180, [](Config &c) {
             c.neon.colorStops = {Stop(0.0f, 0.1f, 0.5f, 1.0f), Stop(0.5f, 0.6f, 0.1f, 1.0f)};
             c.neon.lineWidth = 10.0f;
             c.neon.glowRadius = 30.0f;
             c.neon.bloomStrength = 1.2f;
             c.neon.intensity = 1.5f;
         }},
        {"sharp_corners", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.geometry.cornerRadius = 0.0f;
         }},
        {"small_rect", {536, 288, 208, 144}, 640, 276, 312, [](Config &c) {
             c.geometry.width = 160.0f;
             c.geometry.height = 96.0f;
             c.geometry.position = glm::vec2(560.0f, 312.0f);
             c.geometry.cornerRadius = 20.0f;
             c.neon.lineWidth = 3.0f;
         }},
        {"glow_inside", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.colorStops = {Stop(0.0f, 1.0f, 0.55f, 0.1f), Stop(0.5f, 1.0f, 0.1f, 0.35f)};
             c.neon.glowSide = GlowSide::INSIDE;
             c.neon.glowSideSoftness = 0.0f;
             c.neon.glowRadius = 12.0f;
             c.neon.bloomStrength = 0.6f;
         }},
        {"card_outside", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.glowSide = GlowSide::OUTSIDE;
             c.neon.opaqueMode = OpaqueMode::INSIDE;
             c.neon.opaqueColor = glm::vec4(0.12f, 0.13f, 0.18f, 1.0f);
             c.neon.colorStops = {Stop(0.0f, 0.3f, 1.0f, 0.6f), Stop(0.5f, 0.2f, 0.6f, 1.0f)};
             c.neon.glowRadius = 10.0f;
             c.neon.bloomStrength = 0.5f;
         }},
        {"bounded_band", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.insideCutoff = {true, 6.0f, 2.0f};
             c.neon.outsideCutoff = {true, 14.0f, 3.0f};
             c.neon.glowRadius = 20.0f;
             c.neon.bloomStrength = 0.8f;
         }},
        {"arcs", {296, 140, 192, 144}, 640, 144, 180, [](Config &c) {
             Arc a;
             a.start = 0.02f;
             a.length = 0.20f;
             a.colorStops = {Stop(0.0f, 0.0f, 0.9f, 1.0f), Stop(1.0f, 0.2f, 0.3f, 1.0f)};
             Arc b;
             b.start = 0.36f;
             b.length = 0.22f;
             b.colorStops = {Stop(0.0f, 1.0f, 0.2f, 0.8f), Stop(1.0f, 1.0f, 0.6f, 0.1f)};
             Arc d;
             d.start = 0.68f;
             d.length = 0.24f;
             d.intensity = 0.8f;
             d.colorStops = {Stop(0.0f, 0.3f, 1.0f, 0.3f), Stop(1.0f, 1.0f, 1.0f, 0.2f)};
             c.neon.arcs = {a, b, d};
         }},
        {"segments", {256, 382, 128, 96}, 678, 144, 180, [](Config &c) {
             c.neon.intensity = 0.35f;
             c.neon.colorStops = {Stop(0.0f, 0.25f, 0.4f, 1.0f)};
             auto boost = [](float position, float length, float r, float g, float b) {
                 SegmentBoost s;
                 s.position = position;
                 s.length = length;
                 s.boost = 2.0f;
                 s.colorStops = {Stop(0.0f, r, g, b)};
                 return s;
             };
             c.neon.segmentBoosts = {boost(0.12f, 0.05f, 1.0f, 1.0f, 1.0f),
                                     boost(0.45f, 0.04f, 1.0f, 0.3f, 0.7f),
                                     boost(0.80f, 0.06f, 0.4f, 1.0f, 0.9f)};
         }},
        {"overdrive", {272, 132, 128, 96}, 640, 144, 180, [](Config &c) {
             c.neon.colorStops = {Stop(0.0f, 1.0f, 0.15f, 0.05f), Stop(0.5f, 1.0f, 0.75f, 0.1f)};
             c.neon.intensity = 3.0f;
             c.neon.bloomStrength = 1.5f;
             c.neon.lineWidth = 6.0f;
             c.neon.glowRadius = 8.0f;
         }},
    };

    static const float SCALES[] = {1.0f, 0.75f, 0.5f, 0.35f, 0.25f, 0.125f};
    static const char *const SCALE_TAGS[] = {"s1000", "s750", "s500", "s350", "s250", "s125"};
    static const int SCALE_COUNT = 6;
}

#endif
