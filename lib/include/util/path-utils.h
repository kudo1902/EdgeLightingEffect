#ifndef _EDGE_LIGHTING_PATH_UTILS_H_
#define _EDGE_LIGHTING_PATH_UTILS_H_

#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

namespace EdgeLighting
{
    namespace PathUtils
    {
        /// Returns the total length of a polyline in local-space units.
        inline float GetPathLength(const std::vector<glm::vec2> &points, bool closed)
        {
            if (points.size() < 2)
            {
                return 0.0f;
            }

            float len = 0.0f;
            for (size_t i = 0; i < points.size() - 1; ++i)
            {
                len += glm::length(points[i + 1] - points[i]);
            }
            if (closed)
            {
                len += glm::length(points.front() - points.back());
            }
            return len;
        }

        /// Returns a point on the polyline at progress t in [0, 1].
        /// t=0 is the first point; t=1 is the last point (or back to first if closed).
        inline glm::vec2 GetPointOnPath(float t, const std::vector<glm::vec2> &points, bool closed)
        {
            if (points.empty())
            {
                return glm::vec2(0.0f);
            }

            if (points.size() == 1)
            {
                return points[0];
            }

            float totalLen = GetPathLength(points, closed);
            if (totalLen <= 0.0f)
            {
                return points[0];
            }

            float target = t * totalLen;
            float accum = 0.0f;

            for (size_t i = 0; i < points.size() - 1; ++i)
            {
                const glm::vec2 &a = points[i];
                const glm::vec2 &b = points[i + 1];
                float segLen = glm::length(b - a);

                if (target <= accum + segLen)
                {
                    float frac = (segLen > 0.0f) ? (target - accum) / segLen : 0.0f;
                    frac = glm::clamp(frac, 0.0f, 1.0f);
                    return glm::mix(a, b, frac);
                }
                accum += segLen;
            }

            if (closed)
            {
                const glm::vec2 &a = points.back();
                const glm::vec2 &b = points.front();
                float segLen = glm::length(b - a);
                float frac = (segLen > 0.0f) ? (target - accum) / segLen : 0.0f;
                frac = glm::clamp(frac, 0.0f, 1.0f);
                return glm::mix(a, b, frac);
            }

            return points.back();
        }

        /// Returns the axis-aligned bounding box of a set of points.
        /// @param[out] outCenter  Center of the AABB.
        /// @param[out] outHalfSize  Half-extents of the AABB.
        inline void GetPathAABB(const std::vector<glm::vec2> &points,
                                glm::vec2 &outCenter, glm::vec2 &outHalfSize)
        {
            if (points.empty())
            {
                outCenter = glm::vec2(0.0f);
                outHalfSize = glm::vec2(0.0f);
                return;
            }

            glm::vec2 minPt = points[0];
            glm::vec2 maxPt = points[0];
            for (const auto &p : points)
            {
                minPt = glm::min(minPt, p);
                maxPt = glm::max(maxPt, p);
            }

            outCenter = (minPt + maxPt) * 0.5f;
            outHalfSize = (maxPt - minPt) * 0.5f;
        }

    } // namespace PathUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PATH_UTILS_H_
