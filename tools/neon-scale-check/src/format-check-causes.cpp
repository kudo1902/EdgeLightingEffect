#include "format-check-run.h"

#include "core/render-target-formats.h"
#include "gl/gl-header.h"
#include "renderer/lens-flare-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/spotlight-renderer.h"
#include "util/capture-util.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// `formats`' cases D4, D5, D6 and S: what each cause of a failed allocation may
// block - and may not - and a renderer drawn outside an effect. See
// format-check.h for every case's checks.

namespace NeonScaleCheck
{
    namespace FormatRun
    {
        namespace
        {
            /// A renderer drawn outside an effect, which makes its own
            /// RenderTargetFormats (BaseRenderer::GetRenderTargetFormats) -
            /// exposed here to count its probes.
            template <typename Renderer>
            class Standalone : public Renderer
            {
            public:
                const RenderTargetFormats &Formats() { return this->GetRenderTargetFormats(); }
            };

            /// What one standalone renderer left: the probes its own object
            /// made by the end of Initialize and by the end of its frames, its
            /// last frame, and the frames that left a GL error.
            typedef struct StandaloneResult
            {
                size_t probesAfterInitialize = 0;
                size_t probesAfterFrames = 0;
                RGBA frame;
                int glErrorFrames = 0;
            } StandaloneResult;

            /// Register-free, as a host without EdgeLightingEffect drives it:
            /// OnConfigChanged with the defaults, Initialize, the cell's
            /// config, then the cell's frames - each RenderOffscreen, then
            /// Render - into a capture, as Run draws them.
            template <typename Renderer>
            StandaloneResult DrawStandalone(const Cell &cell)
            {
                StandaloneResult result;
                Standalone<Renderer> renderer;
                renderer.OnConfigChanged(Config{});
                if (!renderer.Initialize())
                {
                    return result;
                }
                result.probesAfterInitialize = renderer.Formats().GetAnswers().size();
                renderer.OnConfigChanged(cell.config);

                OffscreenCapture capture;
                const int frames = FramesFor(cell.variant);
                for (int f = 0; f < frames; ++f)
                {
                    renderer.Update(0.0f, 0.0, cell.config);
                    if (!capture.Begin(FRAME_WIDTH, FRAME_HEIGHT))
                    {
                        return result;
                    }
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    renderer.RenderOffscreen(FRAME_WIDTH, FRAME_HEIGHT, 0.0, cell.config);
                    renderer.Render(FRAME_WIDTH, FRAME_HEIGHT, 0.0, cell.config);
                    if (f + 1 == frames)
                    {
                        CaptureUtil::Image image;
                        capture.Read(image);
                        result.frame = image.pixels;
                    }
                    capture.End();
                    bool error = false;
                    while (glGetError() != GL_NO_ERROR)
                    {
                        error = true;
                    }
                    result.glErrorFrames += error;
                }
                result.probesAfterFrames = renderer.Formats().GetAnswers().size();
                return result;
            }

            /// The cells on @p sceneId alone, in order.
            std::vector<Cell> CellsOf(const std::vector<Cell> &cells, const char *sceneId)
            {
                std::vector<Cell> out;
                const std::string prefix = std::string(sceneId) + " ";
                for (const Cell &cell : cells)
                {
                    if (cell.name.compare(0, prefix.size(), prefix) == 0)
                    {
                        out.push_back(cell);
                    }
                }
                return out;
            }

            /// D4: RGBA16F refused at two attachments - an incomplete
            /// framebuffer with the gather's segment attachment, as a driver
            /// that will not take two half-float attachments refuses it. The
            /// gather falls back to RGBA8 on the segment scenes only; every
            /// one-attachment RGBA16F buffer stays half float, so every cell
            /// without segments draws A's frame byte for byte and the shared
            /// buffer is only ever RGBA16F. Kept by size alone, as every
            /// failure was before the causes were told apart, the gather's
            /// failure put the shared buffer on RGBA8 from the first segment
            /// scene on.
            int CheckAttachmentFailure(const std::vector<Cell> &cells, const RunResult &a, bool verbose)
            {
                Hooks hooks;
                hooks.failed.formats = {GL_RGBA16F};
                hooks.failed.fromAttachments = 2;
                hooks.failed.error = GL_FRAMEBUFFER_UNSUPPORTED;
                const RunResult run = Run(hooks, false, cells);

                int failures =
                    Summarise("D4 - RGBA16F incomplete with two attachments", run, Unprobed(hooks), verbose);
                int withoutSegments = 0;
                int differing = 0;
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    if (!cells[i].config.neon.segmentBoosts.empty())
                    {
                        continue;
                    }
                    ++withoutSegments;
                    if (run.frames[i] != a.frames[i])
                    {
                        if (differing == 0 || verbose)
                        {
                            std::printf("  differs from A: %s\n", cells[i].name.c_str());
                        }
                        ++differing;
                    }
                }
                const int fallbacks = CountLines(run.errors, "gather target unavailable", "SetFailedAllocations");
                const bool shared = SharedOnlyIn(run, GL_RGBA16F);
                std::printf("  gather fallbacks logged %d%s, cells without segments as A %d / %d%s, shared buffer "
                            "only RGBA16F %s\n",
                            fallbacks, fallbacks == 0 ? "!" : "", withoutSegments - differing, withoutSegments,
                            differing ? "!" : "", shared ? "yes" : "NO!");
                return failures + (fallbacks == 0) + (differing != 0) + !shared;
            }

