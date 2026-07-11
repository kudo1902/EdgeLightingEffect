#ifndef _EDGE_LIGHTING_DEMO_BORDER_COLOR_PICKER_H_
#define _EDGE_LIGHTING_DEMO_BORDER_COLOR_PICKER_H_

#include "core/config.h"

#include <cstdint>
#include <string>
#include <vector>

/// Samples colours from the border of an image and produces ColorStops that
/// can be dropped into NeonConfig::colorStops.
///
/// The sample walk starts at the top-left corner of the image and follows the
/// same winding direction as EdgeLighting::RectGeometry::winding, so stop i's
/// position (i / stopCount) maps to roughly the same place on the rendered
/// rounded rectangle's perimeter as it does on the image border.
class BorderColorPicker
{
public:
    /// Load an image from disk (PNG/JPG/BMP/TGA/...). Returns true on success;
    /// leaves the previous image intact on failure.
    bool Load(const std::string &path);

    /// True if a valid image is currently loaded.
    bool HasImage() const { return !mPixels.empty(); }

    int Width() const { return mWidth; }
    int Height() const { return mHeight; }

    /// Path used for the most recent successful Load().
    const std::string &Path() const { return mPath; }

    /// Raw RGBA8 pixel buffer, row-major, top-left origin. width*height*4 bytes.
    const std::vector<uint8_t> &Pixels() const { return mPixels; }

    /// Sample @p stopCount colours from the image so each stop's colour matches
    /// what will be rendered under it when the image is displayed inside a
    /// @p rectWidth × @p rectHeight rectangle (as the demo backdrop does).
    ///
    /// The walk is parameterised on the *target rectangle's* perimeter — not
    /// the image's — so aspect-ratio differences don't cause the picker and
    /// the shader to disagree. For each i in [0, stopCount) at parameter
    /// t = i/stopCount, we find the point (rx, ry) on the rect perimeter,
    /// then look up the image at (rx/rectW × imgW, ry/rectH × imgH).
    ///
    /// Corner arcs are ignored (rect treated as sharp) — good enough when the
    /// corner radius is a small fraction of the rect's dimensions.
    /// Returns empty if no image is loaded or stopCount < 1.
    std::vector<EdgeLighting::ColorStop>
    SampleBorder(int stopCount, EdgeLighting::Winding winding,
                 int rectWidth, int rectHeight) const;

private:
    int mWidth = 0;
    int mHeight = 0;
    std::vector<uint8_t> mPixels; ///< RGBA8, width*height*4 bytes.
    std::string mPath;
};

#endif // _EDGE_LIGHTING_DEMO_BORDER_COLOR_PICKER_H_
