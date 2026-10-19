#include "clock-check.h"

#include "harness.h"
#include "scenes.h"

#include "animation/clock.h"
#include "renderer/droplets-tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        /// The start time: 85 * 10240 s, about 10.07 days - past 2^19 s, where
        /// a float clock stepped by 1/60 s froze. A whole number of cycles of
        /// every phase the frame reads, so a run starting here is owed the
        /// frames of a run starting at 0: the hue at HUE_RATE (435200 turns),
        /// the rays at FLARE_RATE (217600) and the droplets' wrapped time at
        /// speed 1 (time * 0.2 / 2048 = 85 periods of DROPLET_TIME_PERIOD).
        const double CLOCK_START_SECONDS = 870400.0;
        const float HUE_RATE = 0.5f;
        const float FLARE_RATE = 0.25f;
        const float FRAME_STEP = 1.0f / 60.0f;
        /// Frames the CPU half steps the clock by: 10 s at 60 fps.
        const int CPU_FRAMES = 600;

        /// The neon with a two-colour ring so its rotation shows, the droplets,
        /// the lens flare with its rays spinning and the debug layer's
        /// gradient strip: every layer whose shader reads a phase of time.
        Config ClockConfig()
        {
            Config c = BaseConfig();
            c.neon.colorStops = {Stop(0.0f, 0.2f, 0.9f, 1.0f), Stop(0.5f, 1.0f, 0.3f, 0.9f)};
            c.neon.hueRotationRate = HUE_RATE;
            c.droplets.enable = true;
            c.droplets.speed = 1.0f;
            c.lensFlare.enable = true;
            c.lensFlare.rotationRate = FLARE_RATE;
            c.debug.showGradientLUT = true;
            return c;
        }

        /// The CPU half: the clock itself, and the phase helper, at the start
        /// time. Returns whether both held.
        bool CheckClock()
        {
            bool ok = true;

            Clock clock;
            clock.SetTime(CLOCK_START_SECONDS);
            double expected = 0.0;
            for (int i = 0; i < CPU_FRAMES; ++i)
            {
                expected += static_cast<double>(clock.Update(FRAME_STEP));
            }
            const double advanced = static_cast<double>(clock.GetTime()) - CLOCK_START_SECONDS;
            const bool clockOk = std::fabs(advanced - expected) < 1e-6;
            std::printf("clock: %d frames of 1/60 s from %.0f s advanced %.9f s (expected %.9f) - %s\n", CPU_FRAMES,
                        CLOCK_START_SECONDS, advanced, expected, clockOk ? "ok" : "FAIL");
            ok = ok && clockOk;

            // For the record: what the float accumulator the clock used to be
            // makes of the same frames.
            float legacy = static_cast<float>(CLOCK_START_SECONDS);
            for (int i = 0; i < CPU_FRAMES; ++i)
            {
                legacy += FRAME_STEP;
            }
            std::printf("clock: a float accumulator advances %.6f s over the same frames\n",
                        static_cast<double>(legacy) - CLOCK_START_SECONDS);

#ifdef DROPLET_TIME_PERIOD
            // The phase a shader is handed, stepped frame by frame from the
            // start time: every step rate / 60 of a cycle, mod 1.
            double worst = 0.0;
            float previous = GetCyclePhase(CLOCK_START_SECONDS, HUE_RATE);
            double time = CLOCK_START_SECONDS;
            for (int i = 0; i < CPU_FRAMES; ++i)
            {
                time += static_cast<double>(FRAME_STEP);
                const float phase = GetCyclePhase(time, HUE_RATE);
                double step = static_cast<double>(phase) - static_cast<double>(previous);
                step -= std::floor(step);
                worst = std::max(worst, std::fabs(step - static_cast<double>(HUE_RATE * FRAME_STEP)));
                previous = phase;
            }
            const bool phaseOk = worst < 1e-6;
            std::printf("phase: hue phase steps off rate / 60 by at most %.3g of a cycle - %s\n", worst,
                        phaseOk ? "ok" : "FAIL");
            ok = ok && phaseOk;
#else
            std::printf("phase: this library has no GetCyclePhase - skipped\n");
#endif
            return ok;
        }

        /// @p frames frames from a fresh effect with every time-reading layer,
        /// its clock started at @p start, each after one Update(1/60).
        std::vector<RGBA> RenderRun(double start, int frames)
        {
            EdgeLightingEffect effect;
            CreateEffect(effect);
            effect.AddRenderer(RendererLayer::DROPLETS);
            effect.AddRenderer(RendererLayer::LENS_FLARE);
            effect.AddRenderer(RendererLayer::DEBUG);
            effect.SetConfig(ClockConfig());
            effect.GetClock().SetTime(start);
            std::vector<RGBA> out;
            for (int i = 0; i < frames; ++i)
            {
                effect.Update(FRAME_STEP);
                out.push_back(RenderOnce(effect, FRAME_WIDTH, FRAME_HEIGHT));
            }
            return out;
        }

        /// Largest channel difference between two RGBA frames, and how many
        /// pixels differ at all.
        int MaxDifferenceRGBA(const RGBA &a, const RGBA &b, long &pixels)
        {
            int worst = 0;
            pixels = 0;
            for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
            {
                int d = 0;
                for (int k = 0; k < 4; ++k)
                {
                    d = std::max(d, std::abs(int(a[i + k]) - int(b[i + k])));
                }
                worst = std::max(worst, d);
                if (d > 0)
                {
                    ++pixels;
                }
            }
            return worst;
        }
    }

    int ClockCheck(int argc, char **argv)
    {
        int frames = 30;
        std::string dump;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            {
                frames = std::max(2, std::atoi(argv[++i]));
            }
            else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            {
                dump = argv[++i];
            }
        }

        bool ok = CheckClock();

        GLSession gl;
        const std::vector<RGBA> early = RenderRun(0.0, frames);
        const std::vector<RGBA> late = RenderRun(CLOCK_START_SECONDS, frames);
        if (!dump.empty())
        {
            // The run from 0, for `diff` against another build's: what the
            // change does to frames at an ordinary uptime.
            for (int i = 0; i < frames; ++i)
            {
                char name[32];
                std::snprintf(name, sizeof(name), "/clock_%03d.png", i);
                WritePNG4(dump + name, early[i], FRAME_WIDTH, FRAME_HEIGHT);
            }
        }
        int worst = 0;
        long worstPixels = 0;
        for (int i = 0; i < frames; ++i)
        {
            long pixels = 0;
            const int d = MaxDifferenceRGBA(early[i], late[i], pixels);
            if (d > worst || (d == worst && pixels > worstPixels))
            {
                worst = d;
                worstPixels = pixels;
            }
        }
        const bool sameOk = worst <= 1;
        std::printf("render: %d frames from %.0f s against the same from 0 s: max %d level(s), %ld px differ - %s\n",
                    frames, CLOCK_START_SECONDS, worst, worstPixels, sameOk ? "ok" : "FAIL");
        ok = ok && sameOk;

        long movedPixels = 0;
        const int moved = MaxDifferenceRGBA(late.front(), late.back(), movedPixels);
        const bool movedOk = moved > 1;
        std::printf("render: frame 1 against frame %d from %.0f s: max %d levels, %ld px differ - %s\n", frames,
                    CLOCK_START_SECONDS, moved, movedPixels, movedOk ? "ok" : "FAIL (frozen)");
        ok = ok && movedOk;

        std::printf("%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
