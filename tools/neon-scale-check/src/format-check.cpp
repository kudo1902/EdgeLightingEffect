#include "format-check.h"
#include "format-check-run.h"

#include "draw-target-watch.h"

#include "core/format-availability.h"
#include "gl/gl-header.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    using namespace FormatRun;
    using NeonTools::DrawTargetWatch;

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
            FormatAvailability report;
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
                    "a flare and a spotlight, each at 0.5 and 1.0\n\n",
                    cells.size());

        const Hooks none;
        Hooks refuseHalf;
        refuseHalf.refused = HALF_FLOAT;
        Hooks refuseAll;
        refuseAll.refused = EVERYTHING;
        Hooks failHalf;
        failHalf.failed.formats = HALF_FLOAT;
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
        failures += CheckQualityBounds(a, b, cells, verbose);

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
        // so no neon cell draws anything. The flare and the spotlight each
        // have a path that needs no buffer: below 1.0 they must fall back to
        // it, log that once each, and draw exactly what A draws at 1.0.
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

        // D4, D5, D6 and S: what each cause of a failed allocation may block,
        // and a renderer outside an effect (format-check-causes.cpp).
        failures += CheckFailureCauses(cells, a, verbose);

        if (failures)
        {
            std::printf("FAIL: %d check(s) failed (marked !). See tools/neon-scale-check/README.md.\n", failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
