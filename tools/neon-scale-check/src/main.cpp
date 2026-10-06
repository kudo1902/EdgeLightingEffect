// neon-scale-check: the harness behind docs/neon-resolution-scale-comparison.html.
//
//   neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing]
//       Renders the twelve scenes at six scales and writes <outdir>/<label>.json
//       - metrics, cross-sections, timing and the motion sweep. With --images it
//       also writes every PNG the page shows into <outdir>/images/.
//
//   neon-scale-check check [--images-dir DIR]
//       A regression gate. Exits non-zero when scale 1.0 has drifted from the
//       page's committed images, when a reduced scale exceeds its error bound,
//       or when a moving hairline wanders off its edge. See README.md.
//
//   neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE]
//       Timing only, at any frame size, plus each effect's initialisation
//       time. For before / after comparisons; see README.md. MODE is what
//       changes between the timed frames: still (the default), hue,
//       intensity, arc-wipe or segment-travel.
//
//   neon-scale-check partition [--configs N] [--seed S]
//       A regression gate for the scaled path's composite: across N random
//       configs, the blit and the edge ring must cover every pixel they reach
//       exactly once. Exits non-zero on any overlap or seam gap. See README.md.
//
//   --verbose on any of them passes the library's INFO log through; without it
//   only the library's WARN and ERROR lines are shown, on stderr.

#include "harness.h"
#include "partition.h"
#include "scenes.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <string>
#include <vector>

#ifndef DOCS_DIR
#define DOCS_DIR "docs"
#endif

using namespace NeonScaleCheck;

namespace
{
    /// The library logs every line, INFO included, to std::cout, which would
    /// bury this tool's report under hundreds of resize and lifetime messages.
    /// This passes only its WARN and ERROR lines on, to stderr - a shader that
    /// fails to compile must still be seen. --verbose skips the filter.
    class LogFilter : public std::streambuf
    {
    protected:
        int overflow(int c) override
        {
            if (c == traits_type::eof())
            {
                return traits_type::not_eof(c);
            }
            if (c != '\n')
            {
                mLine.push_back(char(c));
                return c;
            }
            if (mLine.find("][WARN]") != std::string::npos || mLine.find("][ERROR]") != std::string::npos)
            {
                std::fprintf(stderr, "%s\n", mLine.c_str());
            }
            mLine.clear();
            return c;
        }

    private:
        std::string mLine;
    };

    /// The hairline and default scenes are the two the page sweeps.
    const char *const SWEEP_SCENES[] = {"hairline", "default"};
    const int SWEEP_STEPS = 64;
    const double SWEEP_STEP_PX = 0.125;
    /// The page's motion sprites: steps 0-31 of the hairline, a 64 x 40 crop
    /// whose top-left is fixed at (608, 164), so the edge starts 16 rows in.
    const int SPRITE_FRAMES = 32;
    const int SPRITE_X = 608;
    const int SPRITE_Y = 164;
    const int SPRITE_W = 64;
    const int SPRITE_H = 40;

