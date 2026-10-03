#include "partition.h"

#include "harness.h"
#include "pass-recorder.h"

#include "gl/gl-header.h"
#include "util/capture-util.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace NeonScaleCheck
{
    namespace
    {
        using NeonTools::DrawRecord;
        using NeonTools::PassKind;
        using NeonTools::PassRecorder;

        /// Frame sizes the configs are drawn at: the common ones, plus an odd
        /// size so no edge sits on a convenient pixel grid.
        const int FRAME_SIZES[][2] = {{640, 360}, {800, 600}, {1280, 720}, {1366, 768}, {1920, 1080}, {333, 517}};
        const int FRAME_SIZE_COUNT = sizeof(FRAME_SIZES) / sizeof(FRAME_SIZES[0]);

        /// How many failing configs are printed in full.
        const int REPORT_LIMIT = 5;

        /// Draws per pixel in the coverage target: each draw adds this to its
        /// channel, so an RGBA8 channel counts up to three draws exactly.
        const float COVERAGE_STEP = 0.25f;

        /// Uniform floats from mt19937's raw 32 bits, written out rather than
        /// through std::uniform_real_distribution, whose output differs between
        /// standard libraries. A seed then names the same configs on every
        /// platform, so a failure on one machine reproduces on another.
        class Random
        {
        public:
            explicit Random(uint32_t seed)
                : mEngine(seed)
            {
            }

            float Uniform(float lo, float hi)
            {
                return lo + (hi - lo) * static_cast<float>(mEngine() >> 8) * (1.0f / 16777216.0f);
            }

            uint32_t Pick(uint32_t count) { return mEngine() % count; }

        private:
            std::mt19937 mEngine;
        };

        /// One random config below scale 1.0, and the frame it is drawn into.
        /// Everything that shapes the ring or the blit area varies: the rect's
        /// size, fractional position (off screen too), corner radius and
        /// winding, the line, the glow's reach, the glow side and both
        /// cutoffs, and the scale itself.
        Config RandomConfig(Random &generator, int &width, int &height)
        {
            const int *size = FRAME_SIZES[generator.Pick(FRAME_SIZE_COUNT)];
            width = size[0];
            height = size[1];
            const float w = static_cast<float>(width);
            const float h = static_cast<float>(height);

            Config c;
            c.debug.showWireframe = false;
            c.neon.enable = true;
            c.neon.hueRotationRate = 0.0f;
            c.neon.colorTransitionDuration = 0.0f;
            c.geometry.width = generator.Uniform(8.0f, w * 1.1f);
            c.geometry.height = generator.Uniform(8.0f, h * 1.1f);
            c.geometry.position = glm::vec2(generator.Uniform(-0.3f * w, 0.9f * w), generator.Uniform(-0.3f * h, 0.9f * h));
            c.geometry.cornerRadius = generator.Uniform(0.0f, 0.6f * std::min(c.geometry.width, c.geometry.height));
            c.geometry.winding = generator.Pick(2) ? Winding::CLOCKWISE : Winding::COUNTER_CLOCKWISE;
            const float falloffs[] = {0.3f, 0.5f, 1.0f, 2.0f, 4.0f};
            c.neon.lineWidth = generator.Uniform(0.0f, 20.0f);
            c.neon.filamentFalloff = falloffs[generator.Pick(5)];
            c.neon.glowRadius = generator.Uniform(0.0f, 40.0f);
            c.neon.bloomStrength = generator.Uniform(0.0f, 1.5f);
            c.neon.intensity = generator.Uniform(0.2f, 2.0f);
            c.neon.glowSide = static_cast<GlowSide>(generator.Pick(3));
            c.neon.glowSideSoftness = generator.Uniform(0.0f, 12.0f);
            c.neon.insideCutoff = Cutoff{generator.Pick(3) == 0, generator.Uniform(0.0f, 80.0f), generator.Uniform(0.0f, 30.0f)};
            c.neon.outsideCutoff = Cutoff{generator.Pick(3) == 0, generator.Uniform(0.0f, 80.0f), generator.Uniform(0.0f, 30.0f)};
            c.neon.resolutionScale = generator.Uniform(0.05f, 0.99f);
            return c;
        }

        typedef struct Coverage
        {
            long covered = 0; ///< Pixels drawn by the blit or the ring.
            long overlap = 0; ///< Pixels drawn more than once.
            long seamGap = 0; ///< Uncovered pixels with the ring on one side and the blit on the other.
        } Coverage;

        /// Replay @p blit and @p ring - their own vertex arrays and recorded
        /// uMVP, through neon.vert, so the same pixels as the real draws - into
        /// one target, additively, the blit counting in red and the ring in
        /// green, and count what lands where.
        ///
        /// A seam gap is only flagged BETWEEN the two: an uncovered pixel with
        /// ring within two pixels on one side and blit on the other. An
        /// uncovered strip with the ring on both sides is legitimate - the
        /// ring's hole, left undrawn where nothing past an inside cutoff can be
        /// lit - and counting it would fail correct frames.
        Coverage MeasureCoverage(const DrawRecord *blit, const DrawRecord *ring, int width, int height)
        {
            OffscreenCapture target;
            target.Begin(width, height);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            if (blit)
            {
                PassRecorder::Redraw(*blit, glm::vec4(COVERAGE_STEP, 0.0f, 0.0f, 0.0f), false);
            }
            if (ring)
            {
                PassRecorder::Redraw(*ring, glm::vec4(0.0f, COVERAGE_STEP, 0.0f, 0.0f), false);
            }
            CaptureUtil::Image image;
            target.Read(image);
            target.End();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            const int step = static_cast<int>(COVERAGE_STEP * 255.0f + 0.5f);
            auto draws = [&](int x, int y, int channel) {
                if (x < 0 || y < 0 || x >= width || y >= height)
                {
                    return 0;
                }
                return (image.pixels[(static_cast<size_t>(y) * width + x) * 4 + channel] + step / 2) / step;
            };
            auto seamAcross = [&](int x, int y, int dx, int dy) {
                for (int before = 1; before <= 2; ++before)
                {
                    for (int after = 1; before + after <= 3; ++after)
                    {
                        const int x0 = x - dx * before;
                        const int y0 = y - dy * before;
                        const int x1 = x + dx * after;
                        const int y1 = y + dy * after;
                        if ((draws(x0, y0, 1) && draws(x1, y1, 0)) || (draws(x0, y0, 0) && draws(x1, y1, 1)))
                        {
                            return true;
                        }
                    }
                }
                return false;
            };

            Coverage coverage;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const int count = draws(x, y, 0) + draws(x, y, 1);
                    if (count >= 1)
                    {
                        ++coverage.covered;
                    }
                    if (count >= 2)
                    {
                        ++coverage.overlap;
                    }
                    if (count == 0 && (seamAcross(x, y, 1, 0) || seamAcross(x, y, 0, 1)))
                    {
                        ++coverage.seamGap;
                    }
                }
            }
            return coverage;
        }

        void PrintConfig(int index, int width, int height, const Config &c, const Coverage &coverage)
        {
            std::printf("  config %d: %d x %d frame, overlap %ld px, seam gap %ld px\n", index, width, height,
                        coverage.overlap, coverage.seamGap);
            std::printf("    rect %.3f x %.3f at (%.3f, %.3f), cornerRadius %.3f, %s\n", c.geometry.width,
                        c.geometry.height, c.geometry.position.x, c.geometry.position.y, c.geometry.cornerRadius,
                        c.geometry.winding == Winding::CLOCKWISE ? "CLOCKWISE" : "COUNTER_CLOCKWISE");
            std::printf("    resolutionScale %.4f, lineWidth %.3f, filamentFalloff %.2f, glowRadius %.3f, "
                        "bloomStrength %.3f, intensity %.3f\n",
                        c.neon.resolutionScale, c.neon.lineWidth, c.neon.filamentFalloff, c.neon.glowRadius,
                        c.neon.bloomStrength, c.neon.intensity);
            std::printf("    glowSide %d, glowSideSoftness %.3f, insideCutoff {%d, %.3f, %.3f}, "
                        "outsideCutoff {%d, %.3f, %.3f}\n",
                        static_cast<int>(c.neon.glowSide), c.neon.glowSideSoftness,
                        static_cast<int>(c.neon.insideCutoff.enable), c.neon.insideCutoff.size,
                        c.neon.insideCutoff.softness, static_cast<int>(c.neon.outsideCutoff.enable),
                        c.neon.outsideCutoff.size, c.neon.outsideCutoff.softness);
        }
    }

    int Partition(int argc, char **argv)
    {
        int configs = 1000;
        uint32_t seed = 1;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--configs") == 0 && i + 1 < argc)
            {
                configs = std::max(1, std::atoi(argv[++i]));
            }
            else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            {
                seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            }
        }

        const GLSession gl;
        PassRecorder::Install();
        // Only the geometry matters here: no readback of what each draw wrote.
        PassRecorder::SetReadback(false);

        long ringDrawn = 0;
        long covered = 0;
        long overlap = 0;
        long seamGap = 0;
        int failing = 0;
        {
            EdgeLightingEffect effect;
            CreateEffect(effect);
            Random generator(seed);
            for (int i = 0; i < configs; ++i)
            {
                int width = 0;
                int height = 0;
                const Config config = RandomConfig(generator, width, height);
                effect.SetConfig(config);
                effect.Update(0.0f);

                OffscreenCapture frame;
                frame.Begin(width, height);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                PassRecorder::Begin();
                effect.Render(width, height);
                PassRecorder::End();
                frame.End();

                const DrawRecord *blit = PassRecorder::Find(PassKind::P2B);
                const DrawRecord *ring = PassRecorder::Find(PassKind::P2C);
                if (ring)
                {
                    ++ringDrawn;
                }
                if (!blit && !ring)
                {
                    continue;
                }
                const Coverage coverage = MeasureCoverage(blit, ring, width, height);
                covered += coverage.covered;
                overlap += coverage.overlap;
                seamGap += coverage.seamGap;
                if (coverage.overlap || coverage.seamGap)
                {
                    if (failing == 0)
                    {
                        std::printf("failing configs (seed %u):\n", seed);
                    }
                    if (failing < REPORT_LIMIT)
                    {
                        PrintConfig(i, width, height, config, coverage);
                    }
                    ++failing;
                }
            }
        }
        PassRecorder::Release();

        std::printf("%d configs, seed %u: ring drawn in %ld, %ld px covered, %ld px overlapping, %ld px of seam gap\n",
                    configs, seed, ringDrawn, covered, overlap, seamGap);
        if (ringDrawn == 0)
        {
            // Nothing was tested. A library without the edge ring, or a renamed
            // shader uniform PassRecorder names the passes by; either way a
            // pass here would be a false one.
            std::printf("FAIL: the ring was never drawn, so nothing was tested. See tools/neon-scale-check/README.md.\n");
            return 1;
        }
        if (failing)
        {
            std::printf("FAIL: %d config(s) do not tile. Rerun with the same --seed and --configs to reproduce.\n",
                        failing);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
