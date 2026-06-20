#ifndef _EDGE_LIGHTING_COLOR_UTILS_H_
#define _EDGE_LIGHTING_COLOR_UTILS_H_

#include "core/config.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace EdgeLighting
{
    namespace ColorUtils
    {
        inline glm::vec3 RgbToHsv(glm::vec3 c)
        {
            float r = c.r, g = c.g, b = c.b;
            float mx = std::max({r, g, b});
            float mn = std::min({r, g, b});
            float d = mx - mn;
            float h = 0.0f;
            if (d > 1e-10f)
            {
                if (mx == r)
                {
                    h = std::fmod((g - b) / d, 6.0f);
                }
                else if (mx == g)
                {
                    h = (b - r) / d + 2.0f;
                }
                else
                {
                    h = (r - g) / d + 4.0f;
                }

                h /= 6.0f;
                if (h < 0.0f)
                {
                    h += 1.0f;
                }
            }
            return glm::vec3(h, mx > 1e-10f ? d / mx : 0.0f, mx);
        }

        inline glm::vec3 HsvToRgb(glm::vec3 c)
        {
            float h = c.x, s = c.y, v = c.z;
            float r = v, g = v, b = v;
            if (s > 0.0f && v > 0.0f)
            {
                h = (h - std::floor(h)) * 6.0f;
                int i = static_cast<int>(h);
                float f = h - static_cast<float>(i);
                float p = v * (1.0f - s);
                float q = v * (1.0f - s * f);
                float t = v * (1.0f - s * (1.0f - f));
                switch (i)
                {
                case 0:
                {
                    r = v;
                    g = t;
                    b = p;
                    break;
                }
                case 1:
                {
                    r = q;
                    g = v;
                    b = p;
                    break;
                }
                case 2:
                {
                    r = p;
                    g = v;
                    b = t;
                    break;
                }
                case 3:
                {
                    r = p;
                    g = q;
                    b = v;
                    break;
                }
                case 4:
                {
                    r = t;
                    g = p;
                    b = v;
                    break;
                }
                default:
                {
                    r = v;
                    g = p;
                    b = q;
                    break;
                }
                }
            }
            return glm::vec3(r, g, b);
        }

        inline glm::vec3 BlendStops(glm::vec3 a, glm::vec3 b, float t, BlendSpace space)
        {
            if (space == BlendSpace::HSV)
            {
                glm::vec3 ha = RgbToHsv(a);
                glm::vec3 hb = RgbToHsv(b);
                float dh = hb.x - ha.x;
                if (dh > 0.5f)
                {
                    dh -= 1.0f;
                }
                if (dh < -0.5f)
                {
                    dh += 1.0f;
                }
                return HsvToRgb(glm::vec3(ha.x + dh * t,
                                          glm::mix(ha.y, hb.y, t),
                                          glm::mix(ha.z, hb.z, t)));
            }
            return glm::mix(a, b, t);
        }

        inline glm::vec3 SampleStops(float pos,
                                     const std::vector<ColorStop> &stops,
                                     BlendSpace blendSpace)
        {
            int count = static_cast<int>(stops.size());
            if (count <= 0)
            {
                return glm::vec3(1.0f);
            }
            if (count == 1)
            {
                return glm::vec3(stops[0].color);
            }
            if (count == 2)
            {
                float tri = 1.0f - std::abs(2.0f * pos - 1.0f);
                return BlendStops(glm::vec3(stops[0].color),
                                  glm::vec3(stops[1].color), tri, blendSpace);
            }
            for (int i = 0; i < count; i++)
            {
                int next = (i + 1 < count) ? i + 1 : 0;
                float a = stops[i].position;
                float b = stops[next].position;
                if (next != 0)
                {
                    if (pos >= a && pos < b)
                    {
                        float t = (pos - a) / std::max(b - a, 0.0001f);
                        return BlendStops(glm::vec3(stops[i].color),
                                          glm::vec3(stops[next].color), t, blendSpace);
                    }
                }
                else
                {
                    float wrapLen = (1.0f - a) + b;
                    if (pos >= a)
                    {
                        float t = (pos - a) / std::max(wrapLen, 0.0001f);
                        return BlendStops(glm::vec3(stops[i].color),
                                          glm::vec3(stops[next].color), t, blendSpace);
                    }
                    if (pos < b)
                    {
                        float t = ((1.0f - a) + pos) / std::max(wrapLen, 0.0001f);
                        return BlendStops(glm::vec3(stops[i].color),
                                          glm::vec3(stops[next].color), t, blendSpace);
                    }
                }
            }
            return glm::vec3(stops[0].color);
        }

    } // namespace ColorUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_COLOR_UTILS_H_
