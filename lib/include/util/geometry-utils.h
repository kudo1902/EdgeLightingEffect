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
                    dist -= geom.width;

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
                float arcLen = PI * r * 0.5f;

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
                    float angle = PI * 0.5f * (1.0f - frac);
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
                    float angle = -PI * 0.5f * frac;
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
                    float angle = -PI * 0.5f * (1.0f + frac);
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
                float arcLen = PI * r * 0.5f;

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
                    float angle = -PI + PI * 0.5f * frac;
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
                    float angle = -PI * 0.5f + PI * 0.5f * frac;
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
                    float angle = PI * 0.5f * frac;
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

        /// Maps a straight edge to the span of perimeter [0, 1] it occupies,
        /// returned as { x = start position, y = length } so it drops straight
        /// into a LitSegment (whose position is its start). Accounts for winding
        /// and rounded corners (the returned span covers only the straight
        /// portion, not the corner arcs). Example: light the left edge with
        ///   auto a = GetEdgeArc(Edge::LEFT, geom);
        ///   LitSegment{ a.x, a.y };
        inline glm::vec2 GetEdgeArc(Edge edge, const RectGeometry &geom)
        {
            float halfW = geom.width * 0.5f;
            float halfH = geom.height * 0.5f;
            float r = std::max(0.0f, std::min(geom.cornerRadius, std::min(halfW, halfH)));

            float ws = geom.width - 2.0f * r;  // top / bottom straight length
            float hs = geom.height - 2.0f * r; // left / right straight length
            float arc = PI * r * 0.5f;         // one quarter-corner length
            float peri = 2.0f * ws + 2.0f * hs + 4.0f * arc;
            if (peri <= 0.0f)
            {
                return glm::vec2(0.0f, 0.0f);
            }

            // Distance to the start of each edge's straight run, in traversal order.
            // CW : top -> right -> bottom -> left.  CCW: left -> bottom -> right -> top.
            float start = 0.0f;
            float len = (edge == Edge::TOP || edge == Edge::BOTTOM) ? ws : hs;

            if (geom.winding == Winding::CLOCKWISE)
            {
                switch (edge)
                {
                    case Edge::TOP:    { start = 0.0f; break; }
                    case Edge::RIGHT:  { start = ws + arc; break; }
                    case Edge::BOTTOM: { start = ws + hs + 2.0f * arc; break; }
                    case Edge::LEFT:   { start = 2.0f * ws + hs + 3.0f * arc; break; }
                }
            }
            else
            {
                switch (edge)
                {
                    case Edge::LEFT:   { start = 0.0f; break; }
                    case Edge::BOTTOM: { start = hs + arc; break; }
                    case Edge::RIGHT:  { start = hs + ws + 2.0f * arc; break; }
                    case Edge::TOP:    { start = 2.0f * hs + ws + 3.0f * arc; break; }
                }
            }

            return glm::vec2(start / peri, len / peri);
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

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GEOMETRY_UTILS_H_
