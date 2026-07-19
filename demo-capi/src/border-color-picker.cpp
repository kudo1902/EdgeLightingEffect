#include "border-color-picker.h"

#include "stb/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace EdgeLightingCapiDemo
{
    namespace
    {
        // Walk a sharp-cornered W×H rectangle from the top-left in the given
        // winding direction; return (u, v) ∈ [0,1] at parameter s ∈ [0,1).
        // Corner arcs are ignored - good enough for small radii.
        void RectPerimeterUV(float s, float W, float H, el_winding_e winding,
                             float &outU, float &outV)
        {
            s -= std::floor(s);
            const float P = 2.0f * (W + H);
            const float d = s * P;

            float x = 0.0f, y = 0.0f;
            if (winding == EL_WINDING_CLOCKWISE)
            {
                if (d < W)
                {
                    x = d;
                    y = 0.0f;
                }
                else if (d < W + H)
                {
                    x = W;
                    y = d - W;
                }
                else if (d < 2.0f * W + H)
                {
                    x = W - (d - W - H);
                    y = H;
                }
                else
                {
                    x = 0.0f;
                    y = H - (d - 2.0f * W - H);
                }
            }
            else
            {
                if (d < H)
                {
                    x = 0.0f;
                    y = d;
                }
                else if (d < H + W)
                {
                    x = d - H;
                    y = H;
                }
                else if (d < 2.0f * H + W)
                {
                    x = W;
                    y = H - (d - H - W);
                }
                else
                {
                    x = W - (d - 2.0f * H - W);
                    y = 0.0f;
                }
            }
            outU = x / W;
            outV = y / H;
        }
    } // namespace

    bool BorderColorPicker::Load(const std::string &path)
    {
        int w = 0, h = 0, n = 0;
        unsigned char *raw = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!raw)
        {
            std::fprintf(stderr, "BorderColorPicker: failed to load '%s' (%s)\n",
                         path.c_str(),
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown");
            return false;
        }
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        mPixels.assign(raw, raw + bytes);
        stbi_image_free(raw);
        mWidth = w;
        mHeight = h;
        mPath = path;
        std::printf("BorderColorPicker: loaded '%s' (%dx%d)\n", path.c_str(), w, h);
        return true;
    }

    std::vector<SampledStop>
    BorderColorPicker::SampleBorder(int stopCount, el_winding_e winding,
                                    int rectWidth, int rectHeight) const
    {
        std::vector<SampledStop> out;
        if (!HasImage() || stopCount < 1)
        {
            return out;
        }
        out.reserve(static_cast<size_t>(stopCount));

        const float rW = static_cast<float>(std::max(rectWidth, 1));
        const float rH = static_cast<float>(std::max(rectHeight, 1));
        const float invN = 1.0f / static_cast<float>(stopCount);

        const int insetX = mWidth >= 3 ? 1 : 0;
        const int insetY = mHeight >= 3 ? 1 : 0;

        for (int i = 0; i < stopCount; ++i)
        {
            const float s = static_cast<float>(i) * invN;
            float u = 0.0f, v = 0.0f;
            RectPerimeterUV(s, rW, rH, winding, u, v);

            int ix = static_cast<int>(u * static_cast<float>(mWidth));
            int iy = static_cast<int>(v * static_cast<float>(mHeight));
            ix = std::clamp(ix, insetX, mWidth - 1 - insetX);
            iy = std::clamp(iy, insetY, mHeight - 1 - insetY);

            const size_t idx = (static_cast<size_t>(iy) * static_cast<size_t>(mWidth) +
                                static_cast<size_t>(ix)) *
                               4u;
            SampledStop stop{};
            stop.position = s;
            stop.r = mPixels[idx + 0] / 255.0f;
            stop.g = mPixels[idx + 1] / 255.0f;
            stop.b = mPixels[idx + 2] / 255.0f;
            stop.a = mPixels[idx + 3] / 255.0f;
            out.push_back(stop);
        }
        return out;
    }
} // namespace EdgeLightingCapiDemo