            /// D5: a texture size limit between the viewport at 0.25 and at
            /// 0.5 (SetMaxTextureSize). Every viewport-sized scaled buffer at
            /// 0.5 is refused before any format is tried - no allocation, no
            /// GL error, nothing kept against a format - so the flare and the
            /// spotlight draw their 0.5 cells at 1.0, the neon at 0.5 has no
            /// shared buffer, and at 0.25 everything allocates in half float
            /// as A does. The fields are bypassed in both runs: the edge
            /// ring's is full resolution whatever the scale, so the limit
            /// would refuse it at 0.25 as well.
            int CheckSizeLimit(bool verbose)
            {
                const std::vector<Cell> cells = MakeRetryCells();
                Hooks limited;
                limited.maxTextureSize = int(float(FRAME_WIDTH) * CELL_SCALES[SCALE_500]) - 1;
                const RunResult a = Run(Hooks{}, true, cells);
                const RunResult run = Run(limited, true, cells);

                std::printf("D5 - textures up to %d px a side, on the retry cells, the fields bypassed\n",
                            limited.maxTextureSize);
                int failures = Summarise("  (the run)", run, Unprobed(limited), verbose);
                const bool large =
                    SameFrame(run, "flare still s500", a, "flare still s1000", "A", cells) +
                        SameFrame(run, "spotlight still s500", a, "spotlight still s1000", "A", cells) ==
                    2;
                const bool small =
                    SameFrame(run, "flare still s250", a, "flare still s250", "A", cells) +
                        SameFrame(run, "spotlight still s250", a, "spotlight still s250", "A", cells) +
                        SameFrame(run, "arcs still s250", a, "arcs still s250", "A", cells) ==
                    3;
                const int refusals = CountLines(run.warnings, "past this GL's texture size limit");
                const int attempts = CountLines(run.errors, "target unavailable");
                const bool shared = SharedOnlyIn(run, GL_RGBA16F);
                std::printf("  at 0.5 as A at 1.0 %s, at 0.25 as A %s, size-limit refusals logged %d (expected 3)%s, "
                            "allocations tried and failed %d%s, shared buffer ready only at 0.25, RGBA16F %s\n",
                            large ? "yes" : "NO!", small ? "yes" : "NO!", refusals, refusals == 3 ? "" : "!",
                            attempts, attempts ? "!" : "", shared ? "yes" : "NO!");
                return failures + !large + !small + (refusals != 3) + (attempts != 0) + !shared;
            }

            /// D6: the segment coverage table unavailable, on the `segments`
            /// scene alone, whose main coverage table is four channels and
            /// unaffected - two ways:
            ///   - RG16F and RG8 refused: the gate knows before the first
            ///     frame, so the table is never tried and its fill program
            ///     never built;
            ///   - RG16F and RG8 failing every allocation: the table is tried
            ///     once, both rows failing - its fill program built just
            ///     before, since nothing knows the allocation fails until it
            ///     is made - and from then on the gate keeps the segments in
            ///     the main table's bake without trying it again at that size.
            /// Either way the frames stay within 2 levels of A's (1 on the M2
            /// Pro) and the shared buffer is RGBA16F. A, which builds the fill
            /// program, is the proof that check can fail.
            int CheckSegmentTable(const std::vector<Cell> &allCells, bool verbose)
            {
                const std::vector<Cell> cells = CellsOf(allCells, "segments");
                const RunResult a = Run(Hooks{}, false, cells);
                const char *const fillLinked = "ShaderProgram[NeonRenderer.GlowCoverFill";
                const int fillInA = CountLines(a.infos, fillLinked, " linked");
                std::printf("D6 - the segment coverage table unavailable, on the %zu `segments` cells (A builds the "
                            "fill program %d time(s)%s)\n",
                            cells.size(), fillInA, fillInA == 0 ? "!" : "");
                int failures = fillInA == 0;

                typedef struct Way
                {
                    const char *label;
                    Hooks hooks;
                    int tries; ///< Failed allocations of the table expected: 0 refused, 2 failing (RG16F, RG8).
                    int fills; ///< Fill programs built expected.
                } Way;
                Way refused{"  refused", Hooks{}, 0, 0};
                refused.hooks.refused = {GL_RG16F, GL_RG8};
                Way failing{"  failing", Hooks{}, 2, 1};
                failing.hooks.failed.formats = {GL_RG16F, GL_RG8};
                for (const Way &way : {refused, failing})
                {
                    const RunResult run = Run(way.hooks, false, cells);
                    failures += Summarise(way.label, run, Unprobed(way.hooks), verbose);
                    int worstLevels = 0;
                    for (size_t i = 0; i < cells.size(); ++i)
                    {
                        int maxLevels = 0;
                        double over1 = 0.0;
                        Compare(a.frames[i], run.frames[i], maxLevels, over1);
                        worstLevels = std::max(worstLevels, maxLevels);
                    }
                    const int tries = CountLines(run.errors, "segment coverage target unavailable");
                    const int fills = CountLines(run.infos, fillLinked, " linked");
                    const int blocked = CountLines(run.warnings, "segment coverage table cannot be allocated");
                    const bool shared = SharedOnlyIn(run, GL_RGBA16F);
                    std::printf("  segment table tries that failed %d (expected %d)%s, fill programs built %d "
                                "(expected %d)%s, held back logged %d (expected 1)%s, worst cell against A %d "
                                "level(s) (at most 2)%s, shared buffer only RGBA16F %s\n",
                                tries, way.tries, tries == way.tries ? "" : "!", fills, way.fills,
                                fills == way.fills ? "" : "!", blocked, blocked == 1 ? "" : "!", worstLevels,
                                worstLevels > 2 ? "!" : "", shared ? "yes" : "NO!");
                    failures +=
                        (tries != way.tries) + (fills != way.fills) + (blocked != 1) + (worstLevels > 2) + !shared;
                }
                return failures;
            }

