// neon-scale-check: the harness behind docs/neon-resolution-scale-comparison.html.
//
//   neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing] [--mode MODE]
//       Renders the twelve scenes at six scales and writes <outdir>/<label>.json
//       - metrics, cross-sections, timing and the motion sweep. With --images it
//       also writes every PNG the page shows into <outdir>/images/.
//
//   neon-scale-check generate <outdir> --set cover [--scene A,B]
//       Renders the animated `cover` scenes (cover.h) - six frames each, at
//       1.0, 0.5 and 0.25, every frame a config change - and writes each
//       frame as an RGBA PNG into <outdir>, with the GPU in <outdir>/gpu.txt.
//       Compare two such directories with `diff`.
//
//   neon-scale-check diff <dirA> <dirB> [--max N] [--within1 PCT]
//       Per PNG present in both: the largest RGB and alpha difference, p99.9,
//       and the share of lit pixels off by 1, 2, 3-4, 5-8 and more levels.
//       Exits non-zero when any file's max exceeds N (default 2) or fewer than
//       PCT percent (default 99.9) of its lit pixels are within 1 level -
//       docs/neon-glow-cover-resolution-plan.md section 5's criterion.
//
//   neon-scale-check check [--images-dir DIR]
//       A regression gate. Exits non-zero when scale 1.0 has drifted from the
//       page's committed images, when a reduced scale exceeds its error bound,
//       or when a moving hairline wanders off its edge. See README.md.
//
//   neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE]
//                         [--arcs N] [--segments M] [--set cover] [--scene A,B]
//                         [--scales 1,0.5] [--gpu] [--passes]
//       Timing only, at any frame size, plus each effect's initialisation
//       time. For before / after comparisons; see README.md. MODE is what
//       changes between the timed frames: still (the default), hue,
//       intensity, arc-wipe, segment-travel, lights (N arcs and M segments
//       changing length every frame, defaults 8 / 0) or resize. --set cover times the
//       cover scenes at their own frame sizes instead of the page's. --gpu
//       adds a timer query round each Render; --passes also each pass's
//       share.
//
//   neon-scale-check partition [--configs N] [--seed S]
//       A regression gate for the scaled path's composite: across N random
//       configs, the blit and the edge ring must cover every pixel they reach
//       exactly once. Exits non-zero on any overlap or seam gap. See README.md.
//
//   --verbose on any of them passes the library's INFO log through; without it
//   only the library's WARN and ERROR lines are shown, on stderr.

