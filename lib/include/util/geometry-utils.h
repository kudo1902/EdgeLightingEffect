#ifndef _EDGE_LIGHTING_GEOMETRY_UTILS_H_
#define _EDGE_LIGHTING_GEOMETRY_UTILS_H_

#include "core/config.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{
    namespace GeometryUtils
    {
        namespace Detail
        {
            /// Clockwise traversal: top → right → bottom → left
            inline glm::vec2 GetPointOnRectCW(float t, const Config::Geometry &geom)
            {
                float halfW = geom.width * 0.5f;
                float halfH = geom.height * 0.5f;
                float r = std::max(0.0f, geom.borderRadius);

                if (r <= 0.0f)
                {
                    float peri = 2.0f * (geom.width + geom.height);
                    float dist = t * peri;

                    // top edge: left to right
                    if (dist <= geom.width)
                    {
                        float frac = dist / geom.width;
                        return glm::vec2(-halfW + frac * geom.width, halfH);
                    }
                    dist -= geom.width;

                    // right edge: top to bottom
                    if (dist <= geom.height)
                    {
                        float frac = dist / geom.height;
                        return glm::vec2(halfW, halfH - frac * geom.height);
                    }
                    dist -= geom.height;

                    // bottom edge: right to left
                    if (dist <= geom.width)
                    {
                        float frac = dist / geom.width;
                        return glm::vec2(halfW - frac * geom.width, -halfH);
                    }
                    dist -= geom.height;

                    // left edge: bottom to top
                    {
                        float frac = dist / geom.height;
                        return glm::vec2(-halfW, -halfH + frac * geom.height);
                    }
                }

                r = std::min(r, std::min(halfW, halfH));

                float halfWs = halfW - r;
                float halfHs = halfH - r;
                float ws = geom.width - 2.0f * r;
                float hs = geom.height - 2.0f * r;
                constexpr float pi = 3.14159265358979323846f;
                float arcLen = pi * r * 0.5f;

                float peri = 2.0f * ws + 2.0f * hs + 4.0f * arcLen;
                float dist = t * peri;

                // top edge: left to right
                if (dist <= ws)
                {
                    float frac = dist / ws;
                    return glm::vec2(-halfWs + frac * ws, halfH);
                }
                dist -= ws;

                // top-right arc: angle π/2 → 0
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = pi * 0.5f * (1.0f - frac);
                    return glm::vec2(halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // right edge: top to bottom
                if (dist <= hs)
                {
                    float frac = dist / hs;
                    return glm::vec2(halfW, halfHs - frac * hs);
                }
                dist -= hs;

                // bottom-right arc: angle 0 → -π/2
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = -pi * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // bottom edge: right to left
                if (dist <= ws)
                {
                    float frac = dist / ws;
                    return glm::vec2(halfWs - frac * ws, -halfH);
                }
                dist -= ws;

                // bottom-left arc: angle -π/2 → -π
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = -pi * 0.5f * (1.0f + frac);
                    return glm::vec2(-halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // left edge: bottom to top
                if (dist <= hs)
                {
                    float frac = dist / hs;
                    return glm::vec2(-halfW, -halfHs + frac * hs);
                }
                dist -= hs;

                // top-left arc: angle -π → -3π/2
                {
                    float frac = dist / arcLen;
                    float angle = -pi * 0.5f * (2.0f + frac);
                    return glm::vec2(-halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
            }

            /// Counter-clockwise traversal: left → bottom → right → top
            inline glm::vec2 GetPointOnRectCCW(float t, const Config::Geometry &geom)
            {
                float halfW = geom.width * 0.5f;
                float halfH = geom.height * 0.5f;
                float r = std::max(0.0f, geom.borderRadius);

                if (r <= 0.0f)
                {
                    float peri = 2.0f * (geom.width + geom.height);
                    float dist = t * peri;

                    // left edge: top to bottom
                    if (dist <= geom.height)
                    {
                        float frac = dist / geom.height;
                        return glm::vec2(-halfW, halfH - frac * geom.height);
                    }
                    dist -= geom.height;

                    // bottom edge: left to right
                    if (dist <= geom.width)
                    {
                        float frac = dist / geom.width;
                        return glm::vec2(-halfW + frac * geom.width, -halfH);
                    }
                    dist -= geom.width;

                    // right edge: bottom to top
                    if (dist <= geom.height)
                    {
                        float frac = dist / geom.height;
                        return glm::vec2(halfW, -halfH + frac * geom.height);
                    }
                    dist -= geom.height;

                    // top edge: right to left
                    {
                        float frac = dist / geom.width;
                        return glm::vec2(halfW - frac * geom.width, halfH);
                    }
                }

                r = std::min(r, std::min(halfW, halfH));

                float halfWs = halfW - r;
                float halfHs = halfH - r;
                float ws = geom.width - 2.0f * r;
                float hs = geom.height - 2.0f * r;
                constexpr float pi = 3.14159265358979323846f;
                float arcLen = pi * r * 0.5f;

                float peri = 2.0f * ws + 2.0f * hs + 4.0f * arcLen;
                float dist = t * peri;

                // left edge: top to bottom
                if (dist <= hs)
                {
                    float frac = dist / hs;
                    return glm::vec2(-halfW, halfHs - frac * hs);
                }
                dist -= hs;

                // bottom-left arc: angle -π → -π/2
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = -pi + pi * 0.5f * frac;
                    return glm::vec2(-halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // bottom edge: left to right
                if (dist <= ws)
                {
                    float frac = dist / ws;
                    return glm::vec2(-halfWs + frac * ws, -halfH);
                }
                dist -= ws;

                // bottom-right arc: angle -π/2 → 0
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = -pi * 0.5f + pi * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), -halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // right edge: bottom to top
                if (dist <= hs)
                {
                    float frac = dist / hs;
                    return glm::vec2(halfW, -halfHs + frac * hs);
                }
                dist -= hs;

                // top-right arc: angle 0 → π/2
                if (dist <= arcLen)
                {
                    float frac = dist / arcLen;
                    float angle = pi * 0.5f * frac;
                    return glm::vec2(halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
                dist -= arcLen;

                // top edge: right to left
                if (dist <= ws)
                {
                    float frac = dist / ws;
                    return glm::vec2(halfWs - frac * ws, halfH);
                }
                dist -= ws;

                // top-left arc: angle π/2 → π
                {
                    float frac = dist / arcLen;
                    float angle = pi * 0.5f + pi * 0.5f * frac;
                    return glm::vec2(-halfWs + r * cosf(angle), halfHs + r * sinf(angle));
                }
            }

        } // namespace Detail

        /// Returns a point on the rectangle edge given a perimeter progress in [0, 1].
        /// Progress 0 = top-left. Direction is controlled by geom.winding.
        /// Supports rounded corners via borderRadius.
        /// The point is in the rectangle's local coordinate system (origin at center).
        inline glm::vec2 GetPointOnRectangle(float t, const Config::Geometry &geom)
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

        inline glm::vec2 AppToLocal(const glm::vec2 &appPt, const Config::Geometry &geom)
        {
            return AppToLocal(appPt, geom.width * 0.5f, geom.height * 0.5f);
        }

        /// Converts an array of app-space points to local space.
        inline std::vector<glm::vec2> AppToLocal(const std::vector<glm::vec2> &appPts, float halfW, float halfH)
        {
            std::vector<glm::vec2> result;
            result.reserve(appPts.size());
            for (const auto &pt : appPts)
                result.push_back(AppToLocal(pt, halfW, halfH));
            return result;
        }

        inline std::vector<glm::vec2> AppToLocal(const std::vector<glm::vec2> &appPts, const Config::Geometry &geom)
        {
            return AppToLocal(appPts, geom.width * 0.5f, geom.height * 0.5f);
        }

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GEOMETRY_UTILS_H_