            /// S: the neon, the flare and the spotlight drawn outside an
            /// effect, each making its own RenderTargetFormats. Each must have
            /// probed the report's five formats by the end of Initialize and
            /// none after - no probe mid-frame - leave no GL error, and draw
            /// what A draws for the same cell, byte for byte.
            int CheckStandalone(const std::vector<Cell> &cells, const RunResult &a)
            {
                std::printf("S - renderers outside an effect, each with its own RenderTargetFormats\n");
                std::ostringstream log;
                std::streambuf *previous = std::cout.rdbuf(log.rdbuf());
                const auto find = [&cells](const std::string &name)
                {
                    return size_t(std::find_if(cells.begin(), cells.end(),
                                               [&name](const Cell &cell) { return cell.name == name; }) -
                                  cells.begin());
                };
                typedef struct Case
                {
                    const char *label;
                    std::string cell;
                    StandaloneResult result;
                } Case;
                std::vector<Case> cases;
                const size_t neonAt = find("default still s500");
                const size_t flareAt = find("flare still s500");
                const size_t spotAt = find("spotlight still s500");
                if (neonAt < cells.size())
                {
                    cases.push_back({"neon", cells[neonAt].name, DrawStandalone<NeonRenderer>(cells[neonAt])});
                }
                if (flareAt < cells.size())
                {
                    cases.push_back(
                        {"flare", cells[flareAt].name, DrawStandalone<LensFlareRenderer>(cells[flareAt])});
                }
                if (spotAt < cells.size())
                {
                    cases.push_back(
                        {"spotlight", cells[spotAt].name, DrawStandalone<SpotlightRenderer>(cells[spotAt])});
                }
                std::cout.rdbuf(previous);

                const size_t expected = ExpectedProbes({});
                int failures = cases.size() == 3 ? 0 : 1;
                if (cases.size() != 3)
                {
                    std::printf("  a cell is missing - a scene renamed?!\n");
                }
                for (const Case &c : cases)
                {
                    const StandaloneResult &r = c.result;
                    const bool probesOk = r.probesAfterInitialize == expected && r.probesAfterFrames == expected;
                    const bool same = r.frame == a.frames[find(c.cell)] && !AllZero(r.frame);
                    std::printf("  %-9s probes %zu after Initialize, %zu after its frames (expected %zu)%s, frames "
                                "with a GL error %d%s, as A's %s %s\n",
                                c.label, r.probesAfterInitialize, r.probesAfterFrames, expected,
                                probesOk ? "" : "!", r.glErrorFrames, r.glErrorFrames ? "!" : "", c.cell.c_str(),
                                same ? "yes" : "NO!");
                    failures += !probesOk + (r.glErrorFrames != 0) + !same;
                }
                return failures;
            }
        }

        int CheckFailureCauses(const std::vector<Cell> &cells, const RunResult &a, bool verbose)
        {
            int failures = CheckAttachmentFailure(cells, a, verbose);
            failures += CheckSizeLimit(verbose);
            failures += CheckSegmentTable(cells, verbose);
            failures += CheckStandalone(cells, a);
            return failures;
        }
    }
}
