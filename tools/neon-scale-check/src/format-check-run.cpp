#include "format-check-run.h"

#include "draw-target-watch.h"
#include "shared-buffer-probe.h"

#include "core/edge-lighting.h"
#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
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
#include <memory>
#include <sstream>

namespace NeonScaleCheck
{
    namespace FormatRun
    {
        using NeonTools::DrawTargetWatch;

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


        /// How far B's frames may be from A's, per group: measured on an AMD
        /// Radeon Pro 5300M (2026-10-27) and padded for the project's other
        /// GPU - the worst cell's largest channel difference plus TWO levels,
        /// and its share of lit channels more than 1 level off times 1.5 plus
        /// 0.5 points. Two levels because the Apple M2 Pro and the AMD render
        /// the same scene up to 2 levels apart (scenes.h), so a bound one level
        /// over the AMD's own figure could fail on the M2 with nothing changed.
        /// Run on the M2 Pro too (2026-10-27): every group inside its bound,
        /// with room - see the M2 figures below - so the bounds stand for both
        /// GPUs. A regression gate on the 8-bit rows: a change that makes them
        /// worse fails it; it says nothing about whether they are good enough
        /// (step 6 of the plan, on the device).
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
        /// at 1.0 (no buffer); spotlight 2 / 0.003 at 0.5, 0 / 0 at 1.0. On
        /// the M2 Pro: still 2 / 0.242, 0.055, 0.030 (soft_wash); hue 2 /
        /// 0.129, 0.229, 0.010 (bounded_band); intensity 2 / 0.260, 0.187,
        /// 0.184 (sharp_corners); segments 53 / 9.448; flare 1 / 0 at 0.5;
        /// spotlight 2 / 0.004 at 0.5; both 0 / 0 at 1.0.
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

        /// The neon on @p scene, drawn @p variant, at CELL_SCALES[@p s].
        Cell NeonCell(const Scene &scene, Variant variant, size_t s)
        {
            Cell cell;
            cell.config = BaseConfig();
            scene.apply(cell.config);
            cell.config.neon.resolutionScale = CELL_SCALES[s];
            cell.config.neon.hueRotationRate = variant == Variant::HUE ? 0.5f : 0.0f;
            cell.variant = variant;
            // The segments scene stacks boosts of 2.0, which the emission
            // table's RGBA8 row clamps to 1.0 (its own fallback line says so):
            // tens of levels off at every scale and variant, so a bound of its
            // own, or it would set every group's.
            cell.group = std::strcmp(scene.id, "segments") == 0
                             ? std::string("neon segments")
                             : std::string("neon ") + VariantName(variant) + " " + CELL_SCALE_TAGS[s];
            cell.name = std::string(scene.id) + " " + VariantName(variant) + " " + CELL_SCALE_TAGS[s];
            return cell;
        }

        /// The flare alone, at CELL_SCALES[@p s]. Below 1.0 its
        /// fullScaleName is the 1.0 cell's, what it draws when no scaled
        /// buffer allocates.
        Cell FlareCell(size_t s)
        {
            Cell flare;
            flare.config = BaseConfig();
            flare.config.neon.enable = false;
            flare.config.lensFlare.enable = true;
            flare.config.lensFlare.resolutionScale = CELL_SCALES[s];
            flare.variant = Variant::STILL;
            flare.group = std::string("flare ") + CELL_SCALE_TAGS[s];
            flare.name = std::string("flare still ") + CELL_SCALE_TAGS[s];
            if (s != 0)
            {
                flare.fullScaleName = std::string("flare still ") + CELL_SCALE_TAGS[0];
            }
            return flare;
        }

