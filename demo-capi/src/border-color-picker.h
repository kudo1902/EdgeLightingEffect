#ifndef _EDGE_LIGHTING_CAPI_DEMO_BORDER_COLOR_PICKER_H_
#define _EDGE_LIGHTING_CAPI_DEMO_BORDER_COLOR_PICKER_H_

#include "edge-lighting-capi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EdgeLightingCapiDemo
{
    struct SampledStop
    {
        float position;
        float r, g, b, a;
    };

    // Samples colours from the border of an image and produces stops that
    // can be pushed into the effect via el_effect_set_color_stop_*.
    //
    // Sampling walks the target rectangle's perimeter (not the image's), so
    // aspect mismatches don't cause the picker and the shader to disagree.
    class BorderColorPicker
    {
    public:
        bool Load(const std::string &path);

        bool HasImage() const { return !mPixels.empty(); }
        int Width() const { return mWidth; }
        int Height() const { return mHeight; }
        const std::string &Path() const { return mPath; }
        const std::vector<uint8_t> &Pixels() const { return mPixels; }

        std::vector<SampledStop> SampleBorder(int stopCount, el_winding_e winding,
                                              int rectWidth, int rectHeight) const;

    private:
        int mWidth = 0;
        int mHeight = 0;
        std::vector<uint8_t> mPixels;
        std::string mPath;
    };
} // namespace EdgeLightingCapiDemo

#endif
