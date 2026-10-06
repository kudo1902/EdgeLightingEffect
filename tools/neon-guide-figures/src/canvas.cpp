#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace NeonGuideFigures
{
    Canvas FromImage(const CaptureUtil::Image &image)
    {
        Canvas out(image.width, image.height);
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const unsigned char *p = &image.pixels[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
                out.At(x, y) = glm::vec3(p[0], p[1], p[2]) / 255.0f;
            }
        }
        return out;
    }

    Canvas Crop(const Canvas &source, int x, int y, int w, int h)
    {
        Canvas out(w, h);
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                const int sx = std::clamp(x + i, 0, source.width - 1);
                const int sy = std::clamp(y + j, 0, source.height - 1);
                out.At(i, j) = source.At(sx, sy);
            }
        }
        return out;
    }

    Canvas Magnify(const Canvas &source, int factor)
    {
        Canvas out(source.width * factor, source.height * factor);
        for (int y = 0; y < out.height; ++y)
        {
            for (int x = 0; x < out.width; ++x)
            {
                out.At(x, y) = source.At(x / factor, y / factor);
            }
        }
        return out;
    }

    Canvas Dim(const Canvas &source, float factor)
    {
        Canvas out = source;
        for (glm::vec3 &p : out.pixels)
        {
            p *= factor;
        }
        return out;
    }

    void DrawDisc(Canvas &canvas, const glm::vec2 &centre, float radius, const glm::vec3 &colour, float alpha)
    {
        const int x0 = int(std::floor(centre.x - radius - 1.0f));
        const int x1 = int(std::ceil(centre.x + radius + 1.0f));
        const int y0 = int(std::floor(centre.y - radius - 1.0f));
        const int y1 = int(std::ceil(centre.y + radius + 1.0f));
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const float d = glm::length(glm::vec2(x + 0.5f, y + 0.5f) - centre) - radius;
                canvas.BlendOver(x, y, colour, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    void DrawLine(Canvas &canvas, const glm::vec2 &a, const glm::vec2 &b, float width, const glm::vec3 &colour,
                  float alpha)
    {
        const float half = 0.5f * width;
        const int x0 = int(std::floor(std::min(a.x, b.x) - half - 1.0f));
        const int x1 = int(std::ceil(std::max(a.x, b.x) + half + 1.0f));
        const int y0 = int(std::floor(std::min(a.y, b.y) - half - 1.0f));
        const int y1 = int(std::ceil(std::max(a.y, b.y) + half + 1.0f));
        const glm::vec2 ab = b - a;
        const float len2 = std::max(glm::dot(ab, ab), 1e-6f);
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const glm::vec2 p(x + 0.5f, y + 0.5f);
                const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
                const float d = glm::length(p - (a + ab * t)) - half;
                canvas.BlendOver(x, y, colour, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    void FillRect(Canvas &canvas, int x, int y, int w, int h, const glm::vec3 &colour, float alpha)
    {
        for (int j = y; j < y + h; ++j)
        {
            for (int i = x; i < x + w; ++i)
            {
                canvas.BlendOver(i, j, colour, alpha);
            }
        }
    }

    float Luma255(const glm::vec3 &rgb)
    {
        return 255.0f * (0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b);
    }

    bool SavePNG(const Canvas &canvas, const std::string &path)
    {
        CaptureUtil::Image image;
        image.width = canvas.width;
        image.height = canvas.height;
        image.channels = 4;
        image.pixels.resize(size_t(canvas.width) * size_t(canvas.height) * 4);
        for (size_t i = 0; i < canvas.pixels.size(); ++i)
        {
            const glm::vec3 c = glm::clamp(canvas.pixels[i], 0.0f, 1.0f);
            image.pixels[i * 4 + 0] = (unsigned char)std::lround(c.r * 255.0f);
            image.pixels[i * 4 + 1] = (unsigned char)std::lround(c.g * 255.0f);
            image.pixels[i * 4 + 2] = (unsigned char)std::lround(c.b * 255.0f);
            image.pixels[i * 4 + 3] = 255;
        }
        if (!CaptureUtil::WritePNG(path, image, true))
        {
            std::fprintf(stderr, "neon-guide-figures: could not write %s\n", path.c_str());
            return false;
        }
        std::printf("  %s (%d x %d)\n", path.c_str(), canvas.width, canvas.height);
        return true;
    }

    namespace
    {
        std::string Num(float v)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", v);
            return buf;
        }

        /// Tick labels: integers print without a decimal, everything else
        /// with as few digits as it needs.
        std::string TickLabel(float v)
        {
            char buf[32];
            if (std::fabs(v - std::round(v)) < 1e-4f)
            {
                std::snprintf(buf, sizeof(buf), "%d", int(std::lround(v)));
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "%g", v);
            }
            return buf;
        }
    }

    bool SaveSVG(const Plot &plot, const std::string &path)
    {
        const float W = 720.0f;
        const float H = 360.0f;
        const float left = 64.0f;
        const float right = 180.0f; // legend column
        const float top = 36.0f;
        const float bottom = 52.0f;
        const float pw = W - left - right;
        const float ph = H - top - bottom;
        auto px = [&](float x) { return left + (x - plot.xMin) / (plot.xMax - plot.xMin) * pw; };
        auto py = [&](float y) { return top + (1.0f - (y - plot.yMin) / (plot.yMax - plot.yMin)) * ph; };

        std::ostringstream s;
        s << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
          << "\" viewBox=\"0 0 " << W << " " << H << "\" font-family=\"Helvetica, Arial, sans-serif\">\n";
        s << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
        s << "<text x=\"" << left << "\" y=\"22\" font-size=\"15\" font-weight=\"bold\" fill=\"#222\">"
          << plot.title << "</text>\n";

        // Grid and ticks.
        for (float x : plot.xTicks)
        {
            s << "<line x1=\"" << Num(px(x)) << "\" y1=\"" << top << "\" x2=\"" << Num(px(x)) << "\" y2=\""
              << top + ph << "\" stroke=\"#e6e6e6\"/>\n";
            s << "<text x=\"" << Num(px(x)) << "\" y=\"" << top + ph + 16 << "\" font-size=\"11\" fill=\"#444\" "
              << "text-anchor=\"middle\">" << TickLabel(x) << "</text>\n";
        }
        for (float y : plot.yTicks)
        {
            s << "<line x1=\"" << left << "\" y1=\"" << Num(py(y)) << "\" x2=\"" << left + pw << "\" y2=\""
              << Num(py(y)) << "\" stroke=\"#e6e6e6\"/>\n";
            s << "<text x=\"" << left - 6 << "\" y=\"" << Num(py(y) + 4) << "\" font-size=\"11\" fill=\"#444\" "
              << "text-anchor=\"end\">" << TickLabel(y) << "</text>\n";
        }
        for (float x : plot.markers)
        {
            s << "<line x1=\"" << Num(px(x)) << "\" y1=\"" << top << "\" x2=\"" << Num(px(x)) << "\" y2=\""
              << top + ph << "\" stroke=\"#888\" stroke-dasharray=\"3,3\"/>\n";
        }
        s << "<rect x=\"" << left << "\" y=\"" << top << "\" width=\"" << pw << "\" height=\"" << ph
          << "\" fill=\"none\" stroke=\"#999\"/>\n";

        // Axis labels.
        s << "<text x=\"" << left + pw / 2 << "\" y=\"" << H - 12 << "\" font-size=\"12\" fill=\"#222\" "
          << "text-anchor=\"middle\">" << plot.xLabel << "</text>\n";
        s << "<text transform=\"translate(16," << top + ph / 2 << ") rotate(-90)\" font-size=\"12\" fill=\"#222\" "
          << "text-anchor=\"middle\">" << plot.yLabel << "</text>\n";

        // Series, clipped to the plot box.
        s << "<clipPath id=\"plot\"><rect x=\"" << left << "\" y=\"" << top << "\" width=\"" << pw
          << "\" height=\"" << ph << "\"/></clipPath>\n";
        float legendY = top + 12.0f;
        for (const Series &series : plot.series)
        {
            s << "<polyline clip-path=\"url(#plot)\" fill=\"none\" stroke=\"" << series.colour
              << "\" stroke-width=\"2\"" << (series.dashed ? " stroke-dasharray=\"6,4\"" : "") << " points=\"";
            for (const glm::vec2 &p : series.points)
            {
                s << Num(px(p.x)) << "," << Num(py(p.y)) << " ";
            }
            s << "\"/>\n";
            s << "<line x1=\"" << left + pw + 14 << "\" y1=\"" << legendY - 4 << "\" x2=\"" << left + pw + 38
              << "\" y2=\"" << legendY - 4 << "\" stroke=\"" << series.colour << "\" stroke-width=\"2\""
              << (series.dashed ? " stroke-dasharray=\"6,4\"" : "") << "/>\n";
            s << "<text x=\"" << left + pw + 44 << "\" y=\"" << legendY << "\" font-size=\"12\" fill=\"#222\">"
              << series.label << "</text>\n";
            legendY += 20.0f;
        }
        s << "</svg>\n";

        std::ofstream file(path);
        if (!file)
        {
            std::fprintf(stderr, "neon-guide-figures: could not write %s\n", path.c_str());
            return false;
        }
        file << s.str();
        std::printf("  %s\n", path.c_str());
        return true;
    }
}
