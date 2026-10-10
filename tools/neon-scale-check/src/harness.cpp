#include "harness.h"

#include "gl/gl-header.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "util/capture-util.h"
#include "pass-recorder.h"
// stb_image and stb_image_write are compiled into libedge-lighting
// (lib/src/util/stb-image.cpp); only the declarations are needed here.
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace NeonScaleCheck
{
    namespace
    {
        GLFWwindow *gWindow = nullptr;

        /// The page's clear colour, rgb(5, 5, 8).
        const float CLEAR_R = 5.0f / 255.0f;
        const float CLEAR_G = 5.0f / 255.0f;
        const float CLEAR_B = 8.0f / 255.0f;
        const double CLEAR_LUMA = 0.2126 * 5.0 + 0.7152 * 5.0 + 0.0722 * 8.0;

        /// A pixel counts as lit when its brightest channel is more than 16
        /// levels above the clear's brightest (8) in either image.
        const int LIT_THRESHOLD = 24;

        /// The page's error heatmap: one colour per error level 0-64,
        /// saturating at 64, with 0 and 1 drawn black. Extracted from the first
        /// version's own *_diff.png images against recomputed errors.
        const unsigned char RAMP[65][3] = {
            {0, 0, 0}, {0, 0, 0}, {6, 2, 13}, {9, 3, 20}, {13, 3, 26}, {16, 4, 33}, {19, 5, 39},
            {22, 6, 46}, {25, 7, 53}, {28, 8, 59}, {31, 9, 66}, {34, 9, 72}, {38, 10, 79},
            {42, 11, 84}, {49, 13, 86}, {57, 15, 88}, {64, 17, 90}, {72, 19, 91}, {79, 20, 93},
            {87, 22, 95}, {95, 24, 96}, {102, 26, 98}, {110, 28, 100}, {117, 29, 102},
            {125, 31, 103}, {132, 33, 105}, {140, 36, 104}, {147, 40, 100}, {154, 45, 95},
            {161, 49, 91}, {168, 53, 86}, {175, 58, 82}, {183, 62, 78}, {190, 66, 73},
            {197, 71, 69}, {204, 75, 64}, {211, 80, 60}, {218, 84, 55}, {225, 88, 51},
            {229, 95, 49}, {231, 103, 48}, {232, 111, 47}, {234, 119, 46}, {236, 127, 45},
            {238, 135, 45}, {239, 143, 44}, {241, 151, 43}, {243, 159, 42}, {245, 167, 42},
            {246, 175, 41}, {248, 183, 40}, {250, 191, 39}, {250, 197, 47}, {250, 202, 57},
            {250, 207, 66}, {251, 211, 76}, {251, 216, 86}, {251, 221, 96}, {251, 226, 105},
            {251, 231, 115}, {251, 236, 125}, {252, 240, 135}, {252, 245, 144}, {252, 250, 154},
            {252, 255, 164},
        };

        double Luma(const RGB &image, size_t pixel)
        {
            return 0.2126 * image[pixel * 3] + 0.7152 * image[pixel * 3 + 1] + 0.0722 * image[pixel * 3 + 2];
        }
    }

    void InitGL()
    {
        if (!glfwInit())
        {
            std::fprintf(stderr, "neon-scale-check: glfwInit failed\n");
            std::exit(2);
        }
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        gWindow = glfwCreateWindow(64, 64, "neon-scale-check", nullptr, nullptr);
        if (!gWindow)
        {
            std::fprintf(stderr, "neon-scale-check: could not create a GL 3.3 core context\n");
            std::exit(2);
        }
        glfwMakeContextCurrent(gWindow);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        {
            std::fprintf(stderr, "neon-scale-check: GLAD could not load GL\n");
            std::exit(2);
        }
        std::fprintf(stderr, "GL renderer: %s\n", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    }

    void ShutdownGL()
    {
        glfwTerminate();
    }

    std::string RendererName()
    {
        const GLubyte *name = glGetString(GL_RENDERER);
        return name ? std::string(reinterpret_cast<const char *>(name)) : std::string("unknown");
    }

    void CreateEffect(EdgeLightingEffect &effect)
    {
        effect.Initialize();
        if (!effect.AddRenderer(RendererLayer::NEON))
        {
            std::fprintf(stderr, "neon-scale-check: the neon renderer failed to initialise\n");
            std::exit(2);
        }
    }

    RGB Render(EdgeLightingEffect &effect, const Config &config)
    {
        effect.SetConfig(config);
        effect.Update(0.0f);
        OffscreenCapture capture;
        capture.Begin(FRAME_WIDTH, FRAME_HEIGHT);
        // The STEADY frame, not the first: at scale 1.0 the frame a config
        // changes on draws pass 1 directly, and the hue-invariant field takes
        // over from the next (NeonRenderer's pass 1f / 1c, I46). What a host
        // shows is the second, so that is what is measured - drawn over a
        // fresh clear, so the first frame's glow is not composited twice.
        glClearColor(CLEAR_R, CLEAR_G, CLEAR_B, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
        glClear(GL_COLOR_BUFFER_BIT);
        effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
        CaptureUtil::Image image;
        capture.Read(image);
        capture.End();

        RGB rgb(size_t(FRAME_WIDTH) * FRAME_HEIGHT * 3);
        for (size_t i = 0, n = size_t(FRAME_WIDTH) * FRAME_HEIGHT; i < n; ++i)
        {
            rgb[i * 3] = image.pixels[i * 4];
            rgb[i * 3 + 1] = image.pixels[i * 4 + 1];
            rgb[i * 3 + 2] = image.pixels[i * 4 + 2];
        }
        return rgb;
    }

    bool ParseTimeMode(const char *name, TimeMode &mode)
    {
        static const TimeMode MODES[] = {TimeMode::STILL, TimeMode::HUE, TimeMode::INTENSITY, TimeMode::ARC_WIPE,
                                         TimeMode::SEGMENT_TRAVEL, TimeMode::LIGHTS, TimeMode::RESIZE};
        for (TimeMode m : MODES)
        {
            if (std::strcmp(name, TimeModeName(m)) == 0)
            {
                mode = m;
                return true;
            }
        }
        return false;
    }

    const char *TimeModeName(TimeMode mode)
    {
        switch (mode)
        {
        case TimeMode::STILL:
        {
            return "still";
        }
        case TimeMode::HUE:
        {
            return "hue";
        }
        case TimeMode::INTENSITY:
        {
            return "intensity";
        }
        case TimeMode::ARC_WIPE:
        {
            return "arc-wipe";
        }
        case TimeMode::SEGMENT_TRAVEL:
        {
            return "segment-travel";
        }
        case TimeMode::LIGHTS:
        {
            return "lights";
        }
        case TimeMode::RESIZE:
        {
            return "resize";
        }
        }
        return "still";
    }

    namespace
    {
        /// @p config as @p mode starts it: the hue rotating, a segment to
        /// move when the scene has none, or @p lights in place of the scene's
        /// own. The other modes start from the scene as it is.
        Config TimeModeStart(const Config &config, TimeMode mode, const LightCounts &lights)
        {
            Config c = config;
            if (mode == TimeMode::HUE || mode == TimeMode::LIGHTS)
            {
                c.neon.hueRotationRate = 0.5f;
            }
            if (mode == TimeMode::LIGHTS)
            {
                c.neon.arcs.clear();
                if (lights.arcs <= 0)
                {
                    c.neon.arcs.push_back(Arc{});
                }
                for (int i = 0; i < lights.arcs; ++i)
                {
                    Arc a;
                    a.start = float(i) / float(lights.arcs);
                    a.length = 0.6f / float(lights.arcs);
                    c.neon.arcs.push_back(a);
                }
                c.neon.segmentBoosts.clear();
                c.neon.preservedSegmentBoosts.clear();
                for (int i = 0; i < lights.segments; ++i)
                {
                    SegmentBoost s;
                    s.position = (float(i) + 0.5f) / float(lights.segments);
                    s.length = 0.1f;
                    s.boost = 1.0f;
                    c.neon.segmentBoosts.push_back(s);
                }
            }
            if (mode == TimeMode::SEGMENT_TRAVEL && c.neon.segmentBoosts.empty())
            {
                SegmentBoost segment;
                segment.position = 0.1f;
                segment.length = 0.1f;
                segment.boost = 1.0f;
                c.neon.segmentBoosts.push_back(segment);
            }
            return c;
        }

        /// @p start with @p mode's change for frame @p frame written into it.
        /// Every frame differs from the one before, so each SetConfig is a
        /// real config change. Not called for STILL or HUE: the hue moves with
        /// the clock, not the config.
        Config TimeModeFrame(const Config &start, TimeMode mode, int frame, const LightCounts &lights)
        {
            Config c = start;
            const float f = static_cast<float>(frame);
            if (mode == TimeMode::LIGHTS)
            {
                for (int i = 0; i < lights.arcs; ++i)
                {
                    c.neon.arcs[size_t(i)].length =
                        (0.3f + 0.6f * (0.5f + 0.5f * std::sin(0.05f * f + 1.3f * float(i)))) / float(lights.arcs);
                }
                for (int i = 0; i < lights.segments; ++i)
                {
                    c.neon.segmentBoosts[size_t(i)].length =
                        0.05f + 0.10f * (0.5f + 0.5f * std::sin(0.05f * f + 1.3f * float(i)));
                }
                return c;
            }
            if (mode == TimeMode::INTENSITY)
            {
                c.neon.intensity = start.neon.intensity * (1.0f + 0.1f * std::sin(0.1f * f));
            }
            else if (mode == TimeMode::ARC_WIPE && !c.neon.arcs.empty())
            {
                c.neon.arcs[0].length = 0.3f + 0.6f * (0.5f + 0.5f * std::sin(0.05f * f));
            }
            else if (mode == TimeMode::RESIZE)
            {
                c.geometry.width = start.geometry.width * (1.0f + 0.15f * std::sin(0.05f * f));
                c.geometry.height = start.geometry.height * (1.0f + 0.15f * std::cos(0.05f * f));
            }
            else if (mode == TimeMode::SEGMENT_TRAVEL)
            {
                c.neon.segmentBoosts[0].position = std::fmod(start.neon.segmentBoosts[0].position + 0.003f * f, 1.0f);
            }
            return c;
        }
    }

    double TimeRender(const Config &config, int width, int height, double *initMs, TimeMode mode,
                      const LightCounts &lights, double *firstMs)
    {
        const auto i0 = std::chrono::high_resolution_clock::now();
        EdgeLightingEffect effect;
        CreateEffect(effect);
        glFinish();
        if (initMs)
        {
            *initMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - i0).count();
        }
        const Config start = TimeModeStart(config, mode, lights);
        effect.SetConfig(start);
        effect.Update(0.0f);
        OffscreenCapture capture;
        capture.Begin(width, height);
        // One frame as the mode draws it. The frame count runs on through the
        // warm-up and every run, so no two frames of an animated mode repeat.
        int frameIndex = 0;
        auto frame = [&]() {
            if (mode != TimeMode::STILL)
            {
                if (mode != TimeMode::HUE)
                {
                    effect.SetConfig(TimeModeFrame(start, mode, frameIndex, lights));
                }
                effect.Update(1.0f / 60.0f);
            }
            effect.Render(width, height);
            ++frameIndex;
        };
        glFinish();
        const auto f0 = std::chrono::high_resolution_clock::now();
        frame();
        glFinish();
        if (firstMs)
        {
            *firstMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - f0).count();
        }
        for (int i = 0; i < 4; ++i)
        {
            frame();
        }
        glFinish();
        double best = 1e30;
        for (int run = 0; run < 5; ++run)
        {
            glFinish();
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int n = 0; n < 40; ++n)
            {
                frame();
            }
            glFinish();
            const auto t1 = std::chrono::high_resolution_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count() / 40.0);
        }
        capture.End();
        return best;
    }

    GpuTiming TimeGPU(const Config &config, int width, int height, TimeMode mode, const LightCounts &lights,
                      bool perPass)
    {
        EdgeLightingEffect effect;
        CreateEffect(effect);
        const Config start = TimeModeStart(config, mode, lights);
        effect.SetConfig(start);
        effect.Update(0.0f);
        OffscreenCapture capture;
        capture.Begin(width, height);
        int frameIndex = 0;
        auto frame = [&](GLuint query) {
            if (mode != TimeMode::STILL)
            {
                if (mode != TimeMode::HUE)
                {
                    effect.SetConfig(TimeModeFrame(start, mode, frameIndex, lights));
                }
                effect.Update(1.0f / 60.0f);
            }
            if (query != 0)
            {
                glBeginQuery(GL_TIME_ELAPSED, query);
            }
            effect.Render(width, height);
            if (query != 0)
            {
                glEndQuery(GL_TIME_ELAPSED);
            }
            // What a host's swap does: hand the frame to the GPU, without
            // waiting for it.
            glFlush();
            ++frameIndex;
        };
        for (int i = 0; i < 20; ++i)
        {
            frame(0);
        }
        glFinish();

        GpuTiming timing;
        const int frames = 120;
        std::vector<GLuint> queries(frames);
        glGenQueries(frames, queries.data());
        for (int i = 0; i < frames; ++i)
        {
            frame(queries[size_t(i)]);
        }
        glFinish();
        std::vector<double> ms(frames);
        for (int i = 0; i < frames; ++i)
        {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(queries[size_t(i)], GL_QUERY_RESULT, &ns);
            ms[size_t(i)] = double(ns) / 1e6;
        }
        glDeleteQueries(frames, queries.data());
        std::sort(ms.begin(), ms.end());
        timing.frameMs = ms[ms.size() / 2];

        if (perPass)
        {
            const int recorded = 60;
            NeonTools::PassRecorder::SetReadback(false);
            NeonTools::PassRecorder::SetTiming(true);
            NeonTools::PassRecorder::Begin();
            for (int i = 0; i < recorded; ++i)
            {
                frame(0);
            }
            NeonTools::PassRecorder::End();
            NeonTools::PassRecorder::ResolveTimes();
            NeonTools::PassRecorder::SetTiming(false);
            NeonTools::PassRecorder::SetReadback(true);
            for (const NeonTools::DrawRecord &draw : NeonTools::PassRecorder::GetDraws())
            {
                const std::string name = NeonTools::PassKindName(draw.kind);
                auto it = std::find_if(timing.passes.begin(), timing.passes.end(),
                                       [&](const std::pair<std::string, double> &p) { return p.first == name; });
                if (it == timing.passes.end())
                {
                    timing.passes.push_back({name, 0.0});
                    it = timing.passes.end() - 1;
                }
                it->second += draw.gpuMs / double(recorded);
            }
            std::sort(timing.passes.begin(), timing.passes.end());
        }
        capture.End();
        return timing;
    }

    RGBA RenderOnce(EdgeLightingEffect &effect, int width, int height)
    {
        OffscreenCapture capture;
        capture.Begin(width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        effect.Render(width, height);
        CaptureUtil::Image image;
        capture.Read(image);
        capture.End();
        return image.pixels;
    }

    Metrics Measure(const RGB &image, const RGB &reference, RGB *heatmap)
    {
        Metrics m{};
        std::vector<int> lit;
        double squared = 0.0;
        double energyImage = 0.0;
        double energyReference = 0.0;
        long over8 = 0;
        const size_t n = size_t(FRAME_WIDTH) * FRAME_HEIGHT;
        if (heatmap)
        {
            heatmap->assign(n * 3, 0);
        }
        for (size_t i = 0; i < n; ++i)
        {
            int error = 0;
            int brightImage = 0;
            int brightReference = 0;
            for (int k = 0; k < 3; ++k)
            {
                const int d = int(image[i * 3 + k]) - int(reference[i * 3 + k]);
                squared += double(d) * d;
                error = std::max(error, std::abs(d));
                brightImage = std::max(brightImage, int(image[i * 3 + k]));
                brightReference = std::max(brightReference, int(reference[i * 3 + k]));
            }
            m.maxDelta = std::max(m.maxDelta, error);
            if (brightImage > LIT_THRESHOLD || brightReference > LIT_THRESHOLD)
            {
                lit.push_back(error);
                if (error > 8)
                {
                    ++over8;
                }
            }
            energyImage += Luma(image, i) - CLEAR_LUMA;
            energyReference += Luma(reference, i) - CLEAR_LUMA;
            if (heatmap)
            {
                const unsigned char *c = RAMP[std::min(error, 64)];
                (*heatmap)[i * 3] = c[0];
                (*heatmap)[i * 3 + 1] = c[1];
                (*heatmap)[i * 3 + 2] = c[2];
            }
        }
        m.litPx = long(lit.size());
        double sum = 0.0;
        for (int e : lit)
        {
            sum += e;
        }
        m.meanLit = lit.empty() ? 0.0 : sum / double(lit.size());
        std::sort(lit.begin(), lit.end());
        m.p99 = lit.empty() ? 0 : lit[size_t(std::floor(0.99 * double(lit.size() - 1)))];
        m.pctOver8 = lit.empty() ? 0.0 : 100.0 * double(over8) / double(lit.size());
        const double mse = squared / (double(n) * 3.0);
        m.psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 100.0;
        m.energyRatio = energyReference != 0.0 ? energyImage / energyReference : 1.0;
        return m;
    }

    std::vector<int> ProfileColumn(const RGB &image, int x, int y0)
    {
        std::vector<int> rows;
        for (int y = y0; y < y0 + 73; ++y)
        {
            rows.push_back(int(std::lround(Luma(image, size_t(y) * FRAME_WIDTH + x))));
        }
        return rows;
    }

    SweepStep MeasureSweep(const RGB &image, int x, double edgeY)
    {
        // The window's FLOOR is subtracted before the centroid and the width
        // are taken, so rows entering and leaving the window as the rect moves
        // do not drag either. (The first version of the page used no floor;
        // its hairline wobble at 0.25, +/-0.53 px, still reproduces here.)
        std::vector<std::pair<double, double>> column;
        double peak = 0.0;
        double floorLuma = 1e30;
        for (int y = 0; y < FRAME_HEIGHT; ++y)
        {
            const double centre = y + 0.5;
            if (std::fabs(centre - edgeY) > 12.0)
            {
                continue;
            }
            const double l = Luma(image, size_t(y) * FRAME_WIDTH + x);
            column.push_back({centre, l});
            peak = std::max(peak, l);
            floorLuma = std::min(floorLuma, l);
        }
        double weight = 0.0;
        double moment = 0.0;
        for (const auto &row : column)
        {
            weight += row.second - floorLuma;
            moment += (row.second - floorLuma) * row.first;
        }
        const double half = 0.5 * (peak + floorLuma);
        double left = 0.0;
        double right = 0.0;
        bool haveLeft = false;
        for (size_t i = 1; i < column.size(); ++i)
        {
            const double a = column[i - 1].second;
            const double b = column[i].second;
            if (!haveLeft && a < half && b >= half)
            {
                left = column[i - 1].first + (half - a) / (b - a);
                haveLeft = true;
            }
            if (a >= half && b < half)
            {
                right = column[i - 1].first + (a - half) / (a - b);
            }
        }
        double energy = 0.0;
        for (size_t i = 0, n = size_t(FRAME_WIDTH) * FRAME_HEIGHT; i < n; ++i)
        {
            energy += Luma(image, i);
        }
        SweepStep step;
        step.peak = int(std::lround(peak));
        step.centroidErr = weight > 0.0 ? moment / weight - edgeY : 0.0;
        step.fwhm = right - left;
        step.energy = std::llround(energy);
        return step;
    }

    int MaxDifference(const RGB &a, const RGB &b)
    {
        int m = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            m = std::max(m, std::abs(int(a[i]) - int(b[i])));
        }
        return m;
    }

    bool LoadPNG(const std::string &path, RGB &out, int &width, int &height)
    {
        int channels = 0;
        unsigned char *pixels = stbi_load(path.c_str(), &width, &height, &channels, 3);
        if (!pixels)
        {
            return false;
        }
        out.assign(pixels, pixels + size_t(width) * height * 3);
        stbi_image_free(pixels);
        return true;
    }

    bool WritePNG(const std::string &path, const RGB &rgb, int width, int height)
    {
        return stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
    }

    bool LoadPNG4(const std::string &path, RGBA &out, int &width, int &height)
    {
        int channels = 0;
        unsigned char *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!pixels)
        {
            return false;
        }
        out.assign(pixels, pixels + size_t(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }

    bool WritePNG4(const std::string &path, const RGBA &rgba, int width, int height)
    {
        return stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
    }
}
