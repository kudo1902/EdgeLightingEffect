#include "core/config-validation.h"
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    namespace ConfigValidation
    {
        namespace
        {
            /// @p value held to [@p lo, @p hi]; NaN to the bound nearest 0 - 0
            /// itself when the range holds it.
            float Clamp(float value, float lo, float hi)
            {
                if (std::isnan(value))
                {
                    return std::clamp(0.0f, lo, hi);
                }
                return std::clamp(value, lo, hi);
            }

            /// A length, size or position in px, of either sign.
            float Extent(float value) { return Clamp(value, -MAX_EXTENT_PX, MAX_EXTENT_PX); }

            /// A rect's side: never negative. A negative side gave the neon a
            /// negative perimeter, and every perimeter-relative uniform of the
            /// glow coverage bake the wrong sign - a GPU hang on every frame
            /// that baked it.
            float Side(float value) { return Clamp(value, 0.0f, MAX_EXTENT_PX); }

            /// Any other float, of either sign.
            float Magnitude(float value) { return Clamp(value, -MAX_MAGNITUDE, MAX_MAGNITUDE); }

            glm::vec2 Extent(const glm::vec2 &v) { return glm::vec2(Extent(v.x), Extent(v.y)); }
            glm::vec2 Magnitude(const glm::vec2 &v) { return glm::vec2(Magnitude(v.x), Magnitude(v.y)); }
            glm::vec3 Magnitude(const glm::vec3 &v) { return glm::vec3(Magnitude(v.x), Magnitude(v.y), Magnitude(v.z)); }
            glm::vec4 Magnitude(const glm::vec4 &v)
            {
                return glm::vec4(Magnitude(v.x), Magnitude(v.y), Magnitude(v.z), Magnitude(v.w));
            }

            void SanitizeCutoff(Cutoff &cutoff)
            {
                cutoff.size = Extent(cutoff.size);
                cutoff.softness = Extent(cutoff.softness);
            }

            void SanitizeStops(std::vector<ColorStop> &stops)
            {
                for (ColorStop &stop : stops)
                {
                    stop.position = Magnitude(stop.position);
                    stop.color = Magnitude(stop.color);
                }
            }

            void SanitizeSegment(SegmentBoost &segment)
            {
                segment.position = Magnitude(segment.position);
                segment.length = Magnitude(segment.length);
                segment.boost = Magnitude(segment.boost);
                SanitizeStops(segment.colorStops);
            }

            void SanitizeGeometry(RectGeometry &geometry)
            {
                geometry.width = Side(geometry.width);
                geometry.height = Side(geometry.height);
                geometry.position = Extent(geometry.position);
                geometry.cornerRadius = Extent(geometry.cornerRadius);
            }

            void SanitizeNeon(NeonConfig &neon)
            {
                neon.resolutionScale = Magnitude(neon.resolutionScale);
                neon.gradientLutSize = std::min(neon.gradientLutSize, MAX_GRADIENT_LUT_SIZE);
                neon.opaqueColor = Magnitude(neon.opaqueColor);
                SanitizeCutoff(neon.opaqueInsideCutoff);
                SanitizeCutoff(neon.opaqueOutsideCutoff);
                neon.lineWidth = Extent(neon.lineWidth);
                neon.filamentFalloff = Magnitude(neon.filamentFalloff);
                neon.intensity = Magnitude(neon.intensity);
                neon.glowRadius = Extent(neon.glowRadius);
                neon.bloomStrength = Magnitude(neon.bloomStrength);
                neon.glowSideSoftness = Extent(neon.glowSideSoftness);
                SanitizeCutoff(neon.insideCutoff);
                SanitizeCutoff(neon.outsideCutoff);
                neon.hueRotationRate = Magnitude(neon.hueRotationRate);
                SanitizeStops(neon.colorStops);
                for (SegmentBoost &segment : neon.segmentBoosts)
                {
                    SanitizeSegment(segment);
                }
                for (PreservedSegment &preserved : neon.preservedSegmentBoosts)
                {
                    SanitizeSegment(preserved.segment);
                }
                for (Arc &arc : neon.arcs)
                {
                    arc.start = Magnitude(arc.start);
                    arc.length = Magnitude(arc.length);
                    arc.intensity = Magnitude(arc.intensity);
                    SanitizeStops(arc.colorStops);
                }
                neon.colorTransitionDuration = Magnitude(neon.colorTransitionDuration);
            }

            void SanitizeDroplets(DropletsConfig &droplets)
            {
                droplets.amount = Magnitude(droplets.amount);
                droplets.speed = Magnitude(droplets.speed);
                droplets.bandWidth = Extent(droplets.bandWidth);
                droplets.bandOffset = Extent(droplets.bandOffset);
                droplets.tint = Magnitude(droplets.tint);
            }

            void SanitizeLensFlare(LensFlareConfig &flare)
            {
                flare.perimeterPosition = Magnitude(flare.perimeterPosition);
                flare.perimeterOffset = Magnitude(flare.perimeterOffset);
                flare.size = Magnitude(flare.size);
                flare.color = Magnitude(flare.color);
                flare.intensity = Magnitude(flare.intensity);
                flare.spread = Magnitude(flare.spread);
                flare.ghostSpacing = Magnitude(flare.ghostSpacing);
                flare.ghostSize = Magnitude(flare.ghostSize);
                flare.ghostOffset = Magnitude(flare.ghostOffset);
                flare.ghostColor = Magnitude(flare.ghostColor);
                flare.ghostTint = Magnitude(flare.ghostTint);
                flare.flareCenter = Magnitude(flare.flareCenter);
                flare.rayDensity = Magnitude(flare.rayDensity);
                flare.rotationRate = Magnitude(flare.rotationRate);
                flare.resolutionScale = Magnitude(flare.resolutionScale);
            }

            void SanitizeLamp(SpotLight &lamp)
            {
                lamp.position = Extent(lamp.position);
                lamp.angle = Magnitude(lamp.angle);
                lamp.beamAngle = Magnitude(lamp.beamAngle);
                // The throw bounds the cone's reach solve: at 1e10 px its
                // bisection's ends became adjacent floats more than 1 px
                // apart, and it never finished.
                lamp.throwLength = Extent(lamp.throwLength);
                lamp.spreadFalloff = Magnitude(lamp.spreadFalloff);
                lamp.apertureWidth = Extent(lamp.apertureWidth);
                lamp.softness = Magnitude(lamp.softness);
                lamp.intensity = Magnitude(lamp.intensity);
                lamp.bloom = Magnitude(lamp.bloom);
                lamp.bloomRadius = Extent(lamp.bloomRadius);
                lamp.colorTemp = Magnitude(lamp.colorTemp);
                lamp.tint = Magnitude(lamp.tint);
            }

            void SanitizeSpotlight(SpotlightConfig &spotlight)
            {
                for (SpotLight &lamp : spotlight.lamps)
                {
                    SanitizeLamp(lamp);
                }
                ClipArea &clip = spotlight.clipArea;
                clip.position = Extent(clip.position);
                clip.width = Extent(clip.width);
                clip.height = Extent(clip.height);
                clip.cornerRadius = Extent(clip.cornerRadius);
                clip.edgeSoftness = Extent(clip.edgeSoftness);
                spotlight.resolutionScale = Magnitude(spotlight.resolutionScale);
            }

            void SanitizeDebug(DebugConfig &debug)
            {
                debug.wireframeColor = Magnitude(debug.wireframeColor);
                debug.sharedBufferInsetSize = Magnitude(debug.sharedBufferInsetSize);
            }
        }

        void Sanitize(Config &config)
        {
            SanitizeGeometry(config.geometry);
            SanitizeNeon(config.neon);
            SanitizeDroplets(config.droplets);
            SanitizeLensFlare(config.lensFlare);
            SanitizeSpotlight(config.spotlight);
            SanitizeDebug(config.debug);
        }
    }
}
