#ifndef _EDGE_LIGHTING_GEOMETRY_UTILS_H_
#define _EDGE_LIGHTING_GEOMETRY_UTILS_H_

#include "core/config.h"
#include "util/constants.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace EdgeLighting
{
    namespace GeometryUtils
    {
        namespace Detail
        {
            /// Fraction of the way along a perimeter span, guarding the spans
            /// that can legitimately collapse to zero length: a full stadium
            /// (cornerRadius == halfW or halfH) has no straight run on two of
            /// its sides, and a zero-width or zero-height rect degenerates
            /// further still. The `dist <= len` tests below still enter such a
            /// span when dist is 0, so an unguarded divide yields 0/0 = NaN and
            /// poisons every consumer of the point. Both endpoints of a
            /// zero-length span are the same point, so 0 is the right answer.
            inline float SafeFrac(float dist, float len)
            {
                return len > 0.0f ? dist / len : 0.0f;
            }

            /// Clockwise traversal: top → right → bottom → left
            inline glm::vec2 GetPointOnRectCW(float t, const RectGeometry &geom)
            {
                float halfW = geom.width * 0.5f;
                float halfH = geom.height * 0.5f;
                float r = std::max(0.0f, geom.cornerRadius);

                if (r <= 0.0f)
                {
                    float peri = 2.0f * (geom.width + geom.height);
                    float dist = t * peri;

                    // top edge: left to right
                    if (dist <= geom.width)
                    {
                        float frac = SafeFrac(dist, geom.width);
                        return glm::vec2(-halfW + frac * geom.width, halfH);
                    }
                    dist -= geom.width;

                    // right edge: top to bottom
                    if (dist <= geom.height)
                    {
                        float frac = SafeFrac(dist, geom.height);
                        return glm::vec2(halfW, halfH - frac * geom.height);
                    }
                    dist -= geom.height;

                    // bottom edge: right to left
                    if (dist <= geom.width)
                    {
                        float frac = SafeFrac(dist, geom.width);
                        return glm::vec2(halfW - frac * geom.width, -halfH);
                    }
                    dist -= geom.width;

                    // left edge: bottom to top
                    {
                        float frac = SafeFrac(dist, geom.height);
                        return glm::vec2(-halfW, -halfH + frac * geom.height);
                    }
                }

                r = std::min(r, std::min(halfW, halfH));

                float halfWs = halfW - r;
                float halfHs = halfH - r;
                float ws = geom.width - 2.0f * r;
                float hs = geom.height - 2.0f * r;
                float arcLen = PI * r * 0.5f;

                float peri = 2.0f * ws + 2.0f * hs + 4.0f * arcLen;
                float dist = t * peri;

                // top edge: left to right
                if (dist <= ws)
                {
                    float frac = SafeFrac(dist, ws);
                    return glm::vec2(-halfWs + frac * ws, halfH);
                }
                dist -= ws;

                // top-right arc: angle π/2 → 0
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = PI * 0.5f * (1.0f - frac);
                    return glm::vec2(halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // right edge: top to bottom
                if (dist <= hs)
                {
                    float frac = SafeFrac(dist, hs);
                    return glm::vec2(halfW, halfHs - frac * hs);
                }
                dist -= hs;

                // bottom-right arc: angle 0 → -π/2
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = -PI * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // bottom edge: right to left
                if (dist <= ws)
                {
                    float frac = SafeFrac(dist, ws);
                    return glm::vec2(halfWs - frac * ws, -halfH);
                }
                dist -= ws;

                // bottom-left arc: angle -π/2 → -π
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = -PI * 0.5f * (1.0f + frac);
                    return glm::vec2(-halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // left edge: bottom to top
                if (dist <= hs)
                {
                    float frac = SafeFrac(dist, hs);
                    return glm::vec2(-halfW, -halfHs + frac * hs);
                }
                dist -= hs;

                // top-left arc: angle -π → -3π/2
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = -PI * 0.5f * (2.0f + frac);
                    return glm::vec2(-halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
            }

            /// Counter-clockwise traversal: left → bottom → right → top
            inline glm::vec2 GetPointOnRectCCW(float t, const RectGeometry &geom)
            {
                float halfW = geom.width * 0.5f;
                float halfH = geom.height * 0.5f;
                float r = std::max(0.0f, geom.cornerRadius);

                if (r <= 0.0f)
                {
                    float peri = 2.0f * (geom.width + geom.height);
                    float dist = t * peri;

                    // left edge: top to bottom
                    if (dist <= geom.height)
                    {
                        float frac = SafeFrac(dist, geom.height);
                        return glm::vec2(-halfW, halfH - frac * geom.height);
                    }
                    dist -= geom.height;

                    // bottom edge: left to right
                    if (dist <= geom.width)
                    {
                        float frac = SafeFrac(dist, geom.width);
                        return glm::vec2(-halfW + frac * geom.width, -halfH);
                    }
                    dist -= geom.width;

                    // right edge: bottom to top
                    if (dist <= geom.height)
                    {
                        float frac = SafeFrac(dist, geom.height);
                        return glm::vec2(halfW, -halfH + frac * geom.height);
                    }
                    dist -= geom.height;

                    // top edge: right to left
                    {
                        float frac = SafeFrac(dist, geom.width);
                        return glm::vec2(halfW - frac * geom.width, halfH);
                    }
                }

                r = std::min(r, std::min(halfW, halfH));

                float halfWs = halfW - r;
                float halfHs = halfH - r;
                float ws = geom.width - 2.0f * r;
                float hs = geom.height - 2.0f * r;
                float arcLen = PI * r * 0.5f;

                float peri = 2.0f * ws + 2.0f * hs + 4.0f * arcLen;
                float dist = t * peri;

                // left edge: top to bottom
                if (dist <= hs)
                {
                    float frac = SafeFrac(dist, hs);
                    return glm::vec2(-halfW, halfHs - frac * hs);
                }
                dist -= hs;

                // bottom-left arc: angle -π → -π/2
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = -PI + PI * 0.5f * frac;
                    return glm::vec2(-halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // bottom edge: left to right
                if (dist <= ws)
                {
                    float frac = SafeFrac(dist, ws);
                    return glm::vec2(-halfWs + frac * ws, -halfH);
                }
                dist -= ws;

                // bottom-right arc: angle -π/2 → 0
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = -PI * 0.5f + PI * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // right edge: bottom to top
                if (dist <= hs)
                {
                    float frac = SafeFrac(dist, hs);
                    return glm::vec2(halfW, -halfHs + frac * hs);
                }
                dist -= hs;

                // top-right arc: angle 0 → π/2
                if (dist <= arcLen)
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = PI * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // top edge: right to left
                if (dist <= ws)
                {
                    float frac = SafeFrac(dist, ws);
                    return glm::vec2(halfWs - frac * ws, halfH);
                }
                dist -= ws;

                // top-left arc: angle π/2 → π
                {
                    float frac = SafeFrac(dist, arcLen);
                    float angle = PI * 0.5f + PI * 0.5f * frac;
                    return glm::vec2(-halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
            }

        } // namespace Detail

        /// Returns a point on the rectangle edge given a perimeter progress in [0, 1].
        /// Progress 0 = top-left. Direction is controlled by geom.winding.
        /// Supports rounded corners via cornerRadius.
        /// The point is in the rectangle's local coordinate system (origin at center).
        inline glm::vec2 GetPointOnRectangle(float t, const RectGeometry &geom)
        {
            if (geom.winding == Winding::CLOCKWISE)
            {
                return Detail::GetPointOnRectCW(t, geom);
            }

            return Detail::GetPointOnRectCCW(t, geom);
        }

        /// Converts an app-space point (rect top-left = (0,0), +Y down) to local space
        /// (rect center = (0,0), +Y up).
        inline glm::vec2 AppToLocal(const glm::vec2 &appPt, float halfW, float halfH)
        {
            return glm::vec2(appPt.x - halfW, halfH - appPt.y);
        }

        inline glm::vec2 AppToLocal(const glm::vec2 &appPt, const RectGeometry &geom)
        {
            return AppToLocal(appPt, geom.width * 0.5f, geom.height * 0.5f);
        }

        /// Converts an array of app-space points to local space.
        inline std::vector<glm::vec2> AppToLocal(const std::vector<glm::vec2> &appPts, float halfW, float halfH)
        {
            std::vector<glm::vec2> result;
            result.reserve(appPts.size());
            for (const auto &pt : appPts)
            {
                result.push_back(AppToLocal(pt, halfW, halfH));
            }
            return result;
        }

        inline std::vector<glm::vec2> AppToLocal(const std::vector<glm::vec2> &appPts, const RectGeometry &geom)
        {
            return AppToLocal(appPts, geom.width * 0.5f, geom.height * 0.5f);
        }

        /// Position of the lens-flare sun in gl_FragCoord pixels (origin
        /// bottom-left, +Y up) for a full-resolution viewport.
        ///
        /// The sun rides the rect perimeter at @c LensFlareConfig::perimeterPosition,
        /// pushed out from the rect by @c perimeterOffset. Shared by both
        /// lens-flare renderers so the sun lands in the identical spot; the
        /// half-res variant scales the result into its FBO by multiplying by its
        /// resolution scale.
        inline glm::vec2 GetSunFragPosition(const LensFlareConfig &lensFlare,
                                            const RectGeometry &geom,
                                            int viewportWidth,
                                            int viewportHeight)
        {
            (void)viewportWidth;

            float halfW = geom.width * 0.5f;
            float halfH = geom.height * 0.5f;

            // Sample the *offset* rounded rect directly rather than sampling the
            // base rect and pushing the sample along an estimated edge normal.
            //
            // A constant-distance offset of a rounded rect is just another
            // rounded rect sharing the same centre: the half extents grow by the
            // offset and the corner radius becomes r + offset, while the straight
            // spans keep their length. The traced curve is identical, but
            // perimeterPosition now advances by arc length along the path the sun
            // actually travels, so the sun moves at a constant speed the whole way
            // round. Sampling the base rect kept t proportional to the *base*
            // perimeter, which is what made the motion look wrong once a corner
            // radius and an offset were combined:
            //   - the sun crawled along the straight edges then whipped through
            //     each corner at (r + offset) / r times its edge speed (6x at
            //     r = 40, offset = 200);
            //   - with a small or zero radius the whole 90 degree normal swing
            //     happened inside the fixed finite-difference epsilon, so the sun
            //     jumped across the corner in a couple of frames;
            //   - an offset below -r inverted the corner arcs, so the sun
            //     backtracked through a cusp instead of turning the corner.
            // Clamping the offset radius at 0 turns that last case into plain
            // sharp corners on an inset rect.
            RectGeometry offsetGeom = geom;
            float radius = std::min(std::max(geom.cornerRadius, 0.0f), std::min(halfW, halfH));
            offsetGeom.width = std::max(geom.width + 2.0f * lensFlare.perimeterOffset, 0.0f);
            offsetGeom.height = std::max(geom.height + 2.0f * lensFlare.perimeterOffset, 0.0f);
            offsetGeom.cornerRadius = std::max(radius + lensFlare.perimeterOffset, 0.0f);

            // Both rects share a centre, so the sample already sits in the base
            // rect's local space. A negative offset large enough to collapse the
            // rect degenerates to that shared centre.
            glm::vec2 local(0.0f);
            if (offsetGeom.width > 0.0f && offsetGeom.height > 0.0f)
            {
                local = GetPointOnRectangle(lensFlare.perimeterPosition, offsetGeom);
            }

            // Local (y-up, centre origin) -> app pixel space (rect top-left
            // origin, y-down) -> gl_FragCoord (y-up).
            glm::vec2 sunApp(geom.position.x + local.x + halfW,
                             geom.position.y + (halfH - local.y));
            return glm::vec2(sunApp.x, static_cast<float>(viewportHeight) - sunApp.y);
        }

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GEOMETRY_UTILS_H_
