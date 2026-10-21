#include "flare-reuse.h"

#include "harness.h"
#include "scenes.h"

#include "gl/gl-header.h"
#include "renderer/base-renderer.h"
#include "renderer/lens-flare-renderer.h"
#include "util/capture-util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        const float DT = 1.0f / 60.0f;

        /// One frame of the script: the config, the frame it is drawn into,
        /// the clock step and what changed on it.
        typedef struct FlareStep
        {
            Config config;
            glm::ivec2 viewport{1280, 720};
            float dt = DT;
            std::string events;
        } FlareStep;

        /// The neon at its defaults and the flare at scale 0.5 with its sun
        /// part way along the top edge - the layer stack the demo draws.
        Config FlareBaseConfig()
        {
            Config c = BaseConfig();
            c.lensFlare.enable = true;
            c.lensFlare.resolutionScale = 0.5f;
            c.lensFlare.perimeterPosition = 0.15f;
            return c;
        }

        /// The scripted sequence. Each stretch of motion is followed by still
        /// frames, where the reuse takes over: the first still frame after a
        /// change must redraw and every later one may reuse, and both have to
        /// match the bypassed run.
        std::vector<FlareStep> BuildScript()
        {
            std::vector<FlareStep> steps;
            FlareStep s;
            s.config = FlareBaseConfig();
            auto push = [&](const char *events, int count)
            {
                for (int i = 0; i < count; ++i)
                {
                    s.events = i == 0 ? events : "hold";
                    steps.push_back(s);
                }
            };

            push("start", 5);
            for (int i = 0; i < 5; ++i)
            {
                s.config.lensFlare.perimeterPosition += 0.013f;
                push("sun-move", 1);
            }
            push("still", 4);
            s.config.lensFlare.rotationRate = 0.3f;
            push("rotation-on", 5);
            // The clock paused with the rays spinning: the phase holds, so the
            // buffer may be reused although the rate is not 0.
            s.dt = 0.0f;
            push("clock-paused", 4);
            s.dt = DT;
            push("clock-resumed", 2);
            s.config.lensFlare.rotationRate = 0.0f;
            push("rotation-off", 5);
            s.config.lensFlare.intensity = 1.6f;
            push("intensity", 4);
            s.config.lensFlare.ghostColor = glm::vec3(0.4f, 0.8f, 1.0f);
            s.config.lensFlare.ghostTint = 0.5f;
            push("ghost-colour", 4);
            for (int i = 0; i < 4; ++i)
            {
                s.config.geometry.position += glm::vec2(7.5f, -3.25f);
                push("rect-move", 1);
            }
            push("still", 3);
            s.viewport = glm::ivec2(1023, 577);
            push("viewport", 4);
            s.viewport = glm::ivec2(1280, 720);
            push("viewport-back", 4);
            s.config.lensFlare.enable = false;
            push("flare-off", 2);
            s.config.lensFlare.enable = true;
            push("flare-on", 4);
            s.config.lensFlare.resolutionScale = 0.25f;
            push("scale-0.25", 4);
            s.config.lensFlare.resolutionScale = 1.0f;
            push("scale-1.0", 3);
            s.config.lensFlare.resolutionScale = 0.5f;
            push("scale-0.5", 4);
            // The sun moved away and back between two frames that otherwise
            // match: the key holds the position itself, so the second redraws
            // and then reuses.
            s.config.lensFlare.perimeterPosition += 0.25f;
            push("sun-jump", 1);
            s.config.lensFlare.perimeterPosition -= 0.25f;
            push("sun-back", 4);
            return steps;
        }

        void SetFlareBypass(EdgeLightingEffect &effect, bool bypass)
        {
            for (const std::shared_ptr<BaseRenderer> &r : effect.GetRenderers())
            {
                if (const std::shared_ptr<LensFlareRenderer> flare = std::dynamic_pointer_cast<LensFlareRenderer>(r))
                {
                    flare->SetBypass(bypass);
                }
            }
        }

        void CreateFlareEffect(EdgeLightingEffect &effect, bool withNeon, bool bypass)
        {
            effect.Initialize();
            if ((withNeon && !effect.AddRenderer(RendererLayer::NEON)) || !effect.AddRenderer(RendererLayer::LENS_FLARE))
            {
                std::fprintf(stderr, "neon-scale-check: a renderer failed to initialise\n");
                std::exit(2);
            }
            SetFlareBypass(effect, bypass);
        }

        /// Largest per-channel difference, and how many pixels differ at all.
        int Difference(const RGBA &a, const RGBA &b, long &differing)
        {
            int worst = 0;
            differing = 0;
            for (size_t i = 0; i < a.size(); i += 4)
            {
                int d = 0;
                for (size_t k = 0; k < 4; ++k)
                {
                    d = std::max(d, std::abs(int(a[i + k]) - int(b[i + k])));
                }
                worst = std::max(worst, d);
                differing += d != 0;
            }
            return worst;
        }

        /// Minimum over 5 runs of the mean frame time of 40 still frames -
        /// Update(1/60) + Render + glFinish each - after 10 warm-up frames, on
        /// a freshly initialised flare-only effect. ms.
        double TimeStill(int width, int height, bool bypass)
        {
            EdgeLightingEffect effect;
            CreateFlareEffect(effect, false, bypass);
            Config config = FlareBaseConfig();
            config.neon.enable = false;
            effect.SetConfig(config);
            effect.Update(0.0f);
            OffscreenCapture capture;
            capture.Begin(width, height);
            auto frame = [&]()
            {
                effect.Update(DT);
                effect.Render(width, height);
                glFinish();
            };
            for (int i = 0; i < 10; ++i)
            {
                frame();
            }
            double best = 1e30;
            for (int run = 0; run < 5; ++run)
            {
                const auto t0 = std::chrono::high_resolution_clock::now();
                for (int n = 0; n < 40; ++n)
                {
                    frame();
                }
                const auto t1 = std::chrono::high_resolution_clock::now();
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count() / 40.0);
            }
            capture.End();
            return best;
        }
    }

    int FlareReuseCheck(int argc, char **argv)
    {
        bool time = true;
        std::string dumpDir;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--no-time") == 0)
            {
                time = false;
            }
            else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            {
                dumpDir = argv[++i];
            }
        }

        const GLSession gl;
        std::printf("%s\n", RendererName().c_str());

        const std::vector<FlareStep> script = BuildScript();
        EdgeLightingEffect forced;
        EdgeLightingEffect reused;
        CreateFlareEffect(forced, true, true);
        CreateFlareEffect(reused, true, false);

        int failures = 0;
        for (size_t f = 0; f < script.size(); ++f)
        {
            const FlareStep &step = script[f];
            RGBA frames[2];
            EdgeLightingEffect *effects[2] = {&forced, &reused};
            for (int r = 0; r < 2; ++r)
            {
                effects[r]->SetConfig(step.config);
                effects[r]->Update(step.dt);
                frames[r] = RenderOnce(*effects[r], step.viewport.x, step.viewport.y);
            }
            long differing = 0;
            const int worst = Difference(frames[0], frames[1], differing);
            const bool bad = worst != 0 || frames[0].size() != frames[1].size();
            std::printf("  frame %3zu %4dx%-4d %-14s %s", f, step.viewport.x, step.viewport.y, step.events.c_str(),
                        bad ? "MISMATCH" : "identical");
            if (bad)
            {
                std::printf(" (max %d, %ld px)", worst, differing);
                ++failures;
                if (!dumpDir.empty())
                {
                    char name[256];
                    std::snprintf(name, sizeof(name), "%s/flare_f%03zu_", dumpDir.c_str(), f);
                    WritePNG4(std::string(name) + "forced.png", frames[0], step.viewport.x, step.viewport.y);
                    WritePNG4(std::string(name) + "reused.png", frames[1], step.viewport.x, step.viewport.y);
                }
            }
            std::printf("\n");
        }
        std::printf("%zu frames, %d mismatched\n", script.size(), failures);

        if (time)
        {
            const int SIZES[][2] = {{1920, 1080}, {3840, 2160}};
            std::printf("\nstill frames, flare alone at scale 0.5, ms (min of 5 x mean of 40, glFinish per frame):\n");
            std::printf("%-10s %10s %10s %8s\n", "size", "redraw", "reuse", "ratio");
            for (const auto &size : SIZES)
            {
                const double redraw = TimeStill(size[0], size[1], true);
                const double reuse = TimeStill(size[0], size[1], false);
                std::printf("%4dx%-5d %10.3f %10.3f %7.2fx\n", size[0], size[1], redraw, reuse, redraw / reuse);
            }
        }

        if (failures)
        {
            std::printf("FAIL: %d frame(s) differ between the reused and the redrawn flare.\n", failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
