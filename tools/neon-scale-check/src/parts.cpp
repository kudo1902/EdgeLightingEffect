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
#include <vector>

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

        /// Where a gate scene puts the page's rect.
        typedef enum class Placement
        {
            AS_IS, ///< Where the page has it.
            /// Across the frame's top-right corner: the ring, whose packing ends
            /// at the viewport, and the shared buffer's region, which its
            /// rounding carries past it, both cross the edge.
            EDGE,
            /// A thin band round the whole frame: the glow's quad fills too
            /// little of its box for the glow's field (FIELD_MIN_FILL), while
            /// the ring's field serves - so under a moving intensity the ring
            /// is composited scaled while the glow is shaded exactly.
            BAND,
        } Placement;

        /// A scene of these two gates: one of the page's twelve, placed.
        typedef struct GateScene
        {
            const char *id;
            const Scene *scene;
            Placement placement;
        } GateScene;

        std::vector<GateScene> GateScenes()
        {
            std::vector<GateScene> scenes;
            for (const Scene &scene : SCENES)
            {
                scenes.push_back({scene.id, &scene, Placement::AS_IS});
            }
            for (const Scene &scene : SCENES)
            {
                if (std::strcmp(scene.id, "default") == 0)
                {
                    scenes.push_back({"default@edge", &scene, Placement::EDGE});
                    scenes.push_back({"default@band", &scene, Placement::BAND});
                }
                else if (std::strcmp(scene.id, "hairline") == 0)
                {
                    scenes.push_back({"hairline@edge", &scene, Placement::EDGE});
                    scenes.push_back({"hairline@band", &scene, Placement::BAND});
                }
                else if (std::strcmp(scene.id, "overdrive") == 0)
                {
                    // Its 1.5 bloom is where the field scaled to a moving
                    // intensity is furthest from the shading (6 levels, I56).
                    scenes.push_back({"overdrive@band", &scene, Placement::BAND});
                }
            }
            return scenes;
        }

        Config GateConfig(const GateScene &gate)
        {
            Config config = BaseConfig();
            gate.scene->apply(config);
            if (gate.placement == Placement::EDGE)
            {
                // The 640 x 360 rect 200 px past the right edge and 150 px past
                // the top (app px, +y down).
                config.geometry.position = glm::vec2(float(FRAME_WIDTH) - 440.0f, -150.0f);
            }
            else if (gate.placement == Placement::BAND)
            {
                // 40 px in from every edge, with a short glow.
                config.geometry.position = glm::vec2(40.0f, 40.0f);
                config.geometry.width = float(FRAME_WIDTH) - 80.0f;
                config.geometry.height = float(FRAME_HEIGHT) - 80.0f;
                config.neon.glowRadius = std::min(config.neon.glowRadius, 8.0f);
            }
            return config;
        }

        /// What happens over the frames before the one compared.
        typedef enum class Motion
        {
            STILL,     ///< Three frames of one config.
            HUE,       ///< The hue rotating.
            INTENSITY, ///< The intensity falling for eight frames, then held for two: the field's scaled
                       ///< composite while it moves (I56), and the frame it stops on.
        } Motion;

        const char *MotionName(Motion motion)
        {
            switch (motion)
            {
            case Motion::STILL:
            {
                return "still";
            }
            case Motion::HUE:
            {
                return "hue";
            }
            case Motion::INTENSITY:
            {
                return "intens";
            }
            }
            return "?";
        }

        void DrawFrame(EdgeLightingEffect &effect, float dt)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            effect.Update(dt);
            effect.Render(FRAME_WIDTH, FRAME_HEIGHT);
        }

        /// Draws @p config as @p motion reaches it into @p screen, and returns
        /// the config the last frame drew.
        Config DrawMotion(EdgeLightingEffect &effect, Config config, Motion motion, CaptureUtil::Image &screen)
        {
            config.neon.hueRotationRate = (motion == Motion::HUE) ? 0.5f : 0.0f;
            OffscreenCapture capture;
            capture.Begin(FRAME_WIDTH, FRAME_HEIGHT);
            if (motion == Motion::INTENSITY)
            {
                const float base = config.neon.intensity;
                for (int f = 0; f < 8; ++f)
                {
                    config.neon.intensity = base * (1.0f - 0.06f * float(f));
                    effect.SetConfig(config);
                    DrawFrame(effect, 1.0f / 60.0f);
                }
                for (int f = 0; f < 2; ++f)
                {
                    DrawFrame(effect, 1.0f / 60.0f);
                }
            }
            else
            {
                effect.SetConfig(config);
                effect.Update(0.0f);
                // Three frames - the field's first-frame bake and its settle -
                // and the third compared.
                for (int f = 0; f < 3; ++f)
                {
                    DrawFrame(effect, (motion == Motion::HUE) ? 1.0f / 60.0f : 0.0f);
                }
            }
            capture.Read(screen);
            capture.End();
            return config;
        }

        /// The frame @p config draws with no history: another line width first,
        /// which resets every schedule a moving intensity keeps (LazyBake), then
        /// three frames of @p config.
        void DrawFresh(EdgeLightingEffect &effect, const Config &config, CaptureUtil::Image &screen)
        {
            Config other = config;
            other.neon.lineWidth += 1.0f;
            effect.SetConfig(other);
            OffscreenCapture capture;
            capture.Begin(FRAME_WIDTH, FRAME_HEIGHT);
            DrawFrame(effect, 0.0f);
            effect.SetConfig(config);
            for (int f = 0; f < 3; ++f)
            {
                DrawFrame(effect, 0.0f);
            }
            capture.Read(screen);
            capture.End();
        }

        /// The screen's light over the square [lo, lo + footprint] of caller
        /// px (y up), averaged as neon-assemble.frag averages it: each pixel
        /// weighted by how much of it the square covers, and a pixel past the
        /// frame read at the nearest pixel inside (neonLightClampPx).
        void ScreenOverSquare(const CaptureUtil::Image &screen, glm::vec2 lo, float footprint, float out[4])
        {
            const glm::vec2 hi = lo + glm::vec2(footprint);
            const glm::ivec2 j0(int(std::floor(lo.x)), int(std::floor(lo.y)));
            const glm::ivec2 j1(int(std::ceil(hi.x)) - 1, int(std::ceil(hi.y)) - 1);
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            for (int y = j0.y; y <= j1.y; ++y)
            {
                const double wy = std::min(double(hi.y), double(y + 1)) - std::max(double(lo.y), double(y));
                const int cy = std::clamp(y, 0, FRAME_HEIGHT - 1);
                const int row = FRAME_HEIGHT - 1 - cy;
                for (int x = j0.x; x <= j1.x; ++x)
                {
                    const double wx = std::min(double(hi.x), double(x + 1)) - std::max(double(lo.x), double(x));
                    const int cx = std::clamp(x, 0, FRAME_WIDTH - 1);
                    const unsigned char *p = &screen.pixels[4 * (size_t(row) * FRAME_WIDTH + cx)];
                    for (int k = 0; k < 4; ++k)
                    {
                        sum[k] += double(p[k]) * wx * wy;
                    }
                }
            }
            for (int k = 0; k < 4; ++k)
            {
                out[k] = float(sum[k] / (double(footprint) * double(footprint)));
            }
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
            // not averaged, so only the light is compared there. 0.4 is the
            // floor (COMPOSITE_MIN_SCALE), where a texel's square is 2.5 px and
            // lands on no whole pixel.
            typedef struct Case
            {
                float scale;
                OpaqueMode fill;
                const char *name;
            } Case;
            static const Case CASES[] = {{1.0f, OpaqueMode::NONE, "1"},
                                         {1.0f, OpaqueMode::INSIDE, "1+fill"},
                                         {1.0f, OpaqueMode::ALL, "1+all"},
                                         {0.5f, OpaqueMode::NONE, "0.5"},
                                         {0.4f, OpaqueMode::NONE, "0.4"}};
            static const Motion MOTIONS[] = {Motion::STILL, Motion::HUE, Motion::INTENSITY};
            std::printf("%-15s %-7s %-6s %5s %9s %10s\n", "scene", "shared", "motion", "max", "lit px", "within 1");
            for (const GateScene &gate : GateScenes())
            {
                for (const Case &cs : CASES)
                {
                    for (const Motion motion : MOTIONS)
                    {
                        Config config = GateConfig(gate);
                        config.neon.resolutionScale = std::min(0.5f, cs.scale);
                        config.neon.compositeScale = cs.scale;
                        config.neon.opaqueMode = cs.fill;
                        CaptureUtil::Image a;
                        const Config drawn = DrawMotion(effect, config, motion, a);

                        const BufferRegistry &registry = effect.GetBufferRegistry();
                        const BufferRecord *record = registry.Find(NeonRenderer::COMPOSITE_KEY);
                        if (!record || !registry.IsCurrent(*record))
                        {
                            std::printf("%-15s %-7s %-6s  the shared buffer was not current\n", gate.id, cs.name,
                                        MotionName(motion));
                            ++failing;
                            continue;
                        }
                        const glm::ivec2 texels = record->GetTexels();
                        CaptureUtil::Image b;
                        CaptureUtil::ReadTexture2D(record->GetTexture(), texels.x, texels.y, b);

                        // Every texel - those past the frame included, which
                        // hold the light at its edge - against the screen
                        // averaged over its square: texel (x, t), t from the
                        // bottom, covers caller px origin + (x, t) / s, 1 / s
                        // wide.
                        const float footprint = 1.0f / cs.scale;
                        Compare c;
                        for (int t = 0; t < texels.y; ++t)
                        {
                            for (int x = 0; x < texels.x; ++x)
                            {
                                float expect[4];
                                ScreenOverSquare(a, record->origin + glm::vec2(float(x), float(t)) * footprint,
                                                 footprint, expect);
                                const unsigned char *pb = &b.pixels[4 * (size_t(texels.y - 1 - t) * texels.x + x)];
                                int d = 0;
                                bool lit = false;
                                for (int k = 0; k < 4; ++k)
                                {
                                    d = std::max(d, static_cast<int>(std::lround(std::fabs(expect[k] - float(pb[k])))));
                                    lit = lit || (k < 3 && (expect[k] >= 1.0f || pb[k] > 0));
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
                        bool fail = c.maxDelta > maxAllowed || share < within1Min;
                        worst = std::max(worst, c.maxDelta);
                        std::printf("%-15s %-7s %-6s %5d %9ld %9.3f%%%s\n", gate.id, cs.name, MotionName(motion),
                                    c.maxDelta, c.lit, share, fail ? "  !" : "");

                        // The frame the intensity stops on must be the frame
                        // the config draws from nothing: no scaled ring kept
                        // in the ring atlas or the shared buffer past it.
                        if (motion == Motion::INTENSITY)
                        {
                            CaptureUtil::Image fresh;
                            DrawFresh(effect, drawn, fresh);
                            const Compare h = CompareFrames(a, fresh);
                            const double heldShare = h.lit ? 100.0 * double(h.within1) / double(h.lit) : 100.0;
                            const bool heldFail = h.maxDelta > 1 || heldShare < within1Min;
                            worst = std::max(worst, h.maxDelta);
                            fail = fail || heldFail;
                            std::printf("%-15s %-7s %-6s %5d %9ld %9.3f%%%s\n", gate.id, cs.name, "held", h.maxDelta,
                                        h.lit, heldShare, heldFail ? "  !" : "");
                        }
                        failing += fail ? 1 : 0;
                    }
                }
            }
        }
        std::printf("worst max %d\n", worst);
        if (failing)
        {
            std::printf("FAIL: %d frame(s) outside max %d / %.2f%% within 1 (held frames: max 1)\n", failing,
                        maxAllowed, within1Min);
            return 1;
        }
        std::printf("PASS (max %d, %.2f%% within 1; held frames max 1)\n", maxAllowed, within1Min);
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
            static const Motion MOTIONS[] = {Motion::STILL, Motion::HUE, Motion::INTENSITY};
            std::printf("%-15s %-6s %-6s %5s %9s %10s\n", "scene", "scale", "motion", "max", "lit px", "within 1");
            for (const GateScene &gate : GateScenes())
            {
                for (float scale : SCALES_HERE)
                {
                    for (const Motion motion : MOTIONS)
                    {
                        Config config = GateConfig(gate);
                        config.neon.resolutionScale = scale;
                        // The light alone: the parts carry no fill.
                        config.neon.opaqueMode = OpaqueMode::NONE;
                        CaptureUtil::Image a;
                        const Config drawn = DrawMotion(effect, config, motion, a);

                        const NeonRenderer::SharedLight light = neon->GetSharedLight();
                        if (!light.valid)
                        {
                            std::printf("%-15s %-6g %-6s  the parts were not current\n", gate.id, scale,
                                        MotionName(motion));
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
                        bool fail = c.maxDelta > maxAllowed || share < within1Min;
                        worst = std::max(worst, c.maxDelta);
                        std::printf("%-15s %-6g %-6s %5d %9ld %9.3f%%%s\n", gate.id, scale, MotionName(motion),
                                    c.maxDelta, c.lit, share, fail ? "  !" : "");

                        // As in `shared`: the frame the intensity stops on is
                        // the frame the config draws from nothing.
                        if (motion == Motion::INTENSITY)
                        {
                            CaptureUtil::Image fresh;
                            DrawFresh(effect, drawn, fresh);
                            const Compare h = CompareFrames(a, fresh);
                            const double heldShare = h.lit ? 100.0 * double(h.within1) / double(h.lit) : 100.0;
                            const bool heldFail = h.maxDelta > 1 || heldShare < within1Min;
                            worst = std::max(worst, h.maxDelta);
                            fail = fail || heldFail;
                            std::printf("%-15s %-6g %-6s %5d %9ld %9.3f%%%s\n", gate.id, scale, "held", h.maxDelta,
                                        h.lit, heldShare, heldFail ? "  !" : "");
                        }
                        failing += fail ? 1 : 0;
                    }
                }
            }
        }
        std::printf("worst max %d\n", worst);
        if (failing)
        {
            std::printf("FAIL: %d frame(s) outside max %d / %.2f%% within 1 (held frames: max 1)\n", failing,
                        maxAllowed, within1Min);
            return 1;
        }
        std::printf("PASS (max %d, %.2f%% within 1; held frames max 1)\n", maxAllowed, within1Min);
        return 0;
    }
}
