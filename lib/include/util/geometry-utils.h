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

        /// Packs @p count loop-sample positions into an RGBA8 buffer (4 bytes each)
        /// for upload as a byte data texture — Tizen/Mali only guarantee byte
        /// textures, so positions can't be stored as float. Each coord is encoded
        /// as 16-bit fixed point (x -> R:hi G:lo, y -> B:hi A:lo), normalised over
        /// [-maxCoord, maxCoord] where @p outMaxCoord is the largest |coordinate|.
        /// The shader decodes with `coord = (2*(hi + lo/255) - 1) * maxCoord`.
        /// ~16-bit precision is far finer than a pixel. @p outMaxCoord receives
        /// the value for the uSampleMaxCoord uniform.
        inline void PackLoopSamplesRGBA8(const std::vector<glm::vec2> &samples, int count,
                                         std::vector<unsigned char> &out, float &outMaxCoord)
        {
            float maxCoord = 1.0f;
            for (int i = 0; i < count; ++i)
            {
                maxCoord = std::max(maxCoord, std::max(std::fabs(samples[i].x), std::fabs(samples[i].y)));
            }
            maxCoord *= 1.0009766f; // small margin so the normalised value never hits exactly 1
            outMaxCoord = maxCoord;

            float inv = 1.0f / (2.0f * maxCoord);
            out.resize(static_cast<size_t>(count) * 4);
            auto pack = [](float n, unsigned char &hi, unsigned char &lo)
            {
                n = std::min(std::max(n, 0.0f), 1.0f);
                float v = n * 255.0f;
                int h = static_cast<int>(std::floor(v));
                int l = static_cast<int>(std::lround((v - static_cast<float>(h)) * 255.0f));
                if (l > 255)
                {
                    l = 0;
                    if (h < 255)
                    {
                        ++h;
                    }
                }
                hi = static_cast<unsigned char>(std::min(std::max(h, 0), 255));
                lo = static_cast<unsigned char>(std::min(std::max(l, 0), 255));
            };
            for (int i = 0; i < count; ++i)
            {
                // map coord in [-maxCoord, maxCoord] to [0, 1], then 16-bit pack.
                pack((samples[i].x + maxCoord) * inv, out[i * 4 + 0], out[i * 4 + 1]); // x -> R, G
                pack((samples[i].y + maxCoord) * inv, out[i * 4 + 2], out[i * 4 + 3]); // y -> B, A
            }
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
