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
            // 2+ stops: walk the ring honouring each stop's position, wrapping
            // from the last stop back to the first across the 0/1 seam. (No
            // special 2-stop case: a triangle there would ignore positions.)
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

        // ------------------------------------------------------------------
        // Per-sample neon gather data (colour + emission weight)
        // ------------------------------------------------------------------

        namespace Detail
        {
            /// Box window: 1 inside [−half, half], 0 outside. @p dist is the
            /// |offset| from the centre. (The spatial gather + sample spacing
            /// smooth the edge, so no explicit feather is needed.)
            inline float SpanCoverage(float dist, float half)
            {
                return dist <= half ? 1.0f : 0.0f;
            }

            inline float Fract(float x) { return x - std::floor(x); }
        } // namespace Detail

        /// Precomputes, for each gather sample, the colour and emission weight the
        /// shader needs — packed as vec4(rgb = colour, a = weight). This is the
        /// single source the shader reads (uSampleData[]); it replaces both a gate
        /// array and the gradient LUT texture, so per-segment colour, a per-segment
        /// gradient rotation, and a nested per-segment spot all cost nothing in the
        /// fragment loop.
        ///
        /// The effect is defined entirely by @c neon.segments — there is no global
        /// ring. A sample is dark unless a segment covers it:
        ///   - Inside a segment: that segment's cyclic gradient across its local
        ///     [0, 1] (stop positions scale with the span; a full ring wraps across
        ///     the 0/1 seam). Empty stops = solid white.
        ///   - Under a segment's spot: the spot's colour blended in (or the segment
        ///     colour if the spot has none).
        ///
        /// @c weight = brightness * coverage (+ spot). It scales filament/halo/bloom
        /// brightness; the brightest segment wins the colour on overlap. The shader
        /// caps the weight at coverage (min(w,1)) for the filament GATE only, so
        /// brightening a segment never extends its lit length past its ends.
        ///
        /// @p sampleCount must match the shader's gather count so si = i/count
        /// lines up with the per-sample position the shader reconstructs.
        inline void BuildSampleData(const NeonConfig &neon, float time, int sampleCount,
                                    std::vector<glm::vec4> &out)
        {
            out.resize(sampleCount);

            for (int i = 0; i < sampleCount; ++i)
            {
                float si = static_cast<float>(i) / static_cast<float>(sampleCount);

                // Dark unless a segment covers this sample.
                float weight = 0.0f; // emission weight of the brightest segment here
                glm::vec3 colour = glm::vec3(0.0f);

                for (const LitSegment &seg : neon.segments)
                {
                    // position is the START; the span runs [position, position+length].
                    // Work from the centre (wrapped into [0,1)) for a symmetric
                    // wrap-aware coverage window.
                    float centre = Detail::Fract(seg.position + seg.length * 0.5f);
                    float dsi = std::fabs(si - centre);
                    dsi = std::min(dsi, 1.0f - dsi);
                    float cover = Detail::SpanCoverage(dsi, seg.length * 0.5f);
                    if (cover <= 0.0f)
                    {
                        continue;
                    }

                    float segWeight = seg.brightness * cover;

                    // Local position within the span [0, 1] (wrap-aware signed offset).
                    float off = si - centre;
                    if (off > 0.5f)
                    {
                        off -= 1.0f;
                    }
                    if (off < -0.5f)
                    {
                        off += 1.0f;
                    }
                    float local = std::min(std::max(0.5f + off / std::max(seg.length, 1e-4f), 0.0f), 1.0f);

                    // The segment gradient is cyclic: stop positions map across
                    // the span (local) and the last stop wraps back to the first.
                    // Fract keeps the rotated position bounded, so the full ring,
                    // partial spans, and rotation are all handled the same way.
                    // Subtract the rotation so a positive hueRotationRate scrolls
                    // the colours WITH the winding (CW winding -> CW movement).
                    glm::vec3 segColour = SampleStops(
                        Detail::Fract(local - time * seg.hueRotationRate),
                        seg.colorStops, seg.blendSpace);

                    // Nested spot in segment-local [0, 1]: spot.position is its
                    // START, covering [position, position + length].
                    if (seg.spot.boost > 0.0f)
                    {
                        float spotCentre = seg.spot.position + seg.spot.length * 0.5f;
                        float spotCover = Detail::SpanCoverage(std::fabs(local - spotCentre),
                                                               seg.spot.length * 0.5f);
                        spotCover *= cover; // confine the accent to the segment
                        if (spotCover > 0.0f)
                        {
                            segWeight += seg.spot.boost * spotCover;
                            if (!seg.spot.colorStops.empty())
                            {
                                // Spot-local [0, 1] (cyclic, same as the segment):
                                // positions scale with the spot's length.
                                float sLocal = (local - seg.spot.position) / std::max(seg.spot.length, 1e-4f);
                                glm::vec3 spotColour = SampleStops(Detail::Fract(sLocal),
                                                                   seg.spot.colorStops, seg.spot.blendSpace);
                                segColour = glm::mix(segColour, spotColour, spotCover);
                            }
                        }
                    }

                    // Brightest segment wins. brightness (segment) + spot boost
                    // feed the WEIGHT, which scales filament/halo/bloom. The
                    // shader caps the weight at coverage for the filament GATE
                    // (min(w,1)), so brightening a segment never extends its lit
                    // length past its ends.
                    if (segWeight > weight)
                    {
                        weight = segWeight;
                        colour = segColour;
                    }
                }

                out[i] = glm::vec4(colour, weight);
            }
        }

    } // namespace ColorUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_COLOR_UTILS_H_
