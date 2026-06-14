#ifndef _CONTOUR_TRACER_H_
#define _CONTOUR_TRACER_H_

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace EdgeLighting
{
    /// Trace the outermost contour of a binary mask using Moore-Neighbor boundary following.
    /// @param pixels          RGBA pixel data (4 bytes per pixel).
    /// @param imgW, imgH      Image dimensions in pixels.
    /// @param rectW, rectH    Destination rectangle size in app space (for pixel -> app conversion).
    /// @param alphaThreshold  Alpha values >= this are considered foreground.
    /// @param maxPoints       Maximum contour points. If exceeded, points are uniformly resampled.
    /// @return Contour points in app space (top-left = 0,0, +Y down), or empty if no foreground.
    std::vector<glm::vec2> TraceOutermostContour(
        const uint8_t *pixels, int imgW, int imgH,
        float rectW, float rectH,
        uint8_t alphaThreshold = 128,
        int maxPoints = 512);

} // namespace EdgeLighting

#endif // _CONTOUR_TRACER_H_
