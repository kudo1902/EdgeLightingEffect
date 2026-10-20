#include "shared-buffer.h"

#include "harness.h"
#include "scenes.h"

#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "gl/gl-header.h"
#include "renderer/base-renderer.h"
#include "util/geometry-utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        /// The share of the frame's light ON THE LINE (Rec. 709 luma above the
        /// clear, over the pixels within the filament's half-width of the
        /// edge) the buffer has to carry, blitted back, on a scene nothing
        /// masks. Measured on the line because the halo and the bloom carry
        /// most of a frame's light: without the filament the whole frame still
        /// came back at 0.93-0.995 of it, the line itself far lower.
        const double SHARED_MIN_LINE_ENERGY = 0.9;

        /// The page's clear, as harness.cpp's Render draws it.
        const float CLEAR[3] = {5.0f / 255.0f, 5.0f / 255.0f, 8.0f / 255.0f};

        /// What a scene's frame is drawn with besides the scene itself.
        typedef struct Variant
        {
            const char *id;
            float hueRotationRate; ///< 0: still, pass 1b shades. Non-zero: the field composites in its place.
        } Variant;

        const Variant VARIANTS[] = {{"still", 0.0f}, {"hue", 0.5f}};

        /// What a reader saw of the shared buffer in one Render.
        typedef struct Seen
        {
            bool ready = false;
            uint32_t version = 0;
            glm::ivec2 viewport{0};
            glm::ivec2 texels{0};
            GLuint texture = 0;
        } Seen;

        /// An internal reader of the neon's shared buffer, as a library
        /// renderer would be one: it takes the buffer in every Render and
        /// records what it was handed. Draws nothing. The check registers one
        /// after the neon and one before it: the effect runs every layer's
        /// offscreen phase before any layer's Render, so both must see the
        /// same image.
        class SharedBufferProbe : public BaseRenderer
        {
        public:
            bool Initialize() override { return true; }
            void Update(float, double, const Config &) override {}
            void Render(int, int, double, const Config &) override
            {
                const std::shared_ptr<const SharedBuffer> buffer = GetSharedBufferRegistry()->Find(SharedBufferId::NEON);
                mSeen = Seen{};
                if (buffer)
                {
                    mSeen = Seen{buffer->IsReady(), buffer->GetVersion(), buffer->GetViewport(), buffer->GetTexels(),
                                 buffer->GetTextureId()};
                }
            }
            void OnConfigChanged(const Config &) override {}
            RendererLayer GetLayer() const override { return RendererLayer::CUSTOM; }

            /// What the last Render was handed.
            const Seen &Last() const { return mSeen; }

        private:
            Seen mSeen;
        };

        /// Whether anything but the glow lands on the frame or masks it: the
        /// one-sided cut, a cutoff, the opaque fill. The buffer holds none of
        /// those, so its light is compared only where none is set.
        bool IsUnmasked(const Config &c)
        {
            return c.neon.glowSide == GlowSide::BOTH && !c.neon.insideCutoff.enable &&
                   !c.neon.outsideCutoff.enable && c.neon.opaqueMode == OpaqueMode::NONE;
        }

        /// The buffer's texels as RGBA floats, bottom row first, read through
        /// a framebuffer of our own over its texture: a reader samples the
        /// shared buffer's texture, never binds the producer's framebuffer.
        std::vector<float> ReadBuffer(const Seen &seen)
        {
            const int w = seen.texels.x;
            const int h = seen.texels.y;
            std::vector<float> texels(size_t(w) * h * 4, 0.0f);
            GLint previous = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, seen.texture, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, texels.data());
            }
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
            glDeleteFramebuffers(1, &fbo);
            return texels;
        }

        /// The buffer as the neon's blit reads it - bilinear, clamped to its
        /// edge, at each pixel centre's uv over the viewport - composited over
        /// the clear, with no cut, cutoffs or dither: FRAME_WIDTH x
        /// FRAME_HEIGHT RGB, top row first, like Render's frames.
        RGB BlitBack(const Seen &seen, const std::vector<float> &texels)
        {
            RGB out(size_t(FRAME_WIDTH) * FRAME_HEIGHT * 3);
            const int w = seen.texels.x;
            const int h = seen.texels.y;
            auto texel = [&](int x, int y, int k)
            {
                x = std::min(std::max(x, 0), w - 1);
                y = std::min(std::max(y, 0), h - 1);
                return texels[(size_t(y) * w + x) * 4 + k];
            };
            for (int row = 0; row < FRAME_HEIGHT; ++row)
            {
                const int y = FRAME_HEIGHT - 1 - row; // gl_FragCoord rows run up
                for (int x = 0; x < FRAME_WIDTH; ++x)
                {
                    const float u = (float(x) + 0.5f) / float(seen.viewport.x);
                    const float v = (float(y) + 0.5f) / float(seen.viewport.y);
                    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
                    {
                        const float fx = u * static_cast<float>(w) - 0.5f;
                        const float fy = v * static_cast<float>(h) - 0.5f;
                        const int x0 = int(std::floor(fx));
                        const int y0 = int(std::floor(fy));
                        const float ax = fx - static_cast<float>(x0);
                        const float ay = fy - static_cast<float>(y0);
                        for (int k = 0; k < 4; ++k)
                        {
                            const float bottom = texel(x0, y0, k) * (1.0f - ax) + texel(x0 + 1, y0, k) * ax;
                            const float top = texel(x0, y0 + 1, k) * (1.0f - ax) + texel(x0 + 1, y0 + 1, k) * ax;
                            rgba[k] = bottom * (1.0f - ay) + top * ay;
                        }
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        const float c = rgba[k] + CLEAR[k] * (1.0f - rgba[3]);
                        out[(size_t(row) * FRAME_WIDTH + x) * 3 + k] =
                            static_cast<unsigned char>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f));
                    }
                }
            }
            return out;
        }

        /// The buffer over black, top row first, its own size.
        RGB BufferImage(const Seen &seen, const std::vector<float> &texels)
        {
            const int w = seen.texels.x;
            const int h = seen.texels.y;
            RGB out(size_t(w) * h * 3);
            for (int row = 0; row < h; ++row)
            {
                const int y = h - 1 - row;
                for (int x = 0; x < w; ++x)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        const float c = texels[(size_t(y) * w + x) * 4 + k];
                        out[(size_t(row) * w + x) * 3 + k] =
                            static_cast<unsigned char>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f));
                    }
                }
            }
            return out;
        }

        /// The light of @p image on the line of @p config's rect - Rec. 709
        /// luma above the clear, summed over the pixels whose centre lies
        /// within the filament's half-width (at least 1.5 px) of the edge -
        /// against @p reference's over the same pixels.
        double LineEnergyRatio(const RGB &image, const RGB &reference, const Config &config)
        {
            const glm::vec2 half(config.geometry.width * 0.5f, config.geometry.height * 0.5f);
            const glm::vec2 centre = config.geometry.position + half;
            const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry);
            const float band = std::max(config.neon.lineWidth * 0.5f, 1.5f);
            const double clear = 0.2126 * 5.0 + 0.7152 * 5.0 + 0.0722 * 8.0;
            double sumImage = 0.0;
            double sumReference = 0.0;
            for (int y = 0; y < FRAME_HEIGHT; ++y)
            {
                for (int x = 0; x < FRAME_WIDTH; ++x)
                {
                    // sdRoundBox, in app px (top-left origin: the box is symmetric).
                    const glm::vec2 p = glm::vec2(float(x) + 0.5f, float(y) + 0.5f) - centre;
                    const glm::vec2 q = glm::abs(p) - half + glm::vec2(radius);
                    const float d = std::min(std::max(q.x, q.y), 0.0f) + glm::length(glm::max(q, glm::vec2(0.0f))) - radius;
                    if (std::fabs(d) > band)
                    {
                        continue;
                    }
                    const size_t i = (size_t(y) * FRAME_WIDTH + x) * 3;
                    sumImage += 0.2126 * image[i] + 0.7152 * image[i + 1] + 0.0722 * image[i + 2] - clear;
                    sumReference += 0.2126 * reference[i] + 0.7152 * reference[i + 1] + 0.0722 * reference[i + 2] - clear;
                }
            }
            return sumReference != 0.0 ? sumImage / sumReference : 1.0;
        }

        /// Whether @p seen describes a frame drawn at FRAME_WIDTH x
        /// FRAME_HEIGHT at @p config's scale: that viewport, and the whole of
        /// it at the scale, floor(viewport * scale) texels.
        bool DescribesFrame(const Seen &seen, const Config &config)
        {
            const glm::ivec2 texels(int(float(FRAME_WIDTH) * config.neon.resolutionScale),
                                    int(float(FRAME_HEIGHT) * config.neon.resolutionScale));
            return seen.viewport == glm::ivec2(FRAME_WIDTH, FRAME_HEIGHT) && seen.texels == texels &&
                   seen.texture != 0;
        }

        /// @p from's rect at half its size and moved off its centre, so most of
        /// the glow @p from drew lies outside the window the moved rect's write
        /// draws into.
        Config MovedRect(const Config &from)
        {
            Config to = from;
            to.geometry.width *= 0.5f;
            to.geometry.height *= 0.5f;
            to.geometry.position.x += from.geometry.width * 0.4f;
            to.geometry.position.y += from.geometry.height * 0.3f;
            return to;
        }

        /// How many texels of @p seen differ at all from @p reference's, both
        /// read back with ReadBuffer; -1 when either is not ready or the two
        /// are not the same size.
        long CountDifferingTexels(const Seen &seen, const Seen &reference)
        {
            if (!seen.ready || !reference.ready || seen.texels != reference.texels)
            {
                return -1;
            }
            const std::vector<float> a = ReadBuffer(seen);
            const std::vector<float> b = ReadBuffer(reference);
            long differ = 0;
            for (size_t i = 0; i < a.size(); i += 4)
            {
                differ += a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3];
            }
            return differ;
        }
    }

    int SharedBufferCheck(int argc, char **argv)
    {
        std::string imagesDir;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--images") == 0 && i + 1 < argc)
            {
                imagesDir = argv[++i];
            }
        }

        const GLSession gl;
        // One effect, reused across every scene as `check` reuses its one,
        // with a probe after the neon and one moved before it.
        EdgeLightingEffect effect;
        CreateEffect(effect);
        const std::shared_ptr<SharedBufferProbe> probe = std::make_shared<SharedBufferProbe>();
        effect.AddRenderer(probe);
        const std::shared_ptr<SharedBufferProbe> early = std::make_shared<SharedBufferProbe>();
        effect.AddRenderer(early);
        {
            std::vector<std::shared_ptr<BaseRenderer>> order = effect.GetRenderers();
            std::rotate(order.begin(), order.end() - 1, order.end()); // the last one first
            effect.SetRendererOrder(order);
        }
        // Frames whose reader before the neon saw another image than the one
        // after it: the two-phase frame says there are none.
        int orderMisses = 0;

        std::printf("%s\n", RendererName().c_str());
        std::printf("%-14s %-6s", "scene", "frame");
        for (int s = 0; s < SCALE_COUNT; ++s)
        {
            std::printf(" %8s", SCALE_TAGS[s]);
        }
        std::printf("\n(each: the buffer's light on the line against the frame's, '-' where a cut, cutoff or fill "
                    "is set; f = the field's composite filled it)\n");

        int failures = 0;
        // Cells whose buffer the field's composite filled (pass 1c): the hue
        // variant exists to cover them, so it has to reach some.
        int fieldCells = 0;
        for (const Variant &variant : VARIANTS)
        {
            for (const Scene &scene : SCENES)
            {
                std::printf("%-14s %-6s", scene.id, variant.id);
                for (int s = 0; s < SCALE_COUNT; ++s)
                {
                    Config config = BaseConfig();
                    scene.apply(config);
                    config.neon.resolutionScale = SCALES[s];
                    config.neon.hueRotationRate = variant.hueRotationRate;

                    // The steady frame. Under the hue the clock moves across the
                    // config change, as a host's frame loop moves it: with the
                    // clock held the frame after the change reuses its pass 1b
                    // and the field never settles. Render then draws two more
                    // frames at the held time - the field baked, then
                    // composited - the second of them the one measured.
                    if (variant.hueRotationRate != 0.0f)
                    {
                        effect.SetConfig(config);
                        effect.Update(1.0f / 60.0f);
                        RenderOnce(effect, FRAME_WIDTH, FRAME_HEIGHT);
                        effect.Update(1.0f / 60.0f);
                    }
                    const RGB frame = Render(effect, config);

                    const Seen seen = probe->Last();
                    orderMisses += early->Last().ready != seen.ready || early->Last().version != seen.version;
                    if (!seen.ready || !DescribesFrame(seen, config))
                    {
                        std::printf(" %8s", "nobuf!");
                        ++failures;
                        continue;
                    }
                    const std::vector<float> texels = ReadBuffer(seen);

                    // A frame that changes nothing, at the held time. A still
                    // config reuses the offscreen buffers, so its version must
                    // hold. Under the hue it holds where pass 1b shaded and
                    // moves where the field's composite filled the buffer,
                    // which is redrawn every frame by design (I56) - so the
                    // version also says which path ran.
                    RenderOnce(effect, FRAME_WIDTH, FRAME_HEIGHT);
                    const bool moved = probe->Last().version != seen.version;
                    bool bad = false;
                    char held = ' ';
                    if (variant.hueRotationRate == 0.0f && moved)
                    {
                        held = 'v';
                        bad = true;
                    }
                    else if (variant.hueRotationRate != 0.0f && moved)
                    {
                        held = 'f';
                        ++fieldCells;
                    }

                    if (IsUnmasked(config))
                    {
                        const double line = LineEnergyRatio(BlitBack(seen, texels), frame, config);
                        bad = bad || line < SHARED_MIN_LINE_ENERGY;
                        std::printf(" %c l%5.3f%s", held, line, bad ? "!" : " ");
                    }
                    else
                    {
                        std::printf(" %c %6s%s", held, "-", bad ? "!" : " ");
                    }
                    failures += bad;

                    if (!imagesDir.empty())
                    {
                        const std::string path = imagesDir + "/" + scene.id + "_" + variant.id + "_" +
                                                 SCALE_TAGS[s] + ".png";
                        if (!WritePNG(path, BufferImage(seen, texels), seen.texels.x, seen.texels.y))
                        {
                            std::fprintf(stderr, "neon-scale-check: cannot write %s\n", path.c_str());
                            return 2;
                        }
                    }
                }
                std::printf("\n");
            }
        }
        std::printf("field composite (f) in %d of the hue variant's cells\n", fieldCells);
        failures += fieldCells == 0;
        std::printf("reader before the neon saw another image than the reader after it: %d frame(s)\n", orderMisses);
        failures += orderMisses != 0;

        // With the neon off there is no glow to share: the buffer has to be
        // not ready on that frame - it is released with the layer - and
        // published again once the neon draws.
        Config off = BaseConfig();
        off.neon.enable = false;
        Render(effect, off);
        const bool withdrawn = !probe->Last().ready;
        Render(effect, BaseConfig());
        const bool republished = probe->Last().ready;
        std::printf("neon disabled: buffer %s; enabled again: %s\n", withdrawn ? "not ready" : "STILL READY",
                    republished ? "ready" : "NOT READY");
        failures += !withdrawn || !republished;

        // The windowed clear (SharedBuffer::BeginWrite) across a rect that
        // moves and shrinks at one scale and viewport, so the buffer is NOT
        // reallocated and only the clear of the last write's window keeps the
        // old glow out of it. The scenes above never reach that case - every
        // cell changes the scale, which reallocates the buffer and clears it
        // whole - and no frame shows a stale texel, since the blit reads only
        // where this write drew: only a reader sees one. Measured against the
        // moved rect drawn on a fresh effect, texel for texel. Still frames
        // only: the hue's would compare two effects' clocks.
        //
        // On an effect of its own, not `effect`: that one ran the hue variant
        // and then went back to rate 0, and the hue's phase is the rate
        // INTEGRATED over the clock (PhaseAccumulator), so its hue stays where
        // the rotation stopped while a fresh effect's sits at phase 0 - every
        // lit texel differs for a reason that is not the clear. Two effects
        // whose rate never moved hold the same phase.
        std::printf("moving rect, texels that differ from a fresh effect:");
        EdgeLightingEffect movingEffect;
        CreateEffect(movingEffect);
        const std::shared_ptr<SharedBufferProbe> movingProbe = std::make_shared<SharedBufferProbe>();
        movingEffect.AddRenderer(movingProbe);
        int movingMisses = 0;
        for (int s = 0; s < SCALE_COUNT; ++s)
        {
            Config from = BaseConfig();
            from.neon.resolutionScale = SCALES[s];
            const Config to = MovedRect(from);
            Render(movingEffect, from);
            Render(movingEffect, to);
            const Seen moved = movingProbe->Last();

            EdgeLightingEffect fresh;
            CreateEffect(fresh);
            const std::shared_ptr<SharedBufferProbe> freshProbe = std::make_shared<SharedBufferProbe>();
            fresh.AddRenderer(freshProbe);
            Render(fresh, to);
            const long differ = CountDifferingTexels(moved, freshProbe->Last());
            std::printf(" %s %ld%s", SCALE_TAGS[s], differ, differ != 0 ? "!" : "");
            movingMisses += differ != 0;
        }
        std::printf("\n");
        failures += movingMisses;

        if (failures)
        {
            std::printf("FAIL: %d value(s) out of bounds (marked !; v = a held still frame moved the version). "
                        "See tools/neon-scale-check/README.md.\n",
                        failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