    /// `mkdir -p` without a shell: every component of @p path, in turn.
    /// Existing directories are fine; anything else that fails is reported.
    bool MakeDirs(const std::string &path)
    {
        for (size_t i = 1; i <= path.size(); ++i)
        {
            if (i == path.size() || path[i] == '/')
            {
                const std::string prefix = path.substr(0, i);
                if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST)
                {
                    return false;
                }
            }
        }
        return true;
    }

    const Scene &FindScene(const char *id)
    {
        for (const Scene &s : SCENES)
        {
            if (std::strcmp(s.id, id) == 0)
            {
                return s;
            }
        }
        std::fprintf(stderr, "neon-scale-check: no scene '%s'\n", id);
        std::exit(2);
    }

    Config SceneConfig(const Scene &scene, float scale)
    {
        Config c = BaseConfig();
        scene.apply(c);
        c.neon.resolutionScale = scale;
        return c;
    }

    std::vector<RGB> RenderAllScales(EdgeLightingEffect &effect, const Scene &scene)
    {
        std::vector<RGB> frames;
        for (int s = 0; s < SCALE_COUNT; ++s)
        {
            frames.push_back(Render(effect, SceneConfig(scene, SCALES[s])));
        }
        return frames;
    }

    std::vector<SweepStep> RunSweep(EdgeLightingEffect &effect, const Scene &scene, float scale, RGB *sprite)
    {
        std::vector<SweepStep> steps;
        if (sprite)
        {
            sprite->assign(size_t(SPRITE_W) * SPRITE_H * SPRITE_FRAMES * 3, 0);
        }
        for (int k = 0; k < SWEEP_STEPS; ++k)
        {
            Config c = SceneConfig(scene, scale);
            c.geometry.position.y += float(k * SWEEP_STEP_PX);
            const RGB frame = Render(effect, c);
            const int column = int(c.geometry.position.x + c.geometry.width * 0.5f);
            steps.push_back(MeasureSweep(frame, column, c.geometry.position.y));
            if (sprite && k < SPRITE_FRAMES)
            {
                for (int y = 0; y < SPRITE_H; ++y)
                {
                    std::memcpy(&(*sprite)[(size_t(k) * SPRITE_H + y) * SPRITE_W * 3],
                                &frame[(size_t(SPRITE_Y + y) * FRAME_WIDTH + SPRITE_X) * 3], size_t(SPRITE_W) * 3);
                }
            }
        }
        return steps;
    }

    int Generate(int argc, char **argv)
    {
        if (argc < 3)
        {
            std::fprintf(stderr, "usage: neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing]\n");
            return 2;
        }
        const std::string out = argv[2];
        std::string label = "run";
        bool images = false;
        bool timing = true;
        for (int i = 3; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--images") == 0)
            {
                images = true;
            }
            else if (std::strcmp(argv[i], "--no-timing") == 0)
            {
                timing = false;
            }
            else if (std::strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            {
                label = argv[++i];
            }
        }
        const std::string imageDir = out + "/images";
        if (!MakeDirs(images ? imageDir : out))
        {
            std::fprintf(stderr, "neon-scale-check: cannot create %s\n", (images ? imageDir : out).c_str());
            return 2;
        }

        const GLSession gl;
        EdgeLightingEffect effect;
        CreateEffect(effect);
        FILE *js = std::fopen((out + "/" + label + ".json").c_str(), "w");
        if (!js)
        {
            std::fprintf(stderr, "neon-scale-check: cannot write %s/%s.json\n", out.c_str(), label.c_str());
            return 2;
        }
        std::fprintf(js, "{\"build\": \"%s\", \"gpu\": \"%s\", \"scenarios\": {", label.c_str(),
                     RendererName().c_str());
        bool firstScene = true;
        for (const Scene &scene : SCENES)
        {
            std::fprintf(stderr, "%s\n", scene.id);
            const std::vector<RGB> frames = RenderAllScales(effect, scene);
            std::fprintf(js, "%s\n\"%s\": {\"metrics\": {", firstScene ? "" : ",", scene.id);
            firstScene = false;
            for (int s = 1; s < SCALE_COUNT; ++s)
            {
                RGB heatmap;
                const Metrics m = Measure(frames[s], frames[0], images ? &heatmap : nullptr);
                std::fprintf(js, "%s\"%s\": {\"maxDelta\": %d, \"meanLit\": %.3f, \"psnr\": %.2f, \"p99\": %d, \"pctOver8\": %.3f, \"energyRatio\": %.5f, \"litPx\": %ld}",
                             s > 1 ? ", " : "", SCALE_TAGS[s], m.maxDelta, m.meanLit, m.psnr, m.p99, m.pctOver8, m.energyRatio, m.litPx);
                if (images)
                {
                    WritePNG(imageDir + "/" + scene.id + "_" + SCALE_TAGS[s] + "_diff.png", heatmap, FRAME_WIDTH, FRAME_HEIGHT);
                }
            }
            std::fprintf(js, "}, \"profile\": {");
            for (int s = 0; s < SCALE_COUNT; ++s)
            {
                const std::vector<int> rows = ProfileColumn(frames[s], scene.profileX, scene.profileY0);
                std::fprintf(js, "%s\"%s\": [", s ? ", " : "", SCALE_TAGS[s]);
                for (size_t r = 0; r < rows.size(); ++r)
                {
                    std::fprintf(js, "%s%d", r ? ", " : "", rows[r]);
                }
                std::fprintf(js, "]");
            }
            std::fprintf(js, "}, \"ms\": {");
            for (int s = 0; s < SCALE_COUNT; ++s)
            {
                const double ms = timing ? TimeRender(SceneConfig(scene, SCALES[s])) : -1.0;
                std::fprintf(js, "%s\"%s\": %.4f", s ? ", " : "", SCALE_TAGS[s], ms);
            }
            std::fprintf(js, "}}");
            if (images)
            {
                for (int s = 0; s < SCALE_COUNT; ++s)
                {
                    const std::string stem = imageDir + "/" + scene.id + "_" + SCALE_TAGS[s];
                    WritePNG(stem + ".png", frames[s], FRAME_WIDTH, FRAME_HEIGHT);
                    const int cx = scene.crop[0], cy = scene.crop[1], cw = scene.crop[2], ch = scene.crop[3];
                    RGB crop(size_t(cw) * ch * 3);
                    for (int y = 0; y < ch; ++y)
                    {
                        std::memcpy(&crop[size_t(y) * cw * 3], &frames[s][(size_t(cy + y) * FRAME_WIDTH + cx) * 3], size_t(cw) * 3);
                    }
                    WritePNG(stem + "_crop.png", crop, cw, ch);
                }
            }
        }
        std::fprintf(js, "},\n\"sweep\": {\"step\": %.3f, \"steps\": %d, \"series\": {", SWEEP_STEP_PX, SWEEP_STEPS);
        for (size_t i = 0; i < sizeof(SWEEP_SCENES) / sizeof(SWEEP_SCENES[0]); ++i)
        {
            const Scene &scene = FindScene(SWEEP_SCENES[i]);
            std::fprintf(stderr, "sweep %s\n", scene.id);
            std::fprintf(js, "%s\"%s\": {", i ? ", " : "", scene.id);
            for (int s = 0; s < SCALE_COUNT; ++s)
            {
                const bool sprite = images && std::strcmp(scene.id, "hairline") == 0;
                RGB spriteImage;
                const std::vector<SweepStep> steps = RunSweep(effect, scene, SCALES[s], sprite ? &spriteImage : nullptr);
                if (sprite)
                {
                    WritePNG(imageDir + "/sweep_hairline_" + SCALE_TAGS[s] + ".png", spriteImage, SPRITE_W, SPRITE_H * SPRITE_FRAMES);
                }
                std::fprintf(js, "%s\"%s\": {\"peak\": [", s ? ", " : "", SCALE_TAGS[s]);
                for (int k = 0; k < SWEEP_STEPS; ++k)
                {
                    std::fprintf(js, "%s%d", k ? ", " : "", steps[k].peak);
                }
                std::fprintf(js, "], \"centroidErr\": [");
                for (int k = 0; k < SWEEP_STEPS; ++k)
                {
                    std::fprintf(js, "%s%.4f", k ? ", " : "", steps[k].centroidErr);
                }
                std::fprintf(js, "], \"fwhm\": [");
                for (int k = 0; k < SWEEP_STEPS; ++k)
                {
                    std::fprintf(js, "%s%.4f", k ? ", " : "", steps[k].fwhm);
                }
                std::fprintf(js, "], \"energy\": [");
                for (int k = 0; k < SWEEP_STEPS; ++k)
                {
                    std::fprintf(js, "%s%lld", k ? ", " : "", steps[k].energy);
                }
                std::fprintf(js, "]}");
            }
            std::fprintf(js, "}");
        }
        std::fprintf(js, "}}}\n");
        std::fclose(js);
        std::fprintf(stderr, "wrote %s/%s.json%s\n", out.c_str(), label.c_str(), images ? " and images/" : "");
        return 0;
    }

    /// Timing only, at any frame size: every scene at every scale, plus how
    /// long each freshly initialised effect took to construct. The scenes'
    /// LAYOUT scales with the frame - rect position, size and corner radius by
    /// the frame's ratio to 1280 x 720 - while the neon's own px parameters
    /// (line width, glow radius, cutoffs) stay as they are, as a host's would
    /// on a bigger display. Interleave runs of the builds being compared and
    /// take the median per figure over the rounds they share; see README.md.
    ///
    /// --mode picks what changes between the timed frames (TimeMode). The
    /// default, still, is what every earlier figure measured; the animated
    /// modes are the frames a host draws while something moves, and the only
    /// ones that see the work the neon does on a changed frame alone.
    int Time(int argc, char **argv)
    {
        if (argc < 3)
        {
            std::fprintf(stderr,
                         "usage: neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE]\n");
            return 2;
        }
        const std::string out = argv[2];
        std::string label = "run";
        int width = FRAME_WIDTH;
        int height = FRAME_HEIGHT;
        TimeMode mode = TimeMode::STILL;
        for (int i = 3; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            {
                label = argv[++i];
            }
            else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            {
                if (!ParseTimeMode(argv[++i], mode))
                {
                    std::fprintf(stderr, "neon-scale-check: --mode wants still, hue, intensity, arc-wipe or "
                                         "segment-travel\n");
                    return 2;
                }
            }
            else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            {
                if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0)
                {
                    std::fprintf(stderr, "neon-scale-check: --size wants WxH, e.g. 1920x1080\n");
                    return 2;
                }
            }
        }
        const float kx = float(width) / float(FRAME_WIDTH);
        const float ky = float(height) / float(FRAME_HEIGHT);

        const GLSession gl;
        FILE *js = std::fopen(out.c_str(), "w");
        if (!js)
        {
            std::fprintf(stderr, "neon-scale-check: cannot write %s\n", out.c_str());
            return 2;
        }
        std::fprintf(js,
                     "{\"build\": \"%s\", \"gpu\": \"%s\", \"size\": [%d, %d], \"mode\": \"%s\", "
                     "\"scenarios\": {",
                     label.c_str(), RendererName().c_str(), width, height, TimeModeName(mode));
        std::vector<double> inits;
        bool first = true;
        for (const Scene &scene : SCENES)
        {
            std::fprintf(stderr, "%s\n", scene.id);
            std::fprintf(js, "%s\n\"%s\": {\"ms\": {", first ? "" : ",", scene.id);
            first = false;
            for (int s = 0; s < SCALE_COUNT; ++s)
            {
                Config c = SceneConfig(scene, SCALES[s]);
                c.geometry.position.x *= kx;
                c.geometry.position.y *= ky;
                c.geometry.width *= kx;
                c.geometry.height *= ky;
                c.geometry.cornerRadius *= std::min(kx, ky);
                double initMs = 0.0;
                const double ms = TimeRender(c, width, height, &initMs, mode);
                inits.push_back(initMs);
                std::fprintf(js, "%s\"%s\": %.4f", s ? ", " : "", SCALE_TAGS[s], ms);
            }
            std::fprintf(js, "}}");
        }
        std::fprintf(js, "},\n\"initMs\": [");
        for (size_t i = 0; i < inits.size(); ++i)
        {
            std::fprintf(js, "%s%.3f", i ? ", " : "", inits[i]);
        }
        std::fprintf(js, "]}\n");
        std::fclose(js);
        std::fprintf(stderr, "wrote %s\n", out.c_str());
        return 0;
    }

    /// Largest error a reduced scale may show against its own 1.0 render:
    /// what the edge ring measured on an AMD Radeon Pro 5300M, plus one level
    /// for GPU-to-GPU variance. Everything reads max 2 there except the
    /// 160 x 96 rect, which is 20 x 12 buffer texels at 0.125.
    int MaxErrorBound(const char *id, int scaleIndex)
    {
        if (std::strcmp(id, "small_rect") == 0)
        {
            if (SCALES[scaleIndex] <= 0.125f)
            {
                return 12;
            }
            if (SCALES[scaleIndex] <= 0.25f)
            {
                return 5;
            }
        }
        return 3;
    }
    /// Scale 1.0 against the page's committed image: GPU-to-GPU variance only.
    const int REFERENCE_DRIFT_BOUND = 2;
    /// The moving hairline's centroid error, any scale (measured: 0.05 below
    /// 1.0, 0.02 at 1.0; 0.53 at 0.25 before the edge ring).
    const double SWEEP_BOUND_PX = 0.1;

    int Check(int argc, char **argv)
    {
        std::string imagesDir = std::string(DOCS_DIR) + "/images/neon-resolution-scale";
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--images-dir") == 0 && i + 1 < argc)
            {
                imagesDir = argv[++i];
            }
        }
        const GLSession gl;
        EdgeLightingEffect effect;
        CreateEffect(effect);
        int failures = 0;
        std::printf("%-14s %6s", "scene", "1.0");
        for (int s = 1; s < SCALE_COUNT; ++s)
        {
            std::printf(" %9s", SCALE_TAGS[s]);
        }
        std::printf("   (1.0: drift from the committed image; others: max / p99 against 1.0)\n");
        for (const Scene &scene : SCENES)
        {
            const std::vector<RGB> frames = RenderAllScales(effect, scene);
            RGB committed;
            int width = 0;
            int height = 0;
            const std::string path = imagesDir + "/" + scene.id + "_s1000.png";
            std::printf("%-14s", scene.id);
            if (!LoadPNG(path, committed, width, height) || width != FRAME_WIDTH || height != FRAME_HEIGHT)
            {
                std::printf(" %6s", "n/a");
                std::fprintf(stderr, "  cannot read %s\n", path.c_str());
                ++failures;
            }
            else
            {
                const int drift = MaxDifference(frames[0], committed);
                const bool bad = drift > REFERENCE_DRIFT_BOUND;
                failures += bad;
                std::printf(" %5d%s", drift, bad ? "!" : " ");
            }
            for (int s = 1; s < SCALE_COUNT; ++s)
            {
                const Metrics m = Measure(frames[s], frames[0], nullptr);
                const bool bad = m.maxDelta > MaxErrorBound(scene.id, s);
                failures += bad;
                std::printf(" %4d/%-3d%s", m.maxDelta, m.p99, bad ? "!" : " ");
            }
            std::printf("\n");
        }
        const Scene &hairline = FindScene("hairline");
        std::printf("hairline sweep, worst centroid error (px):");
        for (int s = 0; s < SCALE_COUNT; ++s)
        {
            double worst = 0.0;
            for (const SweepStep &step : RunSweep(effect, hairline, SCALES[s], nullptr))
            {
                worst = std::max(worst, std::fabs(step.centroidErr));
            }
            const bool bad = worst > SWEEP_BOUND_PX;
            failures += bad;
            std::printf(" %s %.3f%s", SCALE_TAGS[s], worst, bad ? "!" : "");
        }
        std::printf("\n");
        if (failures)
        {
            std::printf("FAIL: %d value(s) out of bounds (marked !). See tools/neon-scale-check/README.md.\n", failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}

int main(int argc, char **argv)
{
    static LogFilter logFilter;
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        verbose = verbose || std::strcmp(argv[i], "--verbose") == 0;
    }
    if (!verbose)
    {
        std::cout.rdbuf(&logFilter);
    }

    if (argc >= 2 && std::strcmp(argv[1], "generate") == 0)
    {
        return Generate(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "check") == 0)
    {
        return Check(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "time") == 0)
    {
        return Time(argc, argv);
    }
    if (argc >= 2 && std::strcmp(argv[1], "partition") == 0)
    {
        return Partition(argc, argv);
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing] [--verbose]\n"
                 "  neon-scale-check check [--images-dir DIR] [--verbose]\n"
                 "  neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE] [--verbose]\n"
                 "  neon-scale-check partition [--configs N] [--seed S] [--verbose]\n");
    return 2;
}
