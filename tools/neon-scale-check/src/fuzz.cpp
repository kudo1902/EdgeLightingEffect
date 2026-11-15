#include "fuzz.h"

#include "harness.h"

#include "animation/neon-animations.h"
#include "gl/gl-header.h"
#include "renderer/base-renderer.h"
#include "renderer/neon-tuning.h"
#include "util/capture-util.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        const float FRAME_DT = 1.0f / 60.0f;
        /// Runs per phase, and frames per run where a phase has more than one.
        const int FUZZ_SEQ_CONFIGS = 600;
        const int FUZZ_SEQ_FRAMES = 3;
        const int FUZZ_FRESH_CONFIGS = 120;
        const int FUZZ_DRIFT_RUNS = 8;
        const int FUZZ_DRIFT_FRAMES = 150;
        const int FUZZ_ANIM_RUNS = 50;
        const int FUZZ_ANIM_FRAMES = 60;
        /// An anim run captures every FUZZ_ANIM_STRIDE-th frame.
        const int FUZZ_ANIM_STRIDE = 6;
        /// The share of configs that draw onto a solid background rather than
        /// transparent black, and of seq configs that move to another viewport.
        const float SOLID_BACKGROUND_SHARE = 0.4f;
        const float VIEWPORT_CHANGE_SHARE = 0.2f;
        const int VIEWPORTS[][2] = {{1280, 720}, {1920, 1080}, {800, 600}, {1001, 577}, {640, 1136}, {3840, 2160}};
        /// The last viewport (4K) is drawn this rarely: it is most of the time.
        const float UHD_SHARE = 0.04f;

        /// xorshift64: the same sequence on every platform and standard
        /// library, which std::uniform_*_distribution does not promise.
        typedef struct Rng
        {
            uint64_t state;

            explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}

            uint64_t Next()
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                return state;
            }
            float U(float a, float b) { return a + (b - a) * float(double(Next() >> 11) / double(1ull << 53)); }
            int I(int a, int b) { return a + int(Next() % uint64_t(b - a + 1)); }
            bool P(float p) { return U(0.0f, 1.0f) < p; }
        } Rng;

        typedef struct View
        {
            int width;
            int height;
            glm::vec4 clear;
        } View;

        View RandomView(Rng &r)
        {
            const int k = r.P(UHD_SHARE) ? 5 : r.I(0, 4);
            View v{VIEWPORTS[k][0], VIEWPORTS[k][1], glm::vec4(0.0f)};
            if (r.P(SOLID_BACKGROUND_SHARE))
            {
                v.clear = glm::vec4(r.U(0, 1), r.U(0, 1), r.U(0, 1), 1.0f);
            }
            return v;
        }

        std::vector<ColorStop> RandomStops(Rng &r, int minStops, int maxStops)
        {
            std::vector<ColorStop> stops;
            const int n = r.I(minStops, maxStops);
            for (int i = 0; i < n; ++i)
            {
                const float alpha = r.P(0.2f) ? r.U(0.1f, 1.0f) : 1.0f;
                stops.push_back(ColorStop{r.U(0, 1), glm::vec4(r.U(0, 1), r.U(0, 1), r.U(0, 1), alpha)});
            }
            return stops;
        }

        BlendSpace RandomBlend(Rng &r) { return BlendSpace(r.I(0, 2)); }

        Cutoff RandomCutoff(Rng &r)
        {
            if (!r.P(0.3f))
            {
                return Cutoff{false, 0.0f, 0.0f};
            }
            return Cutoff{true, r.U(0, 80), r.P(0.3f) ? 0.0f : r.U(0, 20)};
        }

        SegmentBoost RandomSegment(Rng &r)
        {
            SegmentBoost s;
            s.position = r.U(0, 1);
            s.length = r.P(0.15f) ? r.U(0.002f, 0.02f) : r.U(0.02f, 0.6f);
            s.boost = r.U(0, 6);
            s.colorStops = RandomStops(r, 0, 3);
            s.blendSpace = RandomBlend(r);
            return s;
        }

        RectGeometry RandomGeometry(Rng &r, const View &v)
        {
            RectGeometry g;
            const bool tiny = r.P(0.1f);
            g.width = tiny ? r.U(4, 120) : r.U(0.2f, 1.1f) * float(v.width);
            g.height = tiny ? r.U(4, 120) : r.U(0.2f, 1.1f) * float(v.height);
            g.position = r.P(0.15f)
                             ? glm::vec2(r.U(-0.3f, 0.9f) * float(v.width), r.U(-0.3f, 0.9f) * float(v.height))
                             : glm::vec2((float(v.width) - g.width) * r.U(0, 1), (float(v.height) - g.height) * r.U(0, 1));
            const float half = 0.5f * std::min(g.width, g.height);
            g.cornerRadius = r.P(0.2f) ? 0.0f : (r.P(0.1f) ? half : r.U(0, half));
            g.winding = r.P(0.5f) ? Winding::CLOCKWISE : Winding::COUNTER_CLOCKWISE;
            return g;
        }

        void RandomLights(Rng &r, NeonConfig &n)
        {
            n.colorStops = RandomStops(r, 1, 6);
            if (!r.P(0.25f))
            {
                n.arcs.clear();
                const int arcs = r.I(0, MAX_ARCS);
                for (int i = 0; i < arcs; ++i)
                {
                    Arc a;
                    a.start = r.U(0, 1);
                    a.length = r.P(0.1f) ? 1.0f : (r.P(0.05f) ? 0.0f : r.U(0.005f, 1.0f));
                    a.intensity = r.U(0, 2.5f);
                    a.colorStops = r.P(0.5f) ? std::vector<ColorStop>() : RandomStops(r, 1, 5);
                    a.blendSpace = RandomBlend(r);
                    n.arcs.push_back(a);
                }
            }
            const int segments = r.P(0.4f) ? 0 : r.I(1, MAX_SEGMENT_BOOSTS);
            for (int i = 0; i < segments; ++i)
            {
                n.segmentBoosts.push_back(RandomSegment(r));
            }
            if (segments < MAX_SEGMENT_BOOSTS - 1 && r.P(0.2f))
            {
                const int preserved = r.I(1, 2);
                for (int i = 0; i < preserved; ++i)
                {
                    n.preservedSegmentBoosts.push_back(PreservedSegment{uint32_t(i + 1), RandomSegment(r)});
                }
            }
        }

        void RandomGlow(Rng &r, NeonConfig &n)
        {
            const float SCALES[] = {1.0f, 0.75f, 0.5f, 0.35f, 0.25f, 0.125f};
            const int LUT_SIZES[] = {16, 64, 256, 1024, 2048};
            n.enable = !r.P(0.03f);
            n.resolutionScale = r.P(0.7f) ? SCALES[r.I(0, 5)] : r.U(0.1f, 1.0f);
            n.numSamples = r.P(0.6f) ? NEON_MAX_LOOP_SAMPLES : r.I(4, NEON_MAX_LOOP_SAMPLES);
            n.gradientLutSize = LUT_SIZES[r.I(0, 4)];
            n.opaqueMode = r.P(0.6f) ? OpaqueMode::NONE : OpaqueMode(r.I(1, 4));
            n.opaqueColor = glm::vec4(r.U(0, 0.3f), r.U(0, 0.3f), r.U(0, 0.3f), r.P(0.7f) ? 1.0f : r.U(0, 1));
            n.opaqueInsideCutoff = RandomCutoff(r);
            n.opaqueOutsideCutoff = RandomCutoff(r);
            n.lineWidth = r.P(0.1f) ? r.U(0.1f, 1.0f) : r.U(1.0f, 24.0f);
            n.filamentFalloff = r.U(0.2f, 6.0f);
            n.intensity = r.P(0.08f) ? 0.0f : r.U(0.05f, 3.0f);
            n.glowRadius = r.P(0.1f) ? r.U(60, 200) : r.U(0.5f, 60);
            n.bloomStrength = r.P(0.1f) ? 0.0f : r.U(0, 2.5f);
            n.glowSide = GlowSide(r.I(0, 2));
            n.glowSideSoftness = r.P(0.5f) ? 0.0f : r.U(0, 12);
            n.insideCutoff = RandomCutoff(r);
            n.outsideCutoff = RandomCutoff(r);
            n.blendSpace = RandomBlend(r);
            n.hueRotationRate = r.P(0.4f) ? 0.0f : r.U(-2, 2);
            n.colorTransitionDuration = r.P(0.5f) ? 0.0f : r.U(0.05f, 0.5f);
        }

        Config RandomConfig(Rng &r, const View &v)
        {
            Config c;
            c.debug.showWireframe = r.P(0.15f);
            c.debug.showSharedBuffer = r.P(0.3f);
            c.debug.showGradientLUT = r.P(0.15f);
            c.debug.showColorStops = r.P(0.15f);
            c.debug.opaqueOnly = r.P(0.03f);
            c.geometry = RandomGeometry(r, v);
            RandomGlow(r, c.neon);
            RandomLights(r, c.neon);
            return c;
        }

        float Wrap01(float x) { return x - float(int(x >= 1.0f)) + float(int(x < 0.0f)); }

        /// One small change, of the kind an animating host makes frame to frame.
        void Perturb(Rng &r, Config &c)
        {
            NeonConfig &n = c.neon;
            switch (r.I(0, 15))
            {
            case 0: { c.geometry.width = std::max(2.0f, c.geometry.width + r.U(-6, 6)); break; }
            case 1: { c.geometry.position += glm::vec2(r.U(-5, 5), r.U(-5, 5)); break; }
            case 2: { c.geometry.cornerRadius = std::max(0.0f, c.geometry.cornerRadius + r.U(-3, 3)); break; }
            case 3: { n.intensity = std::max(0.0f, n.intensity * r.U(0.85f, 1.15f)); break; }
            case 4: { n.glowRadius = std::max(0.3f, n.glowRadius + r.U(-1.5f, 1.5f)); break; }
            case 5: { n.bloomStrength = std::max(0.0f, n.bloomStrength + r.U(-0.1f, 0.1f)); break; }
            case 6: { n.lineWidth = std::max(0.1f, n.lineWidth + r.U(-0.5f, 0.5f)); break; }
            case 7:
            {
                if (!n.arcs.empty())
                {
                    Arc &a = n.arcs[size_t(r.I(0, int(n.arcs.size()) - 1))];
                    a.start = Wrap01(a.start + r.U(-0.02f, 0.02f));
                    a.length = std::min(1.0f, std::max(0.0f, a.length + r.U(-0.03f, 0.03f)));
                }
                break;
            }
            case 8:
            {
                if (!n.segmentBoosts.empty())
                {
                    SegmentBoost &s = n.segmentBoosts[size_t(r.I(0, int(n.segmentBoosts.size()) - 1))];
                    s.position = Wrap01(s.position + r.U(0.0f, 0.03f));
                }
                break;
            }
            case 9: { n.hueRotationRate = r.P(0.3f) ? 0.0f : r.U(-2, 2); break; }
            case 10:
            {
                const float SCALES[] = {1.0f, 0.5f, 0.25f};
                n.resolutionScale = SCALES[r.I(0, 2)];
                break;
            }
            case 11: { n.outsideCutoff = Cutoff{!n.outsideCutoff.enable, r.U(0, 60), r.U(0, 10)}; break; }
            case 12: { n.opaqueMode = OpaqueMode(r.I(0, 4)); break; }
            case 13: { n.glowSide = GlowSide(r.I(0, 2)); break; }
            case 14: { n.colorStops = RandomStops(r, 1, 4); break; }
            default: { break; } // hold
            }
        }

        AnimationPtr RandomAnimation(Rng &r)
        {
            const float d = r.U(0.3f, 2.0f);
            switch (r.I(0, 12))
            {
            case 0: { return std::make_shared<IntensityPulse>(d, r.U(0, 0.6f), r.U(0.8f, 2.0f)); }
            case 1: { return std::make_shared<IntensityStrobe>(d * 0.3f); }
            case 2: { return std::make_shared<IntensityFadeIn>(r.U(0.5f, 2.0f), d); }
            case 3: { return std::make_shared<IntensityFadeOut>(r.U(0.5f, 2.0f), d); }
            case 4: { return std::make_shared<GlowRadiusBreath>(d, r.U(1, 6), r.U(8, 40)); }
            case 5: { return std::make_shared<BloomPulse>(d, 0.0f, r.U(0.5f, 2.0f)); }
            case 6: { return std::make_shared<HueRotationReverse>(r.U(0.1f, 1.5f), d * 2); }
            case 7: { return std::make_shared<HueRotationEaseReverse>(r.U(0.1f, 1.5f), d * 2); }
            case 8: { return std::make_shared<SegmentTravel>(d, r.U(0.05f, 0.3f), r.U(1, 5)); }
            case 9: { return std::make_shared<SegmentBounce>(d, r.U(0.05f, 0.3f), r.U(1, 5)); }
            case 10: { return std::make_shared<OutlineTracer>(d); }
            case 11: { return std::make_shared<OutlineCollapse>(d); }
            default: { return std::make_shared<ArcWipe>(d, r.U(0, 0.5f), r.U(0.5f, 1.0f), r.U(0.1f, 0.5f)); }
            }
        }

        uint64_t HashFrame(const CaptureUtil::Image &image)
        {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char b : image.pixels)
            {
                h = (h ^ b) * 1099511628211ull;
            }
            return h;
        }

        /// What the run records and, with --against, compares.
        typedef struct Recorder
        {
            FILE *out = nullptr;
            std::map<std::string, uint64_t> expected;
            std::string dumpDir;
            int frames = 0;
            int mismatched = 0;
            int missing = 0;
            int glErrorFrames = 0;
        } Recorder;

        void CaptureFrame(Recorder &rec, EdgeLightingEffect &effect, const View &v, const char *phase, int run, int frame)
        {
            OffscreenCapture capture;
            capture.Begin(v.width, v.height);
            glClearColor(v.clear.r, v.clear.g, v.clear.b, v.clear.a);
            glClear(GL_COLOR_BUFFER_BIT);
            effect.Render(v.width, v.height);
            CaptureUtil::Image image;
            capture.Read(image);
            capture.End();
            bool glError = false;
            while (glGetError() != GL_NO_ERROR)
            {
                glError = true;
            }
            rec.glErrorFrames += glError ? 1 : 0;
            ++rec.frames;

            const std::string key = std::string(phase) + " " + std::to_string(run) + " " + std::to_string(frame);
            const uint64_t hash = HashFrame(image);
            if (rec.out)
            {
                std::fprintf(rec.out, "%s %016llx\n", key.c_str(), (unsigned long long)hash);
            }
            if (rec.expected.empty())
            {
                return;
            }
            const auto it = rec.expected.find(key);
            if (it == rec.expected.end())
            {
                ++rec.missing;
                return;
            }
            if (it->second == hash)
            {
                return;
            }
            if (++rec.mismatched <= 20)
            {
                std::printf("  differs: %s (%dx%d)\n", key.c_str(), v.width, v.height);
            }
            if (!rec.dumpDir.empty())
            {
                const std::string name = rec.dumpDir + "/fuzz_" + phase + "_" + std::to_string(run) + "_" +
                                         std::to_string(frame) + ".png";
                if (!WritePNG4(name, image.pixels, v.width, v.height))
                {
                    std::fprintf(stderr, "fuzz: cannot write %s\n", name.c_str());
                }
            }
        }

        std::unique_ptr<EdgeLightingEffect> MakeEffect(const Config &config)
        {
            auto effect = std::make_unique<EdgeLightingEffect>();
            effect->Initialize();
            effect->AddRenderer(RendererLayer::NEON);
            effect->AddRenderer(RendererLayer::DEBUG);
            effect->SetConfig(config);
            effect->Update(0.0f);
            return effect;
        }

        void RunSeq(Recorder &rec, uint64_t seed)
        {
            Rng r(seed * 11 + 1);
            View v = RandomView(r);
            auto effect = MakeEffect(RandomConfig(r, v));
            for (int run = 0; run < FUZZ_SEQ_CONFIGS; ++run)
            {
                if (r.P(VIEWPORT_CHANGE_SHARE))
                {
                    v = RandomView(r);
                }
                effect->SetConfig(RandomConfig(r, v));
                for (int f = 0; f < FUZZ_SEQ_FRAMES; ++f)
                {
                    effect->Update(FRAME_DT);
                    CaptureFrame(rec, *effect, v, "seq", run, f);
                }
            }
        }

        void RunFresh(Recorder &rec, uint64_t seed)
        {
            Rng r(seed * 13 + 2);
            for (int run = 0; run < FUZZ_FRESH_CONFIGS; ++run)
            {
                const View v = RandomView(r);
                auto effect = MakeEffect(RandomConfig(r, v));
                for (int f = 0; f < 2; ++f)
                {
                    effect->Update(FRAME_DT);
                    CaptureFrame(rec, *effect, v, "fresh", run, f);
                }
            }
        }

        void RunDrift(Recorder &rec, uint64_t seed)
        {
            Rng r(seed * 17 + 3);
            for (int run = 0; run < FUZZ_DRIFT_RUNS; ++run)
            {
                const View v = RandomView(r);
                Config config = RandomConfig(r, v);
                auto effect = MakeEffect(config);
                for (int f = 0; f < FUZZ_DRIFT_FRAMES; ++f)
                {
                    const int changes = r.I(0, 3);
                    for (int k = 0; k < changes; ++k)
                    {
                        Perturb(r, config);
                    }
                    effect->SetConfig(config);
                    effect->Update(FRAME_DT);
                    CaptureFrame(rec, *effect, v, "drift", run, f);
                }
            }
        }

        void RunAnim(Recorder &rec, uint64_t seed)
        {
            Rng r(seed * 19 + 4);
            for (int run = 0; run < FUZZ_ANIM_RUNS; ++run)
            {
                const View v = RandomView(r);
                auto effect = MakeEffect(RandomConfig(r, v));
                const int count = r.I(1, 3);
                for (int k = 0; k < count; ++k)
                {
                    AnimationPtr animation = RandomAnimation(r);
                    if (r.P(0.3f))
                    {
                        animation->SetPlaybackMode(PlaybackMode::ONE_SHOT);
                    }
                    effect->Attach(animation);
                    animation->Play();
                }
                for (int f = 0; f < FUZZ_ANIM_FRAMES; ++f)
                {
                    effect->Update(FRAME_DT);
                    if (f % FUZZ_ANIM_STRIDE == FUZZ_ANIM_STRIDE - 1)
                    {
                        CaptureFrame(rec, *effect, v, "anim", run, f);
                    }
                }
            }
        }

        /// Reads a --write file: "phase run frame hash" lines, '#' comments.
        /// Returns false when it cannot be opened; @p gpu gets its GPU line.
        bool LoadHashes(const std::string &path, std::map<std::string, uint64_t> &out, std::string &gpu)
        {
            std::ifstream in(path);
            if (!in)
            {
                return false;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (line.rfind("# gpu ", 0) == 0)
                {
                    gpu = line.substr(6);
                    continue;
                }
                if (line.empty() || line[0] == '#')
                {
                    continue;
                }
                const size_t lastSpace = line.rfind(' ');
                if (lastSpace == std::string::npos)
                {
                    continue;
                }
                out[line.substr(0, lastSpace)] = std::strtoull(line.c_str() + lastSpace + 1, nullptr, 16);
            }
            return true;
        }
    }

    int FuzzCheck(int argc, char **argv)
    {
        uint64_t seed = 1;
        std::string writePath;
        std::string againstPath;
        Recorder rec;
        for (int i = 2; i < argc; ++i)
        {
            const bool hasValue = i + 1 < argc;
            if (std::strcmp(argv[i], "--seed") == 0 && hasValue)
            {
                seed = std::strtoull(argv[++i], nullptr, 10);
            }
            else if (std::strcmp(argv[i], "--write") == 0 && hasValue)
            {
                writePath = argv[++i];
            }
            else if (std::strcmp(argv[i], "--against") == 0 && hasValue)
            {
                againstPath = argv[++i];
            }
            else if (std::strcmp(argv[i], "--dump") == 0 && hasValue)
            {
                rec.dumpDir = argv[++i];
            }
        }
        if (!rec.dumpDir.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(rec.dumpDir, error);
            if (error)
            {
                std::fprintf(stderr, "fuzz: cannot create %s: %s\n", rec.dumpDir.c_str(), error.message().c_str());
                return 2;
            }
        }

        const GLSession gl;
        const std::string gpu = RendererName();
        std::printf("%s, seed %llu\n", gpu.c_str(), (unsigned long long)seed);
        if (!againstPath.empty())
        {
            std::string theirGpu;
            if (!LoadHashes(againstPath, rec.expected, theirGpu) || rec.expected.empty())
            {
                std::fprintf(stderr, "fuzz: cannot read hashes from %s\n", againstPath.c_str());
                return 2;
            }
            if (theirGpu != gpu)
            {
                std::printf("warning: %s was written on '%s'; a GPU switch alone moves pixels\n", againstPath.c_str(),
                            theirGpu.c_str());
            }
        }
        if (!writePath.empty())
        {
            rec.out = std::fopen(writePath.c_str(), "w");
            if (!rec.out)
            {
                std::fprintf(stderr, "fuzz: cannot write %s\n", writePath.c_str());
                return 2;
            }
            std::fprintf(rec.out, "# neon-scale-check fuzz, seed %llu\n# gpu %s\n", (unsigned long long)seed,
                         gpu.c_str());
        }

        RunSeq(rec, seed);
        RunFresh(rec, seed);
        RunDrift(rec, seed);
        RunAnim(rec, seed);
        if (rec.out)
        {
            std::fclose(rec.out);
        }

        // A key the other file has and this run never drew is a change too.
        const int unmatched = rec.expected.empty() ? 0 : int(rec.expected.size()) - (rec.frames - rec.missing);
        std::printf("%d frames, %d with a GL error", rec.frames, rec.glErrorFrames);
        if (!rec.expected.empty())
        {
            std::printf(", %d differ from %s, %d not in it, %d of its frames not drawn", rec.mismatched,
                        againstPath.c_str(), rec.missing, unmatched);
        }
        std::printf("\n");
        const bool pass = rec.glErrorFrames == 0 && rec.mismatched == 0 && rec.missing == 0 && unmatched == 0;
        std::printf("%s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
}
