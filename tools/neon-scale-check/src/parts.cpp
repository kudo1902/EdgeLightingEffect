#include "parts.h"

#include "harness.h"
#include "scenes.h"

#include "gl/gl-header.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include "renderer/neon-renderer.h"
#include "shaders.h"
#include "util/capture-util.h"
#include "core/buffer-registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace NeonScaleCheck
{
    namespace
    {
        /// The reader: a fullscreen triangle pair, each fragment the neon's
        /// light at its own pixel centre through neon-light.glsl, as a host
        /// would draw it - undithered, over transparent black, unblended.
        const char *const PROBE_VERT_SRC = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        std::string ProbeFragSource()
        {
            return std::string("#version 330 core\n") + ShaderSource::NEON_LIGHT_GLSL +
                   "\nout vec4 fragColor;\nvoid main() { fragColor = neonLight(gl_FragCoord.xy, 1.0); }\n";
        }

        /// The largest per-channel difference of two RGBA8 frames, and the share
        /// of lit pixels (any RGB channel at 1 or more in either) within 1.
        typedef struct Compare
        {
            int maxDelta = 0;
            long lit = 0;
            long within1 = 0;
        } Compare;

        Compare CompareFrames(const CaptureUtil::Image &a, const CaptureUtil::Image &b)
        {
            Compare c;
            const size_t n = a.pixels.size() / 4;
            for (size_t i = 0; i < n; ++i)
            {
                const unsigned char *pa = &a.pixels[4 * i];
                const unsigned char *pb = &b.pixels[4 * i];
                int d = 0;
                bool lit = false;
                for (int k = 0; k < 4; ++k)
                {
                    d = std::max(d, std::abs(int(pa[k]) - int(pb[k])));
                    lit = lit || (k < 3 && (pa[k] > 0 || pb[k] > 0));
                }
                c.maxDelta = std::max(c.maxDelta, d);
                if (lit)
                {
                    ++c.lit;
                    c.within1 += (d <= 1) ? 1 : 0;
                }
            }
            return c;
        }

        std::shared_ptr<NeonRenderer> FindNeon(EdgeLightingEffect &effect)
        {
            for (const auto &renderer : effect.GetRenderers())
            {
                if (auto neon = std::dynamic_pointer_cast<NeonRenderer>(renderer))
                {
                    return neon;
                }
            }
            return nullptr;
        }
    }

    int Shared(int argc, char **argv)
    {
        int maxAllowed = 2;
        double within1Min = 99.9;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            {
                maxAllowed = std::atoi(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--within1") == 0 && i + 1 < argc)
            {
                within1Min = std::atof(argv[++i]);
            }
        }

        const GLSession gl;
        int failing = 0;
        int worst = 0;
        {
            EdgeLightingEffect effect;
            CreateEffect(effect);
            effect.AddHostRead(NeonRenderer::COMPOSITE_KEY);

            // (shared scale, opaque fill): the fill only at 1, where the
            // shared buffer's fill IS the screen's; below 1 it is drawn at s,
            // not averaged, so only the light is compared there.
            typedef struct Case
            {
                float scale;
                OpaqueMode fill;
                const char *name;
            } Case;
            static const Case CASES[] = {{1.0f, OpaqueMode::NONE, "1"},
                                         {1.0f, OpaqueMode::INSIDE, "1+fill"},
                                         {1.0f, OpaqueMode::ALL, "1+all"},
                                         {0.5f, OpaqueMode::NONE, "0.5"}};
            static const char *const MOTIONS[] = {"still", "hue"};
            std::printf("%-15s %-7s %-6s %5s %9s %10s\n", "scene", "shared", "motion", "max", "lit px", "within 1");
            for (const Scene &scene : SCENES)
            {
                for (const Case &cs : CASES)
                {
                    for (const char *motion : MOTIONS)
                    {
                        const bool hue = std::strcmp(motion, "hue") == 0;
                        Config config = BaseConfig();
                        scene.apply(config);
                        config.neon.resolutionScale = 0.5f;
                        config.neon.compositeScale = cs.scale;
                        config.neon.opaqueMode = cs.fill;
                        config.neon.hueRotationRate = hue ? 0.5f : 0.0f;
                        effect.SetConfig(config);
                        effect.Update(0.0f);

                        OffscreenCapture screen;
                        screen.Begin(FRAME_WIDTH, FRAME_HEIGHT);
                        for (int f = 0; f < 3; ++f)
                        {
                            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                            glClear(GL_COLOR_BUFFER_BIT);
                            if (hue)
                            {
                                effect.Update(1.0f / 60.0f);
                            }
                            effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
                        }
                        CaptureUtil::Image a;
                        screen.Read(a);
                        screen.End();

                        const BufferRegistry &registry = effect.GetBufferRegistry();
                        const BufferRecord *record = registry.Find(NeonRenderer::COMPOSITE_KEY);
                        if (!record || !registry.IsCurrent(*record))
                        {
                            std::printf("%-15s %-7s %-6s  the shared buffer was not current\n", scene.id, cs.name,
                                        motion);
                            ++failing;
                            continue;
                        }
                        const glm::ivec2 texels = record->GetTexels();
                        CaptureUtil::Image b;
                        CaptureUtil::ReadTexture2D(record->GetTexture(), texels.x, texels.y, b);

                        // Each texel against the screen averaged over its
                        // square: texel (x, t) - t from the bottom - covers
                        // caller px origin + (x, t) / s, 1 / s wide.
                        const float sc = cs.scale;
                        const int span = static_cast<int>(1.0f / sc + 0.5f);
                        const glm::ivec2 org = glm::ivec2(glm::round(record->origin * sc));
                        Compare c;
                        for (int t = 0; t < texels.y; ++t)
                        {
                            for (int x = 0; x < texels.x; ++x)
                            {
                                const int px0 = (org.x + x) * span;
                                const int py0 = (org.y + t) * span;
                                if (px0 + span > FRAME_WIDTH || py0 + span > FRAME_HEIGHT)
                                {
                                    continue;
                                }
                                int sum[4] = {0, 0, 0, 0};
                                for (int dy = 0; dy < span; ++dy)
                                {
                                    for (int dx = 0; dx < span; ++dx)
                                    {
                                        const int row = FRAME_HEIGHT - 1 - (py0 + dy);
                                        const unsigned char *pa = &a.pixels[4 * (size_t(row) * FRAME_WIDTH + (px0 + dx))];
                                        for (int k = 0; k < 4; ++k)
                                        {
                                            sum[k] += pa[k];
                                        }
                                    }
                                }
                                const unsigned char *pb =
                                    &b.pixels[4 * (size_t(texels.y - 1 - t) * texels.x + x)];
                                int d = 0;
                                bool lit = false;
                                for (int k = 0; k < 4; ++k)
                                {
                                    const float expect = float(sum[k]) / float(span * span);
                                    d = std::max(d, static_cast<int>(std::lround(std::fabs(expect - float(pb[k])))));
                                    lit = lit || (k < 3 && (expect >= 1.0f || pb[k] > 0));
                                }
                                c.maxDelta = std::max(c.maxDelta, d);
                                if (lit)
                                {
                                    ++c.lit;
                                    c.within1 += (d <= 1) ? 1 : 0;
                                }
                            }
                        }
                        const double share = c.lit ? 100.0 * double(c.within1) / double(c.lit) : 100.0;
                        const bool fail = c.maxDelta > maxAllowed || share < within1Min;
                        worst = std::max(worst, c.maxDelta);
                        failing += fail ? 1 : 0;
                        std::printf("%-15s %-7s %-6s %5d %9ld %9.3f%%%s\n", scene.id, cs.name, motion, c.maxDelta,
                                    c.lit, share, fail ? "  !" : "");
                    }
                }
            }
        }
        std::printf("worst max %d\n", worst);
        if (failing)
        {
            std::printf("FAIL: %d frame(s) outside max %d / %.2f%% within 1\n", failing, maxAllowed, within1Min);
            return 1;
        }
        std::printf("PASS (max %d, %.2f%% within 1)\n", maxAllowed, within1Min);
        return 0;
    }

    int Parts(int argc, char **argv)
    {
        int maxAllowed = 1;
        double within1Min = 99.9;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            {
                maxAllowed = std::atoi(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--within1") == 0 && i + 1 < argc)
            {
                within1Min = std::atof(argv[++i]);
            }
        }

        const GLSession gl;
        int failing = 0;
        int worst = 0;
        {
            EdgeLightingEffect effect;
            CreateEffect(effect);
            // The two parts, read by the host: the ring atlas is then drawn on
            // every frame, still or moving.
            effect.AddHostRead(NeonRenderer::GLOW_KEY);
            effect.AddHostRead(NeonRenderer::RING_KEY);
            const std::shared_ptr<NeonRenderer> neon = FindNeon(effect);

            ShaderProgram probe(PROBE_VERT_SRC, ProbeFragSource().c_str(), "neon-scale-check.Parts");
            if (!neon || !probe.IsValid())
            {
                std::printf("FAIL: no neon renderer, or the probe did not build\n");
                return 1;
            }
            VertexArray quad("neon-scale-check.PartsQuad");
            const float verts[] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
            quad.SetVertexData(verts, sizeof(verts));
            quad.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

            static const float SCALES_HERE[] = {1.0f, 0.5f, 0.25f};
            static const char *const MOTIONS[] = {"still", "hue"};
            std::printf("%-15s %-6s %-6s %5s %9s %10s\n", "scene", "scale", "motion", "max", "lit px", "within 1");
            for (const Scene &scene : SCENES)
            {
                for (float scale : SCALES_HERE)
                {
                    for (const char *motion : MOTIONS)
                    {
                        const bool hue = std::strcmp(motion, "hue") == 0;
                        Config config = BaseConfig();
                        scene.apply(config);
                        config.neon.resolutionScale = scale;
                        // The light alone: the parts carry no fill.
                        config.neon.opaqueMode = OpaqueMode::NONE;
                        config.neon.hueRotationRate = hue ? 0.5f : 0.0f;
                        effect.SetConfig(config);
                        effect.Update(0.0f);

                        // Three frames - the field's first-frame bake and its
                        // settle - and the third compared.
                        OffscreenCapture screen;
                        screen.Begin(FRAME_WIDTH, FRAME_HEIGHT);
                        CaptureUtil::Image a;
                        for (int f = 0; f < 3; ++f)
                        {
                            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                            glClear(GL_COLOR_BUFFER_BIT);
                            if (hue)
                            {
                                effect.Update(1.0f / 60.0f);
                            }
                            effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
                        }
                        screen.Read(a);
                        screen.End();

                        const NeonRenderer::SharedLight light = neon->GetSharedLight();
                        if (!light.valid)
                        {
                            std::printf("%-15s %-6g %-6s  the parts were not current\n", scene.id, scale, motion);
                            ++failing;
                            continue;
                        }
                        OffscreenCapture read;
                        read.Begin(FRAME_WIDTH, FRAME_HEIGHT);
                        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glDisable(GL_BLEND);
                        probe.Use();
                        NeonRenderer::UploadSharedLight(probe, light, 0, 1);
                        quad.DrawArrays(GL_TRIANGLES, 6);
                        probe.Unuse();
                        CaptureUtil::Image b;
                        read.Read(b);
                        read.End();

                        const Compare c = CompareFrames(a, b);
                        const double share = c.lit ? 100.0 * double(c.within1) / double(c.lit) : 100.0;
                        const bool fail = c.maxDelta > maxAllowed || share < within1Min;
                        worst = std::max(worst, c.maxDelta);
                        failing += fail ? 1 : 0;
                        std::printf("%-15s %-6g %-6s %5d %9ld %9.3f%%%s\n", scene.id, scale, motion, c.maxDelta, c.lit,
                                    share, fail ? "  !" : "");
                    }
                }
            }
        }
        std::printf("worst max %d\n", worst);
        if (failing)
        {
            std::printf("FAIL: %d frame(s) outside max %d / %.2f%% within 1\n", failing, maxAllowed, within1Min);
            return 1;
        }
        std::printf("PASS (max %d, %.2f%% within 1)\n", maxAllowed, within1Min);
        return 0;
    }
}
