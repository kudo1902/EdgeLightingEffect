#ifndef _EDGE_LIGHTING_GEOMETRY_UTILS_H_
#define _EDGE_LIGHTING_GEOMETRY_UTILS_H_

#include "core/config.h"
#include <glm/glm.hpp>

namespace EdgeLighting
{
    namespace GeometryUtils
    {

        /// Returns a point on the rectangle edge given a perimeter progress in [0, 1].
        /// Progress 0 = top-left, goes clockwise (top → right → bottom → left).
        /// Supports rounded corners via borderRadius.
        /// The point is in the rectangle's local coordinate system (origin at center).
        inline glm::vec2 GetPointOnRectangle(float t, const Config::Geometry &geom)
        {
            float halfW = geom.width * 0.5f;
            float halfH = geom.height * 0.5f;
            float r = std::max(0.0f, geom.borderRadius);

            if (r <= 0.0f)
            {
                float peri = 2.0f * (geom.width + geom.height);
                float dist = t * peri;

                if (dist <= geom.width)
                {
                    float frac = dist / geom.width;
                    return glm::vec2(-halfW + frac * geom.width, halfH);
                }
                dist -= geom.width;

                if (dist <= geom.height)
                {
                    float frac = dist / geom.height;
                    return glm::vec2(halfW, halfH - frac * geom.height);
                }
                dist -= geom.height;

                if (dist <= geom.width)
                {
                    float frac = dist / geom.width;
                    return glm::vec2(halfW - frac * geom.width, -halfH);
                }
                dist -= geom.height;

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

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GEOMETRY_UTILS_H_
