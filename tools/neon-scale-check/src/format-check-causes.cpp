#include "format-check-run.h"
#include "shared-buffer-probe.h"

#include "core/render-target-formats.h"
#include "gl/gl-header.h"
#include "renderer/lens-flare-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/spotlight-renderer.h"
#include "util/capture-util.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// `formats`' cases D4, D7, D5, D6, D8 and S: what each cause of a failed allocation may
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

            /// D7: D4's failure under a growing rect - the `segments` scene at
            /// 0.5, 12 cells each 90 x 45 px larger than the last. A longer
            /// perimeter gathers on a coarser grid, so the gather buffer
            /// SHRINKS as the rect grows (192 x 160 down to 96 x 64): every
            /// cell lands below the size the last failure had. An incomplete
            /// framebuffer is kept for its format and attachment count at
            /// every size, so it fails once and is never tried again; kept by
            /// size, as it first was, it was tried and failed at every new
            /// size (7 times). Out of memory on the same cells is the
            /// contrast, and the proof the gather did shrink: memory is kept
            /// by size, so it is tried again below each failure - more than
            /// once.
            int CheckGrowingRect(bool verbose)
            {
                std::vector<Cell> cells;
                if (const Scene *segments = FindScene("segments"))
                {
                    for (int i = 0; i < 12; ++i)
                    {
                        Cell cell = NeonCell(*segments, Variant::STILL, SCALE_500);
                        cell.config.geometry.position = glm::vec2(40.0f, 30.0f);
                        cell.config.geometry.width = 210.0f + 90.0f * float(i);
                        cell.config.geometry.height = 165.0f + 45.0f * float(i);
                        cell.name += " w" + std::to_string(int(cell.config.geometry.width));
                        cells.push_back(cell);
                    }
                }
                std::printf("D7 - D4's failure under a growing rect, on %zu `segments` cells%s\n", cells.size(),
                            cells.empty() ? " - no `segments` scene!" : "");
                int failures = cells.empty();

                Hooks incomplete;
                incomplete.failed.formats = {GL_RGBA16F};
                incomplete.failed.fromAttachments = 2;
                incomplete.failed.error = GL_FRAMEBUFFER_UNSUPPORTED;
                Hooks outOfMemory = incomplete;
                outOfMemory.failed.error = GL_OUT_OF_MEMORY;
                const RunResult kept = Run(incomplete, false, cells);
                const RunResult memory = Run(outOfMemory, false, cells);
                failures += Summarise("  incomplete", kept, Unprobed(incomplete), verbose);
                failures += Summarise("  out of memory", memory, Unprobed(outOfMemory), verbose);

                const int keptTries = CountLines(kept.errors, "gather target unavailable");
                const int memoryTries = CountLines(memory.errors, "gather target unavailable");
                std::printf("  gather allocations that failed: incomplete %d (expected 1: kept at every size)%s, out "
                            "of memory %d (expected more than 1: kept by size, tried again below)%s\n",
                            keptTries, keptTries == 1 ? "" : "!", memoryTries, memoryTries > 1 ? "" : "!");
                return failures + (keptTries != 1) + (memoryTries <= 1);
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
                // A failure that holds for the run, as D's: no retry, whatever
                // frame time the moving cells pass.
                failing.hooks.outOfMemoryRetry = std::numeric_limits<double>::infinity();
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

            /// What a passing shortage does to the neon's shared buffer - part
            /// of what a way tests, so checked.
            typedef enum class SharedSeen
            {
                FALLS_AND_RETURNS, ///< Seen in RGBA8 and in RGBA16F: the failure reaches it.
                ONLY_HALF_FLOAT,   ///< Only ever RGBA16F: the way tests a buffer whose move it must not hide.
                NOT_DRAWN,         ///< Never ready: no cell draws the neon.
            } SharedSeen;

            /// One way D8's passing shortage is drawn: which allocation fails,
            /// what that leaves in the 8-bit row, and on which cells.
            typedef struct PassingShortage
            {
                const char *label;
                GLint failed;           ///< Its first allocation fails (ForcedFailure::count 1)...
                int64_t fromTexels;     ///< ...of at least this many texels x attachments; 0 is any...
                int fromAttachments;    ///< ...and at least this many attachments.
                GLint fallback;         ///< The row the buffers it reaches fall to - drawn into here, never in A.
                std::vector<Cell> cells; ///< Drawn before the wait and after it; the first also waits.
                SharedSeen shared;
            } PassingShortage;

            /// The name of the phase @p name ends in, if it is @p phase.
            bool InPhase(const std::string &name, const std::string &phase)
            {
                return name.size() > phase.size() &&
                       name.compare(name.size() - phase.size(), phase.size(), phase) == 0;
            }

            /// One passing shortage: @p way's cells " during", the first again
            /// " wait" for 3 s of frame time, every cell again " after" - drawn
            /// with and without the failure. The " wait" cell's last frame -
            /// one frame after the buffers move back up - and every " after"
            /// cell must be A's byte for byte. Returns the failures.
            int CheckPassingShortage(const PassingShortage &way, bool verbose)
            {
                std::vector<Cell> cells;
                const auto addPhase = [&cells, &way](const char *phase)
                {
                    for (Cell cell : way.cells)
                    {
                        cell.name += phase;
                        cells.push_back(cell);
                    }
                };
                addPhase(" during");
                Cell wait = way.cells.front();
                wait.name += " wait";
                wait.deltaTime = 3.0f;
                cells.push_back(wait);
                addPhase(" after");

                Hooks passing;
                passing.failed.formats = {way.failed};
                passing.failed.fromTexels = way.fromTexels;
                passing.failed.fromAttachments = way.fromAttachments;
                passing.failed.count = 1;
                const RunResult a = Run(Hooks{}, false, cells);
                const RunResult run = Run(passing, false, cells);
                int failures = Summarise(way.label, run, Unprobed(passing), verbose);

                // Summed, not chained: every cell that differs is printed.
                int afterCells = 0;
                int afterSame = 0;
                for (const Cell &cell : cells)
                {
                    if (InPhase(cell.name, " wait") || InPhase(cell.name, " after"))
                    {
                        ++afterCells;
                        afterSame += SameFrame(run, cell.name, a, cell.name, "A", cells);
                    }
                }
                const bool fellBack = run.drawFormats.count(way.fallback) && !a.drawFormats.count(way.fallback);
                bool sharedOk = false;
                const char *sharedText = "";
                switch (way.shared)
                {
                case SharedSeen::FALLS_AND_RETURNS:
                {
                    sharedOk = run.sharedFormats.count(GL_RGBA8) && run.sharedFormats.count(GL_RGBA16F);
                    sharedText = sharedOk ? ", shared buffer RGBA8 then RGBA16F yes"
                                          : ", shared buffer RGBA8 then RGBA16F NO!";
                    break;
                }
                case SharedSeen::ONLY_HALF_FLOAT:
                {
                    sharedOk = SharedOnlyIn(run, GL_RGBA16F);
                    sharedText = sharedOk ? ", shared buffer only RGBA16F yes" : ", shared buffer only RGBA16F NO!";
                    break;
                }
                case SharedSeen::NOT_DRAWN:
                {
                    sharedOk = run.sharedReadyFrames == 0;
                    sharedText = sharedOk ? "" : ", shared buffer drawn by a cell meant not to!";
                    break;
                }
                }
                const int fallbacks = CountLines(run.errors, "falling back to", "SetFailedAllocations");
                const int passed = CountLines(run.infos, "out-of-memory failure kept against");
                std::printf("  failed allocations %d (expected 1)%s, drawn in %s meanwhile %s%s, a frame after it "
                            "and after it as A %d / %d%s, passing logged %d (expected 1)%s, failures kept at the end "
                            "%zu (expected 0)%s\n",
                            fallbacks, fallbacks == 1 ? "" : "!", FormatName(way.fallback), fellBack ? "yes" : "NO!",
                            sharedText, afterSame, afterCells, afterSame == afterCells ? "" : "!", passed,
                            passed == 1 ? "" : "!", run.failedAllocations.size(),
                            run.failedAllocations.empty() ? "" : "!");
                return failures + (fallbacks != 1) + !fellBack + !sharedOk + (afterSame != afterCells) +
                       (passed != 1) + !run.failedAllocations.empty();
            }

            /// A retry that runs out of memory again, on the shared buffer:
            /// every RGBA16F allocation from the viewport's size fails, so the
            /// shared buffer falls to RGBA8, and its retry once the failure
            /// expires fails too - which destroys it, and the walk allocates
            /// RGBA8 again at the same size: a fresh texture that looks like
            /// the old one. Drawn with the library's retry and with the
            /// failure held for the run (SetOutOfMemoryRetry, infinity), which
            /// never retries; the two must leave the same frame and the same
            /// shared buffer, every texel, and every texel outside the last
            /// write's window must be 0 - which no frame shows, and which a
            /// reallocation not cleared whole leaves holding DrawTargetWatch's
            /// fill. That proves something only where the window leaves texels
            /// outside it, so the check requires some: on @p cell
            /// (`small_rect`) it does, where a 640 x 360 rect's glow covers the
            /// whole buffer and every write clears it all - the first version
            /// of this check, on `arcs`, passed with the defect in place.
            /// Returns the failures.
            int CheckFailedRetry(const Cell &cell, bool verbose)
            {
                Cell during = cell;
                during.name += " during";
                Cell wait = cell;
                wait.name += " wait";
                wait.deltaTime = 3.0f;
                Cell after = cell;
                after.name += " after";
                after.readShared = true;
                const std::vector<Cell> cells = {during, wait, after};

                Hooks retrying;
                retrying.failed.formats = {GL_RGBA16F};
                retrying.failed.fromTexels = RetryThreshold();
                Hooks held = retrying;
                held.outOfMemoryRetry = std::numeric_limits<double>::infinity();
                const RunResult heldRun = Run(held, false, cells);
                const RunResult run = Run(retrying, false, cells);
                int failures = Summarise("  a failed retry - the shared buffer", run, Unprobed(retrying), verbose);

                const int tries = CountLines(run.errors, "shared target unavailable", "SetFailedAllocations");
                const int heldTries = CountLines(heldRun.errors, "shared target unavailable", "SetFailedAllocations");
                const bool sameFrame = SameFrame(run, after.name, heldRun, after.name, "the held run's", cells);
                const SharedRead &read = run.sharedReads.back();
                const SharedRead &heldRead = heldRun.sharedReads.back();
                size_t differing = 0;
                for (size_t i = 0; i < read.texels.size() && i < heldRead.texels.size(); ++i)
                {
                    differing += read.texels[i] != heldRead.texels[i];
                }
                const bool sameShared =
                    !read.texels.empty() && read.texels.size() == heldRead.texels.size() && differing == 0;
                size_t outside = 0;
                size_t heldOutside = 0;
                const size_t nonZero = CountNonZeroOutsideWindow(read.texels, read.size, read.window, outside);
                const size_t heldNonZero =
                    CountNonZeroOutsideWindow(heldRead.texels, heldRead.size, heldRead.window, heldOutside);
                const bool only8 = SharedOnlyIn(run, GL_RGBA8);
                std::printf("  shared target tries that failed %d (expected 2: the first and its retry)%s, held "
                            "%d (expected 1)%s, shared buffer only RGBA8 %s, frame after it as held %s, shared "
                            "buffer after it as held %s (%zu channel(s) differ); outside the last write's window "
                            "%zu texel(s)%s, not 0 %zu%s (held %zu%s)\n",
                            tries, tries == 2 ? "" : "!", heldTries, heldTries == 1 ? "" : "!",
                            only8 ? "yes" : "NO!", sameFrame ? "yes" : "NO!", sameShared ? "yes" : "NO!",
                            differing, outside,
                            outside == 0 ? " - the window covers the whole buffer, so this check proves nothing!" : "",
                            nonZero, nonZero == 0 ? "" : "!", heldNonZero, heldNonZero == 0 ? "" : "!");
                return failures + (tries != 2) + (heldTries != 1) + !only8 + !sameFrame + !sameShared +
                       (outside == 0) + (nonZero != 0) + (heldNonZero != 0);
            }

            /// D8: out of memory that passes - what a failure's expiry is for.
            ///   - Passing, six ways (PassingShortage): only the first
            ///     allocation of one format fails (ForcedFailure::count), which
            ///     keeps that format off every buffer at least that large. A
            ///     cell that passes 3 s of frame time lets it expire, and every
            ///     buffer must move back up - those that never resize and
            ///     those a still frame reuses included, each asking
            ///     IsBelowBestRow - so that cell's last frame, the one after
            ///     the move, and the cells drawn again after it are A's, byte
            ///     for byte, the failure is cleared and that is said once.
            ///     RGBA16F: the emission table takes the failure, in
            ///     Initialize, and the shared buffer, the gather, the flare's
            ///     and the spotlight's scaled buffers and - on `segments` - the
            ///     four-channel coverage table fall to RGBA8. RG16F on `arcs`:
            ///     the two-channel coverage table takes it and falls to RG8.
            ///     RG16F on `segments`: the segment coverage table does.
            ///     RGBA16F from the viewport's size (RetryThreshold) on `arcs`:
            ///     only the shared buffer is that large, so it alone falls,
            ///     and nothing else re-runs pass 1b on the still frames after
            ///     the expiry - the emission table's move did, in the first
            ///     way. The same on the flare alone, kept on throughout: the
            ///     neon cells of the first way switch it off, which gives its
            ///     buffer back and hides its cache's check. RGBA16F at two
            ///     attachments on `segments` at scale 1/16: only the gather
            ///     has two, and at that scale its buffer (rounded up to whole
            ///     REGION_ALLOC_STEPs, with a footprint) is larger than the
            ///     shared buffer, so the gather falls ALONE - at 0.5 the
            ///     shared buffer fell with it, and its move re-ran the gather,
            ///     which hid that a reused still frame never asked about the
            ///     gather. The shared buffer must stay RGBA16F throughout, or
            ///     the way no longer tests what it is for.
            ///   - A failed retry (CheckFailedRetry), on `small_rect`: the
            ///     shared buffer's retry runs out again, and the buffer it
            ///     reallocates must be cleared whole.
            ///   - Lasting: every RGBA16F allocation fails, over 40 frames 1 s
            ///     apart. Each expiry costs one retry - the emission table's,
            ///     the smallest and first asked - which fails again and doubles
            ///     the wait: failures at 0, 2, 6, 14 and 30 s, so the run ends
            ///     waiting 32 s, until 62 s.
            int CheckTransientFailure(bool verbose)
            {
                const Scene *arcs = FindScene("arcs");
                const Scene *segments = FindScene("segments");
                std::printf("D8 - out of memory that passes, and out of memory that lasts%s\n",
                            arcs && segments ? "" : " - no `arcs` or `segments` scene!");
                if (!arcs || !segments)
                {
                    return 1;
                }
                const Cell arcsCell = NeonCell(*arcs, Variant::STILL, SCALE_500);
                const Cell segmentsCell = NeonCell(*segments, Variant::STILL, SCALE_500);
                const Cell flareCell = FlareCell(SCALE_500);
                const Cell spotlightCell = SpotlightCell(SCALE_500);
                // At GATHER_MIN_SCALE and below the gather runs at the reduced
                // buffer's own scale, so its buffer outgrows the shared one.
                Cell sixteenthCell = segmentsCell;
                sixteenthCell.config.neon.resolutionScale = 0.0625f;
                sixteenthCell.name = "segments still s0625";
                const int64_t viewportSized = RetryThreshold();
                const PassingShortage ways[] = {
                    {"  passing, RGBA16F", GL_RGBA16F, 0, 1, GL_RGBA8, {arcsCell, segmentsCell, flareCell, spotlightCell},
                     SharedSeen::FALLS_AND_RETURNS},
                    {"  passing, RG16F - the coverage table", GL_RG16F, 0, 1, GL_RG8, {arcsCell},
                     SharedSeen::ONLY_HALF_FLOAT},
                    {"  passing, RG16F - the segment table", GL_RG16F, 0, 1, GL_RG8, {segmentsCell},
                     SharedSeen::ONLY_HALF_FLOAT},
                    {"  passing, RGBA16F from the viewport's size - the shared buffer", GL_RGBA16F, viewportSized, 1,
                     GL_RGBA8, {arcsCell}, SharedSeen::FALLS_AND_RETURNS},
                    {"  passing, RGBA16F from the viewport's size - the flare's buffer", GL_RGBA16F, viewportSized, 1,
                     GL_RGBA8, {flareCell}, SharedSeen::NOT_DRAWN},
                    {"  passing, RGBA16F at two attachments at scale 1/16 - the gather alone", GL_RGBA16F, 0, 2,
                     GL_RGBA8, {sixteenthCell}, SharedSeen::ONLY_HALF_FLOAT},
                };
                int failures = 0;
                for (const PassingShortage &way : ways)
                {
                    failures += CheckPassingShortage(way, verbose);
                }
                if (const Scene *small = FindScene("small_rect"))
                {
                    failures += CheckFailedRetry(NeonCell(*small, Variant::STILL, SCALE_500), verbose);
                }
                else
                {
                    std::printf("  no `small_rect` scene for the failed retry!\n");
                    ++failures;
                }

                Hooks lasting;
                lasting.failed.formats = {GL_RGBA16F};
                Cell held = NeonCell(*arcs, Variant::STILL, SCALE_500);
                held.frames = 40;
                held.deltaTime = 1.0f;
                const RunResult kept = Run(lasting, false, {held});
                failures += Summarise("  lasting", kept, Unprobed(lasting), verbose);
                const auto record = std::find_if(kept.failedAllocations.begin(), kept.failedAllocations.end(),
                                                 [](const RenderTargetFormats::FailedAllocation &failed)
                                                 { return failed.internalFormat == GL_RGBA16F && failed.outOfMemory; });
                const bool found = record != kept.failedAllocations.end();
                const double waitSeconds = found ? record->wait : 0.0;
                const double retryAt = found ? record->retryAt : 0.0;
                const int tries = CountLines(kept.errors, "RGBA16F", "target unavailable");
                const bool backedOff = found && waitSeconds == 32.0 && retryAt == 62.0;
                std::printf("  failed allocations over 40 s %d (expected 5)%s, waiting %.0f s until %.0f s (expected 32 "
                            "until 62)%s\n",
                            tries, tries == 5 ? "" : "!", waitSeconds, retryAt, backedOff ? "" : "!");
                failures += (tries != 5) + !backedOff;
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
            failures += CheckGrowingRect(verbose);
            failures += CheckSizeLimit(verbose);
            failures += CheckSegmentTable(cells, verbose);
            failures += CheckTransientFailure(verbose);
            failures += CheckStandalone(cells, a);
            return failures;
        }
    }
}
