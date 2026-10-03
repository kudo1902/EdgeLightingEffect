#ifndef _NEON_GUIDE_FIGURES_CANVAS_H_
#define _NEON_GUIDE_FIGURES_CANVAS_H_

// CPU-side images and plots for the guide's figures: crops and magnified
// views of renders, the few diagrams drawn without the GPU, and SVG line
// plots of measured pixel rows. Nothing here touches GL.

#include "util/capture-util.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace NeonGuideFigures
{
    using namespace EdgeLighting;

    /// An RGB image with float channels in [0, 1], top row first. Renders come
    /// in as RGBA8 and go out as RGB PNGs; floats in between keep the CPU
    /// drawing (discs, lines, blends) free of rounding.
    typedef struct Canvas
    {
        int width = 0;
        int height = 0;
        std::vector<glm::vec3> pixels;

        Canvas() = default;
        Canvas(int w, int h, const glm::vec3 &fill = glm::vec3(0.0f))
            : width(w), height(h), pixels(size_t(w) * size_t(h), fill)
        {
        }

        bool IsValid() const { return width > 0 && height > 0; }
        glm::vec3 &At(int x, int y) { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
        const glm::vec3 &At(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }

        /// Composite @p colour at coverage @p alpha over the pixel, if inside.
        void BlendOver(int x, int y, const glm::vec3 &colour, float alpha)
        {
            if (x < 0 || y < 0 || x >= width || y >= height || alpha <= 0.0f)
            {
                return;
            }
            glm::vec3 &dst = At(x, y);
            dst = colour * alpha + dst * (1.0f - alpha);
        }
    } Canvas;

    /// RGBA8 readback -> canvas, dropping alpha.
    Canvas FromImage(const CaptureUtil::Image &image);

    Canvas Crop(const Canvas &source, int x, int y, int w, int h);

    /// Nearest-neighbour magnification by @p factor - every source pixel
    /// becomes a factor x factor block, so texels and pixel steps stay visible.
    Canvas Magnify(const Canvas &source, int factor);

    /// Multiply every pixel by @p factor.
    Canvas Dim(const Canvas &source, float factor);

    /// Antialiased filled disc, composited over.
    void DrawDisc(Canvas &canvas, const glm::vec2 &centre, float radius, const glm::vec3 &colour, float alpha);

    /// Antialiased line of @p width px, composited over.
    void DrawLine(Canvas &canvas, const glm::vec2 &a, const glm::vec2 &b, float width, const glm::vec3 &colour,
                  float alpha);

    /// Fill an axis-aligned rectangle, composited over.
    void FillRect(Canvas &canvas, int x, int y, int w, int h, const glm::vec3 &colour, float alpha = 1.0f);

    /// Rec. 709 luma of a pixel, in 0..255.
    float Luma255(const glm::vec3 &rgb);

    /// Write @p canvas as an RGB PNG. Logs the path on success.
    bool SavePNG(const Canvas &canvas, const std::string &path);

    /// One line of a plot.
    typedef struct Series
    {
        std::string label;
        std::string colour; ///< Any SVG colour, e.g. "#1f77b4".
        std::vector<glm::vec2> points;
        bool dashed = false;
    } Series;

    /// A minimal line plot, written as a self-contained SVG with its own white
    /// background so it reads the same in a light or dark Markdown viewer.
    typedef struct Plot
    {
        std::string title;
        std::string xLabel;
        std::string yLabel;
        float xMin = 0.0f;
        float xMax = 1.0f;
        float yMin = 0.0f;
        float yMax = 1.0f;
        std::vector<float> xTicks;
        std::vector<float> yTicks;
        std::vector<float> markers; ///< Vertical guide lines at these x values.
        std::vector<Series> series;
    } Plot;

    bool SaveSVG(const Plot &plot, const std::string &path);
}

#endif