        /// Two lamps alone, at CELL_SCALES[@p s]; fullScaleName as FlareCell's.
        Cell SpotlightCell(size_t s)
        {
            Cell spot;
            spot.config = BaseConfig();
            spot.config.neon.enable = false;
            spot.config.spotlight.enable = true;
            spot.config.spotlight.resolutionScale = CELL_SCALES[s];
            SpotLight left;
            left.position = glm::vec2(200.0f, 80.0f);
            left.angle = 60.0f;
            SpotLight right;
            right.position = glm::vec2(1080.0f, 80.0f);
            right.angle = 120.0f;
            spot.config.spotlight.lamps = {left, right};
            spot.variant = Variant::STILL;
            spot.group = std::string("spotlight ") + CELL_SCALE_TAGS[s];
            spot.name = std::string("spotlight still ") + CELL_SCALE_TAGS[s];
            if (s != 0)
            {
                spot.fullScaleName = std::string("spotlight still ") + CELL_SCALE_TAGS[0];
            }
            return spot;
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
                        cells.push_back(NeonCell(scene, variant, s));
                    }
                }
            }

            // The flare and the spotlight below 1.0, each alone: their scaled
            // buffers are the other two walks. And each at 1.0, which needs no
            // buffer: what either must draw when no scaled buffer allocates
            // (case C).
            cells.push_back(FlareCell(SCALE_500));
            cells.push_back(FlareCell(SCALE_1000));
            cells.push_back(SpotlightCell(SCALE_500));
            cells.push_back(SpotlightCell(SCALE_1000));
            return cells;
        }

        /// The size cases D2 and D3 fail allocations from, in texels: halfway
        /// between the viewport at 0.25 and at 0.5, so a viewport-sized scaled
        /// buffer - the neon's shared buffer, the flare's, the spotlight's -
        /// fails at 0.5 and allocates at 0.25. Every other buffer the retry
        /// cells allocate stays below it at either scale (the emission table,
        /// the gather buffer, the glow coverage table, at most 1024 x 128),
        /// which their frames matching A's at 0.25 confirms.
        int64_t RetryThreshold()
        {
            const auto viewportTexels = [](size_t s)
            {
                return int64_t(int(float(FRAME_WIDTH) * CELL_SCALES[s])) *
                       int64_t(int(float(FRAME_HEIGHT) * CELL_SCALES[s]));
            };
            return (viewportTexels(SCALE_500) + viewportTexels(SCALE_250)) / 2;
        }

        /// Cases D2 and D3's cells: every layer with a viewport-sized scaled
        /// buffer drawn at 0.5 and then at 0.25 - large enough to fail, then
        /// small enough to allocate again - the flare and the spotlight after
        /// their 1.0 frames, which D3 must draw at 0.5, and the neon on `arcs`,
        /// which reads the glow coverage table too. The flare first, so its
        /// own walk takes the hook's failure; the walks after it find that
        /// failure kept and skip the format without trying it.
        std::vector<Cell> MakeRetryCells()
        {
            std::vector<Cell> cells;
            cells.push_back(FlareCell(SCALE_1000));
            cells.push_back(FlareCell(SCALE_500));
            cells.push_back(FlareCell(SCALE_250));
            cells.push_back(SpotlightCell(SCALE_1000));
            cells.push_back(SpotlightCell(SCALE_500));
            cells.push_back(SpotlightCell(SCALE_250));
            if (const Scene *arcs = FindScene("arcs"))
            {
                cells.push_back(NeonCell(*arcs, Variant::STILL, SCALE_500));
                cells.push_back(NeonCell(*arcs, Variant::STILL, SCALE_250));
            }
            return cells;
        }

        const Scene *FindScene(const char *id)
        {
            for (const Scene &scene : SCENES)
            {
                if (std::strcmp(scene.id, id) == 0)
                {
                    return &scene;
                }
            }
            return nullptr;
        }


        /// The report rows a run does not probe: a hook answers them instead.
        std::vector<GLint> Unprobed(const Hooks &hooks)
        {
            std::vector<GLint> out = hooks.refused;
            out.insert(out.end(), hooks.unknown.begin(), hooks.unknown.end());
            return out;
        }


        /// The message part of a library log line ("[...][LEVEL] file: func(line) > message").
        std::string MessageOf(const std::string &line)
        {
            const size_t at = line.find("> ");
            return at == std::string::npos ? line : line.substr(at + 2);
        }

        /// The run's reader of the neon's shared buffer: what SharedBufferProbe
        /// records, plus the texels of the Render after @ref ReadNext - read
        /// there, while the buffer is still that frame's.
        class TexelProbe : public SharedBufferProbe
        {
        public:
            /// Read the texels in the next Render that is handed a ready buffer.
            void ReadNext() { mReadNext = true; }
            /// What that Render read, handed over; empty if it was not ready.
            std::vector<float> TakeTexels()
            {
                mReadNext = false;
                return std::move(mTexels);
            }

        protected:
            void OnReady(const SharedBuffer &buffer) override
            {
                if (mReadNext)
                {
                    mTexels = ReadSharedTexels(buffer);
                    mReadNext = false;
                }
            }

        private:
            bool mReadNext = false;
            std::vector<float> mTexels;
        };

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
                effect.GetRenderTargetFormats()->SetMaxTextureSize(hooks.maxTextureSize);
                if (hooks.outOfMemoryRetry >= 0.0)
                {
                    effect.GetRenderTargetFormats()->SetOutOfMemoryRetry(hooks.outOfMemoryRetry);
                }
                const std::shared_ptr<NeonRenderer> neon = std::make_shared<NeonRenderer>();
                if (bypassFields)
                {
                    neon->SetBypass(static_cast<uint32_t>(NeonBypass::GLOW_FIELD) |
                                    static_cast<uint32_t>(NeonBypass::RING_FIELD));
                }
                const std::shared_ptr<TexelProbe> probe = std::make_shared<TexelProbe>();
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
                    const int frames = cell.frames > 0 ? cell.frames : FramesFor(cell.variant);
                    const float deltaTime = cell.deltaTime >= 0.0f      ? cell.deltaTime
                                            : cell.variant == Variant::STILL ? 0.0f
                                                                             : 1.0f / 60.0f;
                    for (int f = 0; f < frames; ++f)
                    {
                        if (cell.variant == Variant::INTENSITY)
                        {
                            config.neon.intensity = baseIntensity * (1.0f + 0.1f * std::sin(0.9f * float(f)));
                        }
                        effect.SetConfig(config);
                        effect.Update(deltaTime);
                        if (!capture.Begin(FRAME_WIDTH, FRAME_HEIGHT))
                        {
                            std::fprintf(stderr, "neon-scale-check: the capture target failed\n");
                            std::exit(2);
                        }
                        if (cell.readShared && f + 1 == frames)
                        {
                            probe->ReadNext();
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
                            result.sharedTexels.push_back(probe->TakeTexels());
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

                // Every texture alive, while the effect still holds its own: up
                // to the highest name ever generated (DrawTargetWatch), so a
                // driver that does not reuse names cannot hide one past a
                // fixed bound. Every texture here is 2D - the library and the
                // capture make no other - and an error from binding one that
                // was not would count as a frame with a GL error.
                const GLuint highest = DrawTargetWatch::GetHighestTextureName();
                for (GLuint name = 1; name <= highest; ++name)
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
                bool scanError = false;
                while (glGetError() != GL_NO_ERROR)
                {
                    scanError = true;
                }
                result.glErrorFrames += scanError;
                result.probes = effect.GetRenderTargetFormats()->GetAnswers().size();
                result.failedAllocations = effect.GetRenderTargetFormats()->GetFailedAllocations();
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
                if (line.find("[INFO]") != std::string::npos)
                {
                    result.infos.push_back(message);
                }
                else if (line.find("[WARN]") != std::string::npos)
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

        /// Whether @p run's frame of the cell named @p name is, byte for byte,
        bool SameFrame(const RunResult &run, const std::string &name, const RunResult &reference,
                       const std::string &referenceName, const char *referenceLabel, const std::vector<Cell> &cells)
        {
            const auto indexOf = [&cells](const std::string &cellName)
            {
                const auto it = std::find_if(cells.begin(), cells.end(),
                                             [&cellName](const Cell &cell) { return cell.name == cellName; });
                return size_t(it - cells.begin());
            };
            const size_t at = indexOf(name);
            const size_t referenceAt = indexOf(referenceName);
            if (at >= cells.size() || referenceAt >= cells.size())
            {
                std::printf("  no cell %s - a scene renamed?\n",
                            (at >= cells.size() ? name : referenceName).c_str());
                return false;
            }
            const bool same = run.frames[at] == reference.frames[referenceAt] && !AllZero(run.frames[at]);
            if (!same)
            {
                std::printf("  %s differs from %s's %s\n", name.c_str(), referenceLabel, referenceName.c_str());
            }
            return same;
        }

        /// Lines of @p lines holding @p needle and, if given, @p also.
        int CountLines(const std::vector<std::string> &lines, const char *needle, const char *also)
        {
            int count = 0;
            for (const std::string &line : lines)
            {
                count += line.find(needle) != std::string::npos && (!also || line.find(also) != std::string::npos);
            }
            return count;
        }

        int CheckQualityBounds(const RunResult &a, const RunResult &b, const std::vector<Cell> &cells, bool verbose)
        {
            int failures = 0;
            std::printf("B against A, worst cell per group (max levels / %% of lit channels more than 1 off; "
                        "bound in brackets):\n");
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
            return failures;
        }
    }
}