#include "cover.h"
#include "harness.h"
#include "partition.h"
#include "scenes.h"
#include "pass-recorder.h"
// Compiled into libedge-lighting (lib/src/util/stb-image.cpp); declarations only.
#include "stb/stb_image_write.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

    /// @p list split at commas, empty items dropped.
    std::vector<std::string> SplitList(const std::string &list)
    {
        std::vector<std::string> out;
        size_t begin = 0;
        while (begin <= list.size())
        {
            const size_t end = std::min(list.find(',', begin), list.size());
            if (end > begin)
            {
                out.push_back(list.substr(begin, end - begin));
            }
            begin = end + 1;
        }
        return out;
    }

    /// Whether @p id passes the --scene filter @p only (empty: every scene).
    bool Selected(const std::vector<std::string> &only, const char *id)
    {
        return only.empty() || std::find(only.begin(), only.end(), std::string(id)) != only.end();
    }

    /// The page's tag for @p scale: s1000 for 1.0, s500 for 0.5, ...
    std::string ScaleTag(float scale)
    {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "s%d", int(std::lround(scale * 1000.0f)));
        return tag;
    }

    /// `generate <outdir> --set cover`: every frame of every cover scene at
    /// every cover scale, one RGBA PNG each, on a freshly initialised effect
    /// per scene and scale.
    int GenerateCover(const std::string &out, const std::vector<std::string> &only)
    {
        if (!MakeDirs(out))
        {
            std::fprintf(stderr, "neon-scale-check: cannot create %s\n", out.c_str());
            return 2;
        }
        const GLSession gl;
        {
            std::ofstream gpu(out + "/gpu.txt");
            gpu << RendererName() << "\n";
        }
        // Speed over size: the 3840 x 2160 frames dominate the run at the
        // default level and filter search, and the files are compared, not
        // shipped.
        stbi_write_png_compression_level = 1;
        stbi_write_force_png_filter = 1;
        int written = 0;
        for (const CoverScene &scene : COVER_SCENES)
        {
            if (!Selected(only, scene.id))
            {
                continue;
            }
            std::fprintf(stderr, "%s\n", scene.id);
            for (float scale : COVER_SCALES)
            {
                EdgeLightingEffect effect;
                CreateEffect(effect);
                for (int f = 0; f < COVER_FRAMES; ++f)
                {
                    effect.SetConfig(CoverFrameConfig(scene, f, scale));
                    effect.Update(1.0f / 60.0f);
                    const RGBA frame = RenderOnce(effect, scene.width, scene.height);
                    char name[256];
                    std::snprintf(name, sizeof(name), "/%s_%s_f%d.png", scene.id, ScaleTag(scale).c_str(), f);
                    if (!WritePNG4(out + name, frame, scene.width, scene.height))
                    {
                        std::fprintf(stderr, "neon-scale-check: cannot write %s%s\n", out.c_str(), name);
                        return 2;
                    }
                    ++written;
                }
            }
        }
        std::fprintf(stderr, "wrote %d frames to %s\n", written, out.c_str());
        return 0;
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
            std::fprintf(stderr,
                         "usage: neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing] "
                         "[--mode MODE]\n");
            return 2;
        }
        const std::string out = argv[2];
        for (int i = 3; i + 1 < argc; ++i)
        {
            if (std::strcmp(argv[i], "--set") == 0 && std::strcmp(argv[i + 1], "cover") == 0)
            {
                std::vector<std::string> only;
                for (int j = 3; j + 1 < argc; ++j)
                {
                    if (std::strcmp(argv[j], "--scene") == 0)
                    {
                        only = SplitList(argv[j + 1]);
                    }
                }
                return GenerateCover(out, only);
            }
        }
        std::string label = "run";
        bool images = false;
        bool timing = true;
        // What changes between the timed frames, as for `time`. The page is
        // timed with the hue rotating: since a still frame reuses its passes
        // at every scale (I40, I46), a still timing no longer shows what the
        // scale costs on the frames where it matters.
        TimeMode mode = TimeMode::STILL;
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
            else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
            {
                if (!ParseTimeMode(argv[++i], mode))
                {
                    std::fprintf(stderr, "neon-scale-check: --mode wants still, hue, intensity, arc-wipe, "
                                         "segment-travel, lights or resize\n");
                    return 2;
                }
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
        std::fprintf(js, "{\"build\": \"%s\", \"gpu\": \"%s\", \"timingMode\": \"%s\", \"scenarios\": {",
                     label.c_str(), RendererName().c_str(), TimeModeName(mode));
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
                const double ms =
                    timing ? TimeRender(SceneConfig(scene, SCALES[s]), FRAME_WIDTH, FRAME_HEIGHT, nullptr, mode) : -1.0;
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
    ///
    /// --set cover times the cover scenes (cover.h) instead, each at its own
    /// frame size and as its frame 0 configures it. --scene and --scales
    /// narrow either set. --gpu adds each config's GPU frame time (TimeGPU),
    /// --passes its passes' too. Every figure is also printed, one line per
    /// scene and scale.
    int Time(int argc, char **argv)
    {
        if (argc < 3)
        {
            std::fprintf(stderr,
                         "usage: neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE] "
                         "[--arcs N] [--segments M] [--set cover] [--scene A,B] [--scales 1,0.5] [--gpu] "
                         "[--passes]\n");
            return 2;
        }
        const std::string out = argv[2];
        std::string label = "run";
        int width = FRAME_WIDTH;
        int height = FRAME_HEIGHT;
        TimeMode mode = TimeMode::STILL;
        LightCounts lights;
        bool cover = false;
        bool gpu = false;
        bool passes = false;
        std::vector<std::string> only;
        std::vector<float> scales;
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
                    std::fprintf(stderr, "neon-scale-check: --mode wants still, hue, intensity, arc-wipe, "
                                         "segment-travel, lights or resize\n");
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
            else if (std::strcmp(argv[i], "--arcs") == 0 && i + 1 < argc)
            {
                lights.arcs = std::max(0, std::min(std::atoi(argv[++i]), 8));
            }
            else if (std::strcmp(argv[i], "--segments") == 0 && i + 1 < argc)
            {
                lights.segments = std::max(0, std::min(std::atoi(argv[++i]), 8));
            }
            else if (std::strcmp(argv[i], "--set") == 0 && i + 1 < argc)
            {
                cover = std::strcmp(argv[++i], "cover") == 0;
            }
            else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
            {
                only = SplitList(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--scales") == 0 && i + 1 < argc)
            {
                for (const std::string &item : SplitList(argv[++i]))
                {
                    scales.push_back(float(std::atof(item.c_str())));
                }
            }
            else if (std::strcmp(argv[i], "--gpu") == 0)
            {
                gpu = true;
            }
            else if (std::strcmp(argv[i], "--passes") == 0)
            {
                gpu = true;
                passes = true;
            }
        }
        if (scales.empty())
        {
            if (cover)
            {
                scales.assign(std::begin(COVER_SCALES), std::end(COVER_SCALES));
            }
            else
            {
                scales.assign(SCALES, SCALES + SCALE_COUNT);
            }
        }
        const float kx = float(width) / float(FRAME_WIDTH);
        const float ky = float(height) / float(FRAME_HEIGHT);

        // Each scene to time, as a config at scale 1 and the frame it is drawn into.
        typedef struct Timed
        {
            std::string id;
            Config config;
            int width;
            int height;
        } Timed;
        std::vector<Timed> timed;
        if (cover)
        {
            for (const CoverScene &scene : COVER_SCENES)
            {
                if (Selected(only, scene.id))
                {
                    timed.push_back({scene.id, CoverFrameConfig(scene, 0, 1.0f), scene.width, scene.height});
                }
            }
        }
        else
        {
            for (const Scene &scene : SCENES)
            {
                if (!Selected(only, scene.id))
                {
                    continue;
                }
                Config c = SceneConfig(scene, 1.0f);
                c.geometry.position.x *= kx;
                c.geometry.position.y *= ky;
                c.geometry.width *= kx;
                c.geometry.height *= ky;
                c.geometry.cornerRadius *= std::min(kx, ky);
                timed.push_back({scene.id, c, width, height});
            }
        }

        const GLSession gl;
        if (passes)
        {
            NeonTools::PassRecorder::Install();
        }
        FILE *js = std::fopen(out.c_str(), "w");
        if (!js)
        {
            std::fprintf(stderr, "neon-scale-check: cannot write %s\n", out.c_str());
            return 2;
        }
        std::fprintf(js,
                     "{\"build\": \"%s\", \"gpu\": \"%s\", \"size\": [%d, %d], \"set\": \"%s\", \"mode\": \"%s\", "
                     "\"arcs\": %d, \"segments\": %d, \"scenarios\": {",
                     label.c_str(), RendererName().c_str(), width, height, cover ? "cover" : "page",
                     TimeModeName(mode), lights.arcs, lights.segments);
        std::printf("%s, %s, mode %s", RendererName().c_str(), cover ? "cover set" : "page set", TimeModeName(mode));
        if (mode == TimeMode::LIGHTS)
        {
            std::printf(" (%d arcs, %d segments)", lights.arcs, lights.segments);
        }
        std::printf("\n");
        std::vector<double> inits;
        std::vector<double> firsts;
        bool first = true;
        for (const Timed &t : timed)
        {
            std::fprintf(stderr, "%s\n", t.id.c_str());
            std::vector<double> ms;
            std::vector<GpuTiming> gpus;
            for (float scale : scales)
            {
                Config c = t.config;
                c.neon.resolutionScale = scale;
                double initMs = 0.0;
                double firstMs = 0.0;
                ms.push_back(TimeRender(c, t.width, t.height, &initMs, mode, lights, &firstMs));
                inits.push_back(initMs);
                firsts.push_back(firstMs);
                if (gpu)
                {
                    gpus.push_back(TimeGPU(c, t.width, t.height, mode, lights, passes));
                }
                std::printf("%-18s %-6s %8.4f ms", t.id.c_str(), ScaleTag(scale).c_str(), ms.back());
                if (gpu)
                {
                    std::printf("  gpu %8.4f ms", gpus.back().frameMs);
                    for (const auto &p : gpus.back().passes)
                    {
                        std::printf("  %s %.4f", p.first.c_str(), p.second);
                    }
                }
                std::printf("\n");
                std::fflush(stdout);
            }
            std::fprintf(js, "%s\n\"%s\": {\"ms\": {", first ? "" : ",", t.id.c_str());
            first = false;
            for (size_t s = 0; s < scales.size(); ++s)
            {
                std::fprintf(js, "%s\"%s\": %.4f", s ? ", " : "", ScaleTag(scales[s]).c_str(), ms[s]);
            }
            std::fprintf(js, "}");
            if (gpu)
            {
                std::fprintf(js, ", \"gpu\": {");
                for (size_t s = 0; s < scales.size(); ++s)
                {
                    std::fprintf(js, "%s\"%s\": %.4f", s ? ", " : "", ScaleTag(scales[s]).c_str(), gpus[s].frameMs);
                }
                std::fprintf(js, "}");
            }
            if (passes)
            {
                std::fprintf(js, ", \"passes\": {");
                for (size_t s = 0; s < scales.size(); ++s)
                {
                    std::fprintf(js, "%s\"%s\": {", s ? ", " : "", ScaleTag(scales[s]).c_str());
                    for (size_t p = 0; p < gpus[s].passes.size(); ++p)
                    {
                        std::fprintf(js, "%s\"%s\": %.4f", p ? ", " : "", gpus[s].passes[p].first.c_str(),
                                     gpus[s].passes[p].second);
                    }
                    std::fprintf(js, "}");
                }
                std::fprintf(js, "}");
            }
            std::fprintf(js, "}");
        }
        std::fprintf(js, "},\n\"initMs\": [");
        for (size_t i = 0; i < inits.size(); ++i)
        {
            std::fprintf(js, "%s%.3f", i ? ", " : "", inits[i]);
        }
        std::fprintf(js, "],\n\"firstFrameMs\": [");
        for (size_t i = 0; i < firsts.size(); ++i)
        {
            std::fprintf(js, "%s%.3f", i ? ", " : "", firsts[i]);
        }
        std::fprintf(js, "]}\n");
        std::fclose(js);
        if (passes)
        {
            NeonTools::PassRecorder::Release();
        }
        std::fprintf(stderr, "wrote %s\n", out.c_str());
        return 0;
    }

    /// The first line of @p dir/gpu.txt, or "unknown".
    std::string ReadGpu(const std::string &dir)
    {
        std::ifstream in(dir + "/gpu.txt");
        std::string line;
        return std::getline(in, line) ? line : std::string("unknown");
    }

    /// `diff <dirA> <dirB>`: the measure docs/neon-glow-cover-resolution-plan.md
    /// section 5 judges a step that may move pixels by. RGB and alpha are
    /// reported apart (a layer's coverage alpha can move where its colour on
    /// black cannot); the distribution is over LIT pixels, any RGB channel
    /// at 1 or more in either image.
    int Diff(int argc, char **argv)
    {
        if (argc < 4)
        {
            std::fprintf(stderr, "usage: neon-scale-check diff <dirA> <dirB> [--max N] [--within1 PCT]\n");
            return 2;
        }
        const std::string a = argv[2];
        const std::string b = argv[3];
        int maxAllowed = 2;
        double within1Required = 99.9;
        for (int i = 4; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            {
                maxAllowed = std::atoi(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--within1") == 0 && i + 1 < argc)
            {
                within1Required = std::atof(argv[++i]);
            }
        }
        std::vector<std::string> names;
        if (DIR *d = opendir(a.c_str()))
        {
            while (dirent *e = readdir(d))
            {
                const std::string n = e->d_name;
                if (n.size() > 4 && n.substr(n.size() - 4) == ".png")
                {
                    names.push_back(n);
                }
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        const std::string gpuA = ReadGpu(a);
        const std::string gpuB = ReadGpu(b);
        std::printf("A: %s (%s)\nB: %s (%s)\n", a.c_str(), gpuA.c_str(), b.c_str(), gpuB.c_str());
        if (gpuA != gpuB)
        {
            std::printf("WARNING: rendered on different GPUs - a GPU switch alone moves pixels by a level.\n");
        }
        std::printf("%-34s %4s %5s %6s %9s %8s %8s %8s %8s %8s\n", "file", "max", "alpha", "p99.9", "lit px", "1",
                    "2", "3-4", "5-8", ">8");
        int failures = 0;
        int compared = 0;
        int worstMax = 0;
        int worstAlpha = 0;
        double worstWithin1 = 100.0;
        long changed = 0;
        for (const std::string &n : names)
        {
            RGBA pa;
            RGBA pb;
            int wa = 0, ha = 0, wb = 0, hb = 0;
            if (!LoadPNG4(a + "/" + n, pa, wa, ha))
            {
                continue;
            }
            if (!LoadPNG4(b + "/" + n, pb, wb, hb) || wa != wb || ha != hb)
            {
                std::printf("%-34s missing in B or a different size\n", n.c_str());
                ++failures;
                continue;
            }
            ++compared;
            long hist[256] = {0};
            long lit = 0;
            int maxRgb = 0;
            int maxAlpha = 0;
            for (size_t i = 0; i < pa.size(); i += 4)
            {
                maxAlpha = std::max(maxAlpha, std::abs(int(pa[i + 3]) - int(pb[i + 3])));
                const int la = std::max(pa[i], std::max(pa[i + 1], pa[i + 2]));
                const int lb = std::max(pb[i], std::max(pb[i + 1], pb[i + 2]));
                int d = 0;
                for (int k = 0; k < 3; ++k)
                {
                    d = std::max(d, std::abs(int(pa[i + k]) - int(pb[i + k])));
                }
                maxRgb = std::max(maxRgb, d);
                changed += (d > 0);
                if (la == 0 && lb == 0)
                {
                    continue;
                }
                ++lit;
                ++hist[d];
            }
            long acc = 0;
            int p999 = 0;
            for (int d = 255; d >= 0; --d)
            {
                acc += hist[d];
                if (acc > lit / 1000)
                {
                    p999 = d;
                    break;
                }
            }
            auto pct = [&](int lo, int hi) {
                long sum = 0;
                for (int d = lo; d <= hi; ++d)
                {
                    sum += hist[d];
                }
                return lit ? 100.0 * double(sum) / double(lit) : 0.0;
            };
            const double within1 = pct(0, 1);
            const bool bad = maxRgb > maxAllowed || within1 < within1Required;
            failures += bad;
            worstMax = std::max(worstMax, maxRgb);
            worstAlpha = std::max(worstAlpha, maxAlpha);
            worstWithin1 = std::min(worstWithin1, within1);
            std::printf("%-34s %4d %5d %6d %9ld %7.3f%% %7.3f%% %7.3f%% %7.3f%% %7.3f%%%s\n", n.c_str(), maxRgb,
                        maxAlpha, p999, lit, pct(1, 1), pct(2, 2), pct(3, 4), pct(5, 8), pct(9, 255),
                        bad ? " !" : "");
        }
        std::printf("%d file(s): worst max %d (alpha %d), worst share within 1 level %.3f%%, %ld pixel(s) differ\n",
                    compared, worstMax, worstAlpha, worstWithin1, changed);
        if (compared == 0)
        {
            std::printf("FAIL: nothing compared\n");
            return 1;
        }
        if (failures)
        {
            std::printf("FAIL: %d file(s) outside max %d / %.2f%% within 1 (marked !)\n", failures, maxAllowed,
                        within1Required);
            return 1;
        }
        std::printf("PASS (max %d, %.2f%% within 1)\n", maxAllowed, within1Required);
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
    if (argc >= 2 && std::strcmp(argv[1], "diff") == 0)
    {
        return Diff(argc, argv);
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  neon-scale-check generate <outdir> [--label NAME] [--images] [--no-timing] [--mode MODE] [--verbose]\n"
                 "  neon-scale-check generate <outdir> --set cover [--scene A,B] [--verbose]\n"
                 "  neon-scale-check check [--images-dir DIR] [--verbose]\n"
                 "  neon-scale-check time <out.json> [--label NAME] [--size WxH] [--mode MODE] [--arcs N] [--segments M]\n"
                 "                        [--set cover] [--scene A,B] [--scales 1,0.5] [--gpu] [--passes] [--verbose]\n"
                 "  neon-scale-check partition [--configs N] [--seed S] [--verbose]\n"
                 "  neon-scale-check diff <dirA> <dirB> [--max N] [--within1 PCT]\n");
    return 2;
}
