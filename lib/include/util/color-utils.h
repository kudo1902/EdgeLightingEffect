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

        inline glm::vec3 RgbToHsl(glm::vec3 c)
        {
            float r = c.r, g = c.g, b = c.b;
            float mx = std::max({r, g, b});
            float mn = std::min({r, g, b});
            float l = 0.5f * (mx + mn);
            float h = 0.0f, s = 0.0f;
            float d = mx - mn;
            if (d > 1e-10f)
            {
                s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
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
            return glm::vec3(h, s, l);
        }

        inline glm::vec3 HslToRgb(glm::vec3 c)
        {
            float h = c.x, s = c.y, l = c.z;
            if (s < 1e-10f)
            {
                return glm::vec3(l);
            }
            auto hueToRgb = [](float p, float q, float t)
            {
                if (t < 0.0f)
                {
                    t += 1.0f;
                }
                if (t > 1.0f)
                {
                    t -= 1.0f;
                }
                if (t < 1.0f / 6.0f)
                {
                    return p + (q - p) * 6.0f * t;
                }
                if (t < 0.5f)
                {
                    return q;
                }
                if (t < 2.0f / 3.0f)
                {
                    return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
                }
                return p;
            };
            float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
            float p = 2.0f * l - q;
            return glm::vec3(hueToRgb(p, q, h + 1.0f / 3.0f),
                             hueToRgb(p, q, h),
                             hueToRgb(p, q, h - 1.0f / 3.0f));
        }

        /// Blend two stops. The colour blends in @p space; alpha always blends
        /// linearly, because it is an emission scale (see @ref SampleRing)
        /// rather than a colour channel - there is no hue-space analogue for
        /// it, and routing it through HSV/HSL would make a fade depend on the
        /// blend space.
        inline glm::vec4 BlendStops(glm::vec4 a, glm::vec4 b, float t, BlendSpace space)
        {
            float alpha = glm::mix(a.a, b.a, t);
            if (space == BlendSpace::HSV || space == BlendSpace::HSL)
            {
                glm::vec3 ha = (space == BlendSpace::HSV) ? RgbToHsv(glm::vec3(a)) : RgbToHsl(glm::vec3(a));
                glm::vec3 hb = (space == BlendSpace::HSV) ? RgbToHsv(glm::vec3(b)) : RgbToHsl(glm::vec3(b));
                float dh = hb.x - ha.x;
                if (dh > 0.5f)
                {
                    dh -= 1.0f;
                }
                if (dh < -0.5f)
                {
                    dh += 1.0f;
                }
                glm::vec3 mid(ha.x + dh * t,
                              glm::mix(ha.y, hb.y, t),
                              glm::mix(ha.z, hb.z, t));
                glm::vec3 rgb = (space == BlendSpace::HSV) ? HsvToRgb(mid) : HslToRgb(mid);
                return glm::vec4(rgb, alpha);
            }
            return glm::vec4(glm::mix(glm::vec3(a), glm::vec3(b), t), alpha);
        }

        /// @p stops ordered ascending by position, as @ref SampleRing and
        /// @ref SampleSpan both require.
        ///
        /// Call this once when baking a LUT, not per sample - it copies. The
        /// sort is stable, so stops sharing a position keep their authored
        /// order (which decides which one wins the hard edge between them),
        /// and an already-sorted list comes back byte-identical.
        ///
        /// Ordering is not something a caller can be relied on to maintain:
        /// nothing in @ref NeonConfig enforces it, the debug UI lets a stop be
        /// dragged past its neighbour, and a @c ColorStopField::POSITION
        /// animation can drive two stops through each other mid-playback. An
        /// unsorted ring does not fail loudly - it renders a silently wrong
        /// gradient (a 4-stop ring authored out of order disagreed with the
        /// sorted one at 7 of 8 sample positions and never reached two of its
        /// four colours), so the renderers sort at the bake instead of trusting
        /// the input.
        inline std::vector<ColorStop> SortStops(std::vector<ColorStop> stops)
        {
            std::stable_sort(stops.begin(), stops.end(),
                             [](const ColorStop &a, const ColorStop &b)
                             {
                                 return a.position < b.position;
                             });
            return stops;
        }

        /// Sample a CYCLIC stop ring at normalised perimeter position @p pos:
        /// past the last stop the walk wraps back round to the first.
        ///
        /// The counterpart is @ref SampleSpan, which holds its end colours
        /// instead. Neither is the "default" - they describe two different data
        /// shapes, and picking the wrong one is silent:
        ///
        ///   ring - @c NeonConfig::colorStops. Genuinely circular (position 1.0
        ///          IS position 0.0), baked into a @c GL_REPEAT texture.
        ///   span - a per-arc or per-segment row, laid head-to-tail across a
        ///          finite stretch and baked @c CLAMP_TO_EDGE.
        ///
        /// Both are named for their domain so the mismatch is visible at the
        /// call site; baking a span with this function is what produced the
        /// reversed gradients described in @ref SampleSpan.
        ///
        /// Returns straight (non-premultiplied) RGBA. The @c .a channel is the
        /// stop's emission scale at this position: the renderers bake it into
        /// the LUT alpha channel and the neon shaders apply it to the emission
        /// MAGNITUDE, not to the colour - see the alpha gather in neon.frag.
        /// Premultiplying here would be wrong: the shader normalises the
        /// gathered colour to unit magnitude (@c acc/wsumLit), which would
        /// divide any alpha folded into RGB straight back out.
        ///
        /// @note @p stops must be sorted ascending by @c position - the ring
        ///       walk below assumes it, and an unsorted list yields a silently
        ///       distorted gradient rather than an error. Run them through
        ///       @ref SortStops first; the renderers do this once per LUT bake,
        ///       so anything reaching the shaders is already ordered.
        inline glm::vec4 SampleRing(float pos,
                                    const std::vector<ColorStop> &stops,
                                    BlendSpace blendSpace)
        {
            int count = static_cast<int>(stops.size());
            if (count <= 0)
            {
                return glm::vec4(1.0f);
            }
            if (count == 1)
            {
                return stops[0].color;
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
                        return BlendStops(stops[i].color, stops[next].color, t, blendSpace);
                    }
                }
                else
                {
                    float wrapLen = (1.0f - a) + b;
                    if (pos >= a)
                    {
                        float t = (pos - a) / std::max(wrapLen, 0.0001f);
                        return BlendStops(stops[i].color, stops[next].color, t, blendSpace);
                    }
                    if (pos < b)
                    {
                        float t = ((1.0f - a) + pos) / std::max(wrapLen, 0.0001f);
                        return BlendStops(stops[i].color, stops[next].color, t, blendSpace);
                    }
                }
            }
            return stops[0].color;
        }

        /// Sample a NON-cyclic head-to-tail span at @p pos, holding the first
        /// and last stop colours outside the stops' own range.
        ///
        /// The counterpart of @ref SampleRing - see there for which data shape
        /// takes which. This one is for the per-segment and per-arc atlas rows,
        /// which are laid out head-to-tail across a finite stretch (see
        /// @ref SegmentBoost) and sampled @c CLAMP_TO_EDGE.
        ///
        /// Baking those rows with the cyclic sampler put the wrap interval
        /// inside the visible span: stops at 0.2 and 0.8 rendered the head of
        /// the row (t = 0) as a blend of the LAST and FIRST colours rather than
        /// the first, and ramped the tail back toward the head colour instead
        /// of holding it. A two-stop white-to-red span came out white, red,
        /// then white again. The two samplers agree exactly at the midpoint,
        /// which is why it was easy to miss - the error lives at the ends,
        /// where the emission is dimmest.
        ///
        /// @note @p stops must be sorted ascending by @c position - run them
        ///       through @ref SortStops first, exactly as @ref SampleRing
        ///       requires.
        inline glm::vec4 SampleSpan(float pos,
                                    const std::vector<ColorStop> &stops,
                                    BlendSpace blendSpace)
        {
            int count = static_cast<int>(stops.size());
            if (count <= 0)
            {
                return glm::vec4(1.0f);
            }
            if (count == 1 || pos <= stops[0].position)
            {
                return stops[0].color;
            }
            if (pos >= stops[count - 1].position)
            {
                return stops[count - 1].color;
            }
            for (int i = 0; i + 1 < count; i++)
            {
                float a = stops[i].position;
                float b = stops[i + 1].position;
                if (pos >= a && pos < b)
                {
                    float t = (pos - a) / std::max(b - a, 0.0001f);
                    return BlendStops(stops[i].color, stops[i + 1].color, t, blendSpace);
                }
            }
            return stops[count - 1].color;
        }

    } // namespace ColorUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_COLOR_UTILS_H_
