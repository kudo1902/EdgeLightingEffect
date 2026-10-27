#include "format-check.h"

#include "draw-target-watch.h"
#include "harness.h"
#include "scenes.h"
#include "shared-buffer-probe.h"

#include "core/render-target-formats.h"
#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "gl/gl-header.h"
#include "renderer/base-renderer.h"
#include "renderer/neon-renderer.h"
#include "util/capture-util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        using NeonTools::DrawTargetWatch;

        /// GLES 3.0 with neither EXT_color_buffer_half_float nor
        /// EXT_color_buffer_float: no 16-bit float target renders.
        const std::vector<GLint> HALF_FLOAT = {GL_RGBA16F, GL_RG16F, GL_R16F};
        /// Every format the library allocates: nothing renders.
        const std::vector<GLint> EVERYTHING = {GL_RGBA16F, GL_RG16F, GL_R16F, GL_RGBA8, GL_RG8};
        /// The startup report's rows (RenderTargetFormats::LogReport): what a
        /// run probes, less what a hook answers instead.
        const GLint REPORT_ROWS[] = {GL_RGBA16F, GL_RG16F, GL_R16F, GL_RGBA8, GL_RG8};

        const float CELL_SCALES[] = {1.0f, 0.5f, 0.25f};
        const char *const CELL_SCALE_TAGS[] = {"s1000", "s500", "s250"};

        /// What moves between a cell's frames.
        typedef enum class Variant
        {
            STILL,     ///< Nothing: the steady frame, as `check` draws it.
            HUE,       ///< The hue rotating, the clock advancing - the glow field's frames.
            INTENSITY, ///< The intensity moving every frame - the field's gain path (I56).
        } Variant;

        /// Frames a cell draws: the second for a still config, as `check`
        /// takes it; enough for a moving one that the fields bake and then
        /// composite (LazyBake waits for a config to hold).
        int FramesFor(Variant variant)
        {
            return variant == Variant::STILL ? 2 : 6;
        }

        const char *VariantName(Variant variant)
        {
            switch (variant)
            {
            case Variant::STILL:
            {
                return "still";
            }
            case Variant::HUE:
            {
                return "hue";
            }
            case Variant::INTENSITY:
            {
                return "intensity";
            }
            }
            return "?";
        }

        /// One scene, drawn one way, at one scale.
        typedef struct Cell
        {
            std::string name;  ///< For a failure line.
            std::string group; ///< What its quality bound is kept per: layer, variant, scale.
            Config config;
            Variant variant;
            /// For a flare or spotlight cell below 1.0: the name of the same
            /// layer's cell at 1.0, which case C must draw - with no buffer to
            /// be had, the layer falls back to 1.0. Empty otherwise.
            std::string fullScaleName;
        } Cell;

        /// How far B's frames may be from A's, per group: measured on an AMD
        /// Radeon Pro 5300M (2026-10-27) and padded for the project's other
        /// GPU - the worst cell's largest channel difference plus TWO levels,
        /// and its share of lit channels more than 1 level off times 1.5 plus
        /// 0.5 points. Two levels because the Apple M2 Pro and the AMD render
        /// the same scene up to 2 levels apart (scenes.h), so a bound one level
        /// over the AMD's own figure could fail on the M2 with nothing changed.
        /// Not yet run on the M2: recalibrate from `formats --verbose` there if
        /// it fails. A regression gate on the 8-bit rows: a change that makes
        /// them worse fails it; it says nothing about whether they are good
        /// enough (step 6 of the plan, on the device).
        typedef struct QualityBound
        {
            const char *group;
            int maxLevels;   ///< Largest channel difference in any frame of the group.
            double over1Pct; ///< Largest share of lit channels more than 1 level off, percent.
        } QualityBound;

        /// Measured worst cells, as max levels / percent: still 2 / 0.021,
        /// 0.003, 0.001 at 1.0, 0.5, 0.25; hue 2 / 0.092, 0.108, 0.006;
        /// intensity 3 / 0.458, 2 / 0.303, 2 / 0.338 (card_outside,
        /// overdrive, sharp_corners - the moving intensity is the field's gain
        /// path); segments 53 / 8.919 (the clamp); flare 1 / 0 at 0.5, 0 / 0
        /// at 1.0 (no buffer); spotlight 2 / 0.003 at 0.5, 0 / 0 at 1.0.
        const QualityBound FORMAT_QUALITY_BOUNDS[] = {
            {"neon still s1000", 4, 0.54},     {"neon still s500", 4, 0.51},      {"neon still s250", 4, 0.51},
            {"neon hue s1000", 4, 0.64},       {"neon hue s500", 4, 0.67},        {"neon hue s250", 4, 0.51},
            {"neon intensity s1000", 5, 1.19}, {"neon intensity s500", 4, 0.96},  {"neon intensity s250", 4, 1.01},
            {"neon segments", 55, 13.9},       {"flare s500", 3, 0.50},           {"spotlight s500", 4, 0.51},
            {"flare s1000", 2, 0.50},          {"spotlight s1000", 2, 0.50},
        };

        const QualityBound *FindBound(const std::string &group)
        {
            for (const QualityBound &bound : FORMAT_QUALITY_BOUNDS)
            {
                if (group == bound.group)
                {
                    return &bound;
                }
            }
            return nullptr;
        }

        std::vector<Cell> MakeCells()
        {
            std::vector<Cell> cells;
            const Variant variants[] = {Variant::STILL, Variant::HUE, Variant::INTENSITY};
            for (const Variant variant : variants)
            {
                for (size_t s = 0; s < std::size(CELL_SCALES); ++s)
                {
                    for (const Scene &scene : SCENES)
                    {
                        Cell cell;
                        cell.config = BaseConfig();
                        scene.apply(cell.config);
                        cell.config.neon.resolutionScale = CELL_SCALES[s];
                        cell.config.neon.hueRotationRate = variant == Variant::HUE ? 0.5f : 0.0f;
                        cell.variant = variant;
                        // The segments scene stacks boosts of 2.0, which the
                        // emission table's RGBA8 row clamps to 1.0 (its own
                        // fallback line says so): tens of levels off at every
                        // scale and variant, so a bound of its own, or it
                        // would set every group's.
                        cell.group = std::strcmp(scene.id, "segments") == 0
                                         ? std::string("neon segments")
                                         : std::string("neon ") + VariantName(variant) + " " + CELL_SCALE_TAGS[s];
                        cell.name = std::string(scene.id) + " " + VariantName(variant) + " " + CELL_SCALE_TAGS[s];
                        cells.push_back(cell);
                    }
                }
            }

            // The flare and the spotlight below 1.0, each alone: their scaled
            // buffers are the other two walks. And each at 1.0, which needs no
            // buffer: what either must draw when no scaled buffer allocates
            // (case C).
            Cell flare;
            flare.config = BaseConfig();
            flare.config.neon.enable = false;
            flare.config.lensFlare.enable = true;
            flare.config.lensFlare.resolutionScale = 0.5f;
            flare.variant = Variant::STILL;
            flare.group = "flare s500";
            flare.name = "flare still s500";
            flare.fullScaleName = "flare still s1000";
            cells.push_back(flare);
            flare.config.lensFlare.resolutionScale = 1.0f;
            flare.group = "flare s1000";
            flare.name = "flare still s1000";
            flare.fullScaleName.clear();
            cells.push_back(flare);

            Cell spot;
            spot.config = BaseConfig();
            spot.config.neon.enable = false;
            spot.config.spotlight.enable = true;
            spot.config.spotlight.resolutionScale = 0.5f;
            SpotLight left;
            left.position = glm::vec2(200.0f, 80.0f);
            left.angle = 60.0f;
            SpotLight right;
            right.position = glm::vec2(1080.0f, 80.0f);
            right.angle = 120.0f;
            spot.config.spotlight.lamps = {left, right};
            spot.variant = Variant::STILL;
            spot.group = "spotlight s500";
            spot.name = "spotlight still s500";
            spot.fullScaleName = "spotlight still s1000";
            cells.push_back(spot);
            spot.config.spotlight.resolutionScale = 1.0f;
            spot.group = "spotlight s1000";
            spot.name = "spotlight still s1000";
            spot.fullScaleName.clear();
            cells.push_back(spot);
            return cells;
        }

        /// What a run's RenderTargetFormats is told, through its three
        /// verification hooks.
        typedef struct Hooks
        {
            std::vector<GLint> refused; ///< SetRefusedFormats.
            std::vector<GLint> failed;  ///< SetFailedAllocations.
            std::vector<GLint> unknown; ///< SetUnknownFormats.
        } Hooks;

        /// The report rows a run does not probe: a hook answers them instead.
        std::vector<GLint> Unprobed(const Hooks &hooks)
        {
            std::vector<GLint> out = hooks.refused;
            out.insert(out.end(), hooks.unknown.begin(), hooks.unknown.end());
            return out;
        }

        /// Everything one run left to judge.
        typedef struct RunResult
        {
            std::vector<RGBA> frames;             ///< Each cell's last frame, over transparent black.
            bool neonPresent = false;             ///< The neon survived Initialize.
            size_t probes = 0;                    ///< RenderTargetFormats::GetAnswers().size() at the end.
            int glErrorFrames = 0;                ///< Frames after which glGetError was not GL_NO_ERROR.
            std::map<GLint, long> drawFormats;    ///< Offscreen draws by the format they wrote.
            std::map<GLint, int> aliveFormats;    ///< Textures alive at the end, by format.
            int sharedReadyFrames = 0;
            std::map<GLint, int> sharedFormats;   ///< The shared buffer's formats while ready.
            std::vector<std::string> fieldLinks;  ///< The field programs' "linked" log lines.
            std::vector<std::string> unknownRows; ///< The startup report's UNKNOWN rows.
            std::vector<std::string> warnings;
            std::vector<std::string> errors;
        } RunResult;

        /// The message part of a library log line ("[...][LEVEL] file: func(line) > message").
        std::string MessageOf(const std::string &line)
        {
            const size_t at = line.find("> ");
            return at == std::string::npos ? line : line.substr(at + 2);
        }

        RunResult Run(const Hooks &hooks, bool bypassFields, const std::vector<Cell> &cells)
        {
            RunResult result;
            std::ostringstream log;
            std::streambuf *previous = std::cout.rdbuf(log.rdbuf());
            DrawTargetWatch::Reset();
            {
                EdgeLightingEffect effect;
                effect.GetRenderTargetFormats()->SetRefusedFormats(hooks.refused);
                effect.GetRenderTargetFormats()->SetFailedAllocations(hooks.failed);
                effect.GetRenderTargetFormats()->SetUnknownFormats(hooks.unknown);
                const std::shared_ptr<NeonRenderer> neon = std::make_shared<NeonRenderer>();
                if (bypassFields)
                {
                    neon->SetBypass(static_cast<uint32_t>(NeonBypass::GLOW_FIELD) |
                                    static_cast<uint32_t>(NeonBypass::RING_FIELD));
                }
                const std::shared_ptr<SharedBufferProbe> probe = std::make_shared<SharedBufferProbe>();
                effect.AddRenderer(neon);
                effect.AddRenderer(RendererLayer::LENS_FLARE);
                effect.AddRenderer(RendererLayer::SPOTLIGHT);
                effect.AddRenderer(probe);
                effect.Initialize();
                const std::vector<RendererLayer> layers = effect.GetLayerOrder();
                result.neonPresent = std::find(layers.begin(), layers.end(), RendererLayer::NEON) != layers.end();

                OffscreenCapture capture;
                for (const Cell &cell : cells)
                {
                    Config config = cell.config;
                    const float baseIntensity = config.neon.intensity;
                    const int frames = FramesFor(cell.variant);
                    for (int f = 0; f < frames; ++f)
                    {
                        if (cell.variant == Variant::INTENSITY)
                        {
                            config.neon.intensity = baseIntensity * (1.0f + 0.1f * std::sin(0.9f * float(f)));
                        }
                        effect.SetConfig(config);
                        effect.Update(cell.variant == Variant::STILL ? 0.0f : 1.0f / 60.0f);
                        if (!capture.Begin(FRAME_WIDTH, FRAME_HEIGHT))
                        {
                            std::fprintf(stderr, "neon-scale-check: the capture target failed\n");
                            std::exit(2);
                        }
                        DrawTargetWatch::Begin();
                        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
                        DrawTargetWatch::End();
                        if (f + 1 == frames)
                        {
                            CaptureUtil::Image image;
                            capture.Read(image);
                            result.frames.push_back(image.pixels);
                        }
                        capture.End();
                        bool error = false;
                        while (glGetError() != GL_NO_ERROR)
                        {
                            error = true;
                        }
                        result.glErrorFrames += error;
                    }
                }

                // Every texture alive, while the effect still holds its own.
                for (GLuint name = 1; name < 16384; ++name)
                {
                    if (!glIsTexture(name))
                    {
                        continue;
                    }
                    glBindTexture(GL_TEXTURE_2D, name);
                    GLint format = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
                    ++result.aliveFormats[format];
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                result.probes = effect.GetRenderTargetFormats()->GetAnswers().size();
                result.sharedReadyFrames = probe->GetReadyFrames();
                result.sharedFormats = probe->GetFormats();
            }
            std::cout.rdbuf(previous);
            result.drawFormats = DrawTargetWatch::GetFormats();

            std::istringstream lines(log.str());
            std::string line;
            while (std::getline(lines, line))
            {
                const std::string message = MessageOf(line);
                if ((message.find("ShaderProgram[NeonRenderer.Field") == 0 ||
                     message.find("ShaderProgram[NeonRenderer.RingField") == 0) &&
                    message.find(" linked") != std::string::npos)
                {
                    result.fieldLinks.push_back(message);
                }
                if (message.find(": UNKNOWN (") != std::string::npos)
                {
                    result.unknownRows.push_back(message);
                }
                if (line.find("[WARN]") != std::string::npos)
                {
                    result.warnings.push_back(message);
                }
                else if (line.find("[ERROR]") != std::string::npos)
                {
                    result.errors.push_back(message);
                }
            }
            return result;
        }

        bool Contains(const std::vector<GLint> &set, GLint format)
        {
            return std::find(set.begin(), set.end(), format) != set.end();
        }

        size_t ExpectedProbes(const std::vector<GLint> &unprobed)
        {
            size_t probes = 0;
            for (const GLint row : REPORT_ROWS)
            {
                probes += Contains(unprobed, row) ? 0 : 1;
            }
            return probes;
        }

        const char *FormatName(GLint format)
        {
            switch (format)
            {
            case GL_RGBA16F:
            {
                return "RGBA16F";
            }
            case GL_RG16F:
            {
                return "RG16F";
            }
            case GL_R16F:
            {
                return "R16F";
            }
            case GL_RGBA8:
            {
                return "RGBA8";
            }
            case GL_RG8:
            {
                return "RG8";
            }
            case GL_R8:
            {
                return "R8";
            }
            case GL_RGBA:
            {
                return "RGBA";
            }
            }
            return "other";
        }

        template <typename Count>
        std::string DescribeFormats(const std::map<GLint, Count> &formats)
        {
            std::string out;
            for (const auto &entry : formats)
            {
                char item[48];
                std::snprintf(item, sizeof(item), "%s%s x%ld", out.empty() ? "" : ", ", FormatName(entry.first),
                              long(entry.second));
                out += item;
            }
            return out.empty() ? "none" : out;
        }

        /// The common lines of a run's summary, and the checks every run
        /// shares: the probe count (the report's rows, less @p unprobed - the
        /// ones a hook answered) and the GL errors. Returns the failures.
        int Summarise(const char *label, const RunResult &run, const std::vector<GLint> &unprobed, bool verbose)
        {
            int failures = 0;
            const size_t expected = ExpectedProbes(unprobed);
            const bool probesBad = run.probes != expected;
            failures += probesBad;
            failures += run.glErrorFrames != 0;
            std::printf("%s\n", label);
            std::printf("  neon %s, probes %zu (expected %zu)%s, frames with a GL error %d%s\n",
                        run.neonPresent ? "registered" : "dropped", run.probes, expected, probesBad ? "!" : "",
                        run.glErrorFrames, run.glErrorFrames != 0 ? "!" : "");
            std::printf("  offscreen draws by target: %s\n", DescribeFormats(run.drawFormats).c_str());
            std::printf("  textures alive at the end: %s\n", DescribeFormats(run.aliveFormats).c_str());
            std::printf("  shared buffer ready on %d frame(s), as %s\n", run.sharedReadyFrames,
                        DescribeFormats(run.sharedFormats).c_str());
            std::printf("  field programs linked: %zu; WARN lines %zu, ERROR lines %zu\n", run.fieldLinks.size(),
                        run.warnings.size(), run.errors.size());
            if (verbose)
            {
                for (const std::string &w : run.warnings)
                {
                    std::printf("    WARN  %s\n", w.c_str());
                }
                for (const std::string &e : run.errors)
                {
                    std::printf("    ERROR %s\n", e.c_str());
                }
            }
            return failures;
        }

        /// Largest channel difference and share of lit channels (either image
        /// above 0) more than 1 level off, percent.
        void Compare(const RGBA &a, const RGBA &b, int &maxLevels, double &over1Pct)
        {
            maxLevels = 0;
            long lit = 0;
            long over1 = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            {
                const int d = std::abs(int(a[i]) - int(b[i]));
                maxLevels = std::max(maxLevels, d);
                if (a[i] != 0 || b[i] != 0)
                {
                    ++lit;
                    over1 += d > 1;
                }
            }
            over1Pct = lit ? 100.0 * double(over1) / double(lit) : 0.0;
        }

        bool AllZero(const RGBA &frame)
        {
            return std::all_of(frame.begin(), frame.end(), [](unsigned char v) { return v == 0; });
        }

        /// Cells whose frames differ by a byte between two runs; prints the
        /// first (every one with @p verbose), naming @p other.
        int CountDiffering(const RunResult &run, const RunResult &other, const char *otherName,
                           const std::vector<Cell> &cells, bool verbose)
        {
            int differing = 0;
            for (size_t i = 0; i < cells.size(); ++i)
            {
                if (run.frames[i] != other.frames[i])
                {
                    if (differing == 0 || verbose)
                    {
                        std::printf("  differs from %s: %s\n", otherName, cells[i].name.c_str());
                    }
                    ++differing;
                }
            }
            return differing;
        }

        /// Draws into, and textures alive in, any format of @p set.
        void CountInFormats(const RunResult &run, const std::vector<GLint> &set, int &drawn, int &alive)
        {
            drawn = 0;
            for (const auto &entry : run.drawFormats)
            {
                drawn += Contains(set, entry.first) ? int(entry.second) : 0;
            }
            alive = 0;
            for (const auto &entry : run.aliveFormats)
            {
                alive += Contains(set, entry.first) ? entry.second : 0;
            }
        }

        bool SharedOnlyIn(const RunResult &run, GLint format)
        {
            return run.sharedReadyFrames > 0 && run.sharedFormats.size() == 1 && run.sharedFormats.count(format);
        }
    }

    int FormatCheck(int argc, char **argv)
    {
        bool verbose = false;
        for (int i = 2; i < argc; ++i)
        {
            verbose = verbose || std::strcmp(argv[i], "--verbose") == 0;
        }

        const GLSession gl;
        DrawTargetWatch::Install();
        std::printf("%s\n", RendererName().c_str());

        // The startup report, on an object of its own: the library logs it
        // at INFO, which this tool hides without --verbose, so it is caught
        // and printed here.
        {
            std::ostringstream log;
            std::streambuf *previous = std::cout.rdbuf(log.rdbuf());
            RenderTargetFormats report;
            report.LogReport();
            std::cout.rdbuf(previous);
            std::istringstream lines(log.str());
            std::string line;
            while (std::getline(lines, line))
            {
                std::printf("  %s\n", MessageOf(line).c_str());
            }
        }

        const std::vector<Cell> cells = MakeCells();
        std::printf("%zu cells a run: every check scene at 1.0, 0.5 and 0.25, still, hue and intensity; "
                    "a flare and a spotlight at 0.5 and 1.0\n\n",
                    cells.size());

        const Hooks none;
        Hooks refuseHalf;
        refuseHalf.refused = HALF_FLOAT;
        Hooks refuseAll;
        refuseAll.refused = EVERYTHING;
        Hooks failHalf;
        failHalf.failed = HALF_FLOAT;
        Hooks unknownHalf;
        unknownHalf.unknown = HALF_FLOAT;

        const RunResult a = Run(none, false, cells);
        const RunResult b = Run(refuseHalf, false, cells);
        const RunResult bBypassed = Run(refuseHalf, true, cells);
        const RunResult c = Run(refuseAll, false, cells);
        const RunResult d = Run(failHalf, false, cells);
        const RunResult e = Run(unknownHalf, false, cells);

        int failures = 0;

        // A: the reference, and the proof the checks below can fail - it has
        // to reach every half-float target the B run must not.
        failures += Summarise("A - nothing refused", a, Unprobed(none), verbose);
        {
            const bool sawHalf = a.drawFormats.count(GL_RGBA16F) && a.drawFormats.count(GL_RG16F);
            const bool allFields = a.fieldLinks.size() == 3;
            const bool shared = SharedOnlyIn(a, GL_RGBA16F);
            std::printf("  reaches half-float targets %s, all three field programs %s, shared buffer RGBA16F %s\n",
                        sawHalf ? "yes" : "NO!", allFields ? "yes" : "NO!", shared ? "yes" : "NO!");
            failures += !sawHalf + !allFields + !shared;
        }

        // B: no refused format anywhere, no field, frames as the bypassed run.
        failures += Summarise("B - half float refused", b, Unprobed(refuseHalf), verbose);
        {
            int drawnRefused = 0;
            int aliveRefused = 0;
            CountInFormats(b, HALF_FLOAT, drawnRefused, aliveRefused);
            const bool shared = SharedOnlyIn(b, GL_RGBA8);
            const bool noFields = b.fieldLinks.empty();
            const int differing = CountDiffering(b, bBypassed, "the fields-bypassed run", cells, verbose);
            std::printf("  draws into a refused format %d%s, refused textures alive %d%s, shared buffer only "
                        "RGBA8 %s, field programs linked %zu%s, cells differing from the fields-bypassed run "
                        "%d%s\n",
                        drawnRefused, drawnRefused ? "!" : "", aliveRefused, aliveRefused ? "!" : "",
                        shared ? "yes" : "NO!", b.fieldLinks.size(), noFields ? "" : "!", differing,
                        differing ? "!" : "");
            failures += (drawnRefused != 0) + (aliveRefused != 0) + !shared + !noFields + (differing != 0);
        }
        failures += Summarise("B with both fields bypassed", bBypassed, Unprobed(refuseHalf), verbose);

        // B against A, per group: the 8-bit rows' cost.
        std::printf("B against A, worst cell per group (max levels / %% of lit channels more than 1 off; "
                    "bound in brackets):\n");
        {
            std::map<std::string, std::pair<int, double>> worst;
            std::map<std::string, std::string> worstCell;
            for (size_t i = 0; i < cells.size(); ++i)
            {
                int maxLevels = 0;
                double over1 = 0.0;
                Compare(a.frames[i], b.frames[i], maxLevels, over1);
                if (verbose)
                {
                    std::printf("    %-28s %3d  %6.3f%%\n", cells[i].name.c_str(), maxLevels, over1);
                }
                std::pair<int, double> &w = worst[cells[i].group];
                if (maxLevels > w.first || over1 > w.second || worstCell[cells[i].group].empty())
                {
                    worstCell[cells[i].group] = cells[i].name;
                }
                w.first = std::max(w.first, maxLevels);
                w.second = std::max(w.second, over1);
            }
            for (const QualityBound &bound : FORMAT_QUALITY_BOUNDS)
            {
                const auto it = worst.find(bound.group);
                if (it == worst.end())
                {
                    continue;
                }
                const bool bad = it->second.first > bound.maxLevels || it->second.second > bound.over1Pct;
                failures += bad;
                std::printf("  %-22s %3d [%3d]  %6.3f%% [%6.3f%%]%s  (%s)\n", bound.group, it->second.first,
                            bound.maxLevels, it->second.second, bound.over1Pct, bad ? " !" : "",
                            worstCell[bound.group].c_str());
            }
            for (const auto &entry : worst)
            {
                if (!FindBound(entry.first))
                {
                    std::printf("  %s: no bound!\n", entry.first.c_str());
                    ++failures;
                }
            }
        }

        // D: the probe passes half float and every full-size allocation of it
        // fails (SetFailedAllocations) - the fallback the walks keep for what
        // a probe cannot predict. It has to land every buffer where B's probe
        // landed it: the same 8-bit rows, the same frames, byte for byte.
        failures += Summarise("D - half float passes the probe, its allocations fail", d, Unprobed(failHalf), verbose);
        {
            int drawnFailed = 0;
            int aliveFailed = 0;
            CountInFormats(d, HALF_FLOAT, drawnFailed, aliveFailed);
            int fallbacks = 0;
            for (const std::string &err : d.errors)
            {
                fallbacks += err.find("falling back to") != std::string::npos &&
                             err.find("SetFailedAllocations") != std::string::npos;
            }
            const bool shared = SharedOnlyIn(d, GL_RGBA8);
            const int differing = CountDiffering(d, b, "B", cells, verbose);
            std::printf("  fallbacks logged %d%s, draws into a failed format %d%s, failed-format textures alive "
                        "%d%s, shared buffer only RGBA8 %s, cells differing from B %d%s\n",
                        fallbacks, fallbacks == 0 ? "!" : "", drawnFailed, drawnFailed ? "!" : "", aliveFailed,
                        aliveFailed ? "!" : "", shared ? "yes" : "NO!", differing, differing ? "!" : "");
            failures += (fallbacks == 0) + (drawnFailed != 0) + (aliveFailed != 0) + !shared + (differing != 0);
        }

        // E: the probe gives no answer for half float (SetUnknownFormats) - an
        // upload that failed for a passing reason. Unknown is not "no": every
        // walk and both field gates must let the allocation decide, which
        // here succeeds, so the run must be A's - the report says UNKNOWN for
        // the three rows (and probes only the other two), nothing is skipped
        // or falls back, every half-float target and all three field programs
        // are reached, and every frame matches A's byte for byte.
        failures += Summarise("E - half float unknown to the probe", e, Unprobed(unknownHalf), verbose);
        {
            const bool sawHalf = e.drawFormats.count(GL_RGBA16F) && e.drawFormats.count(GL_RG16F);
            const bool unknownReported = e.unknownRows.size() == HALF_FLOAT.size();
            int skipsOrFallbacks = 0;
            for (const std::string &w : e.warnings)
            {
                skipsOrFallbacks += w.find("not renderable on this driver") != std::string::npos;
            }
            for (const std::string &err : e.errors)
            {
                skipsOrFallbacks += err.find("falling back to") != std::string::npos;
            }
            const bool allFields = e.fieldLinks.size() == 3;
            const bool shared = SharedOnlyIn(e, GL_RGBA16F);
            const int differing = CountDiffering(e, a, "A", cells, verbose);
            std::printf("  report rows UNKNOWN %zu%s, skips or fallbacks logged %d%s, reaches half-float targets "
                        "%s, all three field programs %s, shared buffer RGBA16F %s, cells differing from A %d%s\n",
                        e.unknownRows.size(), unknownReported ? "" : "!", skipsOrFallbacks,
                        skipsOrFallbacks ? "!" : "", sawHalf ? "yes" : "NO!", allFields ? "yes" : "NO!",
                        shared ? "yes" : "NO!", differing, differing ? "!" : "");
            failures += !unknownReported + (skipsOrFallbacks != 0) + !sawHalf + !allFields + !shared +
                        (differing != 0);
        }

        // C: nothing renders - the neon cannot even hold its emission table,
        // so no neon cell draws anything. The flare and the spotlight have a
        // path that needs no buffer: below 1.0 each must fall back to it, log
        // that once, and draw exactly what A draws at 1.0.
        failures += Summarise("C - everything refused", c, Unprobed(refuseAll), verbose);
        {
            std::map<std::string, size_t> byName;
            for (size_t i = 0; i < cells.size(); ++i)
            {
                byName[cells[i].name] = i;
            }
            int neonDrawn = 0;
            int fullScaleWanted = 0;
            int fullScaleMatched = 0;
            for (size_t i = 0; i < cells.size(); ++i)
            {
                if (cells[i].group.compare(0, 5, "neon ") == 0)
                {
                    neonDrawn += !AllZero(c.frames[i]);
                    continue;
                }
                // Every flare and spotlight cell, 1.0 itself included, must be
                // A's 1.0 frame of its layer.
                const std::string &reference = cells[i].fullScaleName.empty() ? cells[i].name : cells[i].fullScaleName;
                const auto it = byName.find(reference);
                ++fullScaleWanted;
                if (it != byName.end() && c.frames[i] == a.frames[it->second] && !AllZero(c.frames[i]))
                {
                    ++fullScaleMatched;
                }
                else if (verbose || fullScaleMatched + 1 == fullScaleWanted)
                {
                    std::printf("  differs from A at 1.0: %s\n", cells[i].name.c_str());
                }
            }
            int fallbackLogs = 0;
            for (const std::string &w : c.warnings)
            {
                fallbackLogs += w.find("no reduced-scale target could be allocated") != std::string::npos;
            }
            const bool fallbacksLogged = fallbackLogs == 2;
            std::printf("  neon dropped %s, neon frames that drew anything %d%s, flare and spotlight frames as A's at "
                        "1.0 %d / %d%s, falls back to 1.0 logged %d (expected 2)%s, shared buffer never ready %s\n",
                        c.neonPresent ? "NO!" : "yes", neonDrawn, neonDrawn ? "!" : "", fullScaleMatched,
                        fullScaleWanted, fullScaleMatched == fullScaleWanted ? "" : "!", fallbackLogs,
                        fallbacksLogged ? "" : "!", c.sharedReadyFrames == 0 ? "yes" : "NO!");
            failures += c.neonPresent + (neonDrawn != 0) + (fullScaleMatched != fullScaleWanted) + !fallbacksLogged +
                        (c.sharedReadyFrames != 0);
        }

        if (failures)
        {
            std::printf("FAIL: %d check(s) failed (marked !). See tools/neon-scale-check/README.md.\n", failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
