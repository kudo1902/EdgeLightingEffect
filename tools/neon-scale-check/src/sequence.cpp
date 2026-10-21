#include "sequence.h"

#include "harness.h"
#include "scenes.h"

#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "gl/gl-header.h"
#include "renderer/base-renderer.h"
#include "renderer/neon-renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace
    {
        /// `normal` against `forced`, in 8-bit levels, on any channel of the
        /// frame or of the shared buffer: the held gather region reads within
        /// 1 level (I55), and each field's composite within 1 (I46, I52) - the
        /// two can meet on one pixel.
        const int NORMAL_TOLERANCE = 2;

        /// On a frame whose intensity moved: the fields keep their bake and
        /// composite it scaled by I / I0 (I56), up to 2 levels off on the
        /// default look and 6 at bloom 1.5 - the generator's ceiling.
        const int INTENSITY_TOLERANCE = 6;

        /// Failures listed per run before the rest are only counted.
        const int REPORT_LIMIT = 12;

        const float DT = 1.0f / 60.0f;

        /// The frames the sequences draw into: the page's, two smaller ones of
        /// other aspects, and an odd size, so a resize changes the reduced
        /// buffer's texel count by a fraction.
        const int VIEWPORTS[][2] = {{1280, 720}, {960, 540}, {800, 600}, {1023, 577}};
        const int VIEWPORT_COUNT = 4;

        /// Uniform floats from mt19937's raw 32 bits, as `partition` draws
        /// them: a seed names the same sequences on every platform.
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

            bool Chance(float p) { return Uniform(0.0f, 1.0f) < p; }

        private:
            std::mt19937 mEngine;
        };

        /// One frame of a sequence: the config, the frame it is drawn into,
        /// and what changed on it.
        typedef struct Step
        {
            Config config;
            glm::ivec2 viewport{0};
            std::string events;
            bool intensityMoved = false;
        } Step;

        /// A config animated by one field over a stretch of frames.
        typedef struct Anim
        {
            int frames = 0; ///< Left to run; 0 when idle.
            float rate = 0.0f;
            int index = 0;
            glm::vec2 velocity{0.0f};
        } Anim;

        /// The random config sequence. Every frame either holds (a stretch of
        /// 1-6 unchanged frames, where the renderer's skips take over), or
        /// steps the animations running and maybe starts one, plus at most a
        /// few discrete changes.
        class SequenceGenerator
        {
        public:
            explicit SequenceGenerator(uint32_t seed)
                : mRng(seed)
            {
                mConfig = BaseConfig();
                const int sceneCount = int(sizeof(SCENES) / sizeof(SCENES[0]));
                SCENES[mRng.Pick(uint32_t(sceneCount))].apply(mConfig);
                pickViewport();
                mConfig.neon.resolutionScale = SCALES[mRng.Pick(SCALE_COUNT)];
                mConfig.neon.hueRotationRate = mRng.Chance(0.5f) ? 0.5f : 0.0f;
                // Keep the scene's bloom inside what INTENSITY_TOLERANCE covers.
                mConfig.neon.bloomStrength = std::min(mConfig.neon.bloomStrength, 1.5f);
                randomRect();
                if (mRng.Chance(0.5f))
                {
                    randomArcs();
                }
                if (mRng.Chance(0.4f))
                {
                    addSegment();
                }
            }

            Step Next()
            {
                Step step;
                const float intensityBefore = mConfig.neon.intensity;
                if (mHold > 0)
                {
                    --mHold;
                    step.events = "hold";
                }
                else
                {
                    advance(step.events);
                    if (mRng.Chance(0.15f))
                    {
                        mHold = 1 + int(mRng.Pick(6));
                    }
                }
                step.config = mConfig;
                step.viewport = mViewport;
                step.intensityMoved = mConfig.neon.intensity != intensityBefore;
                return step;
            }

        private:
            void note(std::string &events, const char *what)
            {
                if (!events.empty())
                {
                    events += ' ';
                }
                events += what;
            }

            static float wrap(float x) { return x - std::floor(x); }

            void pickViewport()
            {
                const int *v = VIEWPORTS[mRng.Pick(VIEWPORT_COUNT)];
                mViewport = glm::ivec2(v[0], v[1]);
            }

            /// A rect of 10-85% of the frame on each axis, anywhere that keeps
            /// its centre in the frame - so its glow is often cut by the edge.
            void randomRect()
            {
                RectGeometry &g = mConfig.geometry;
                g.width = std::floor(mRng.Uniform(0.1f, 0.85f) * float(mViewport.x));
                g.height = std::floor(mRng.Uniform(0.1f, 0.85f) * float(mViewport.y));
                g.position.x = std::floor(mRng.Uniform(0.0f, float(mViewport.x)) - g.width * 0.5f);
                g.position.y = std::floor(mRng.Uniform(0.0f, float(mViewport.y)) - g.height * 0.5f);
                g.cornerRadius = mRng.Chance(0.2f) ? 0.0f : mRng.Uniform(0.0f, 0.5f) * std::min(g.width, g.height);
            }

            ColorStop randomStop(float position)
            {
                // Now and then a stop below full alpha: the ring is then not
                // opaque, which shuts the fields and the uniform-ring shortcut.
                const float alpha = mRng.Chance(0.15f) ? mRng.Uniform(0.3f, 1.0f) : 1.0f;
                return ColorStop{position, glm::vec4(mRng.Uniform(0.0f, 1.0f), mRng.Uniform(0.0f, 1.0f),
                                                     mRng.Uniform(0.0f, 1.0f), alpha)};
            }

            std::vector<ColorStop> randomStops()
            {
                std::vector<ColorStop> stops;
                const int count = 1 + int(mRng.Pick(4));
                for (int i = 0; i < count; ++i)
                {
                    stops.push_back(randomStop(float(i) / float(count)));
                }
                return stops;
            }

            void randomArcs()
            {
                mConfig.neon.arcs.clear();
                const int count = 1 + int(mRng.Pick(4));
                for (int i = 0; i < count; ++i)
                {
                    Arc arc;
                    arc.start = mRng.Uniform(0.0f, 1.0f);
                    arc.length = mRng.Uniform(0.05f, 0.6f);
                    arc.intensity = mRng.Uniform(0.4f, 1.6f);
                    if (mRng.Chance(0.3f))
                    {
                        arc.colorStops = randomStops();
                    }
                    mConfig.neon.arcs.push_back(arc);
                }
            }

            void addSegment()
            {
                if (mConfig.neon.segmentBoosts.size() >= 4)
                {
                    return;
                }
                SegmentBoost s;
                s.position = mRng.Uniform(0.0f, 1.0f);
                s.length = mRng.Uniform(0.02f, 0.3f);
                s.boost = mRng.Uniform(0.3f, 2.0f);
                if (mRng.Chance(0.3f))
                {
                    s.colorStops = randomStops();
                }
                mConfig.neon.segmentBoosts.push_back(s);
            }

            void startAnims(std::string &events)
            {
                NeonConfig &n = mConfig.neon;
                auto start = [&](Anim &a, float rateLo, float rateHi)
                {
                    a.frames = 3 + int(mRng.Pick(28));
                    a.rate = mRng.Uniform(rateLo, rateHi) * (mRng.Chance(0.5f) ? -1.0f : 1.0f);
                };
                if (mArc.frames == 0 && !n.arcs.empty() && mRng.Chance(0.1f))
                {
                    start(mArc, 0.002f, 0.03f);
                    mArc.index = int(mRng.Pick(uint32_t(n.arcs.size())));
                    note(events, "arc-anim");
                }
                if (mSegment.frames == 0 && !n.segmentBoosts.empty() && mRng.Chance(0.1f))
                {
                    start(mSegment, 0.002f, 0.03f);
                    mSegment.index = int(mRng.Pick(uint32_t(n.segmentBoosts.size())));
                    note(events, "segment-anim");
                }
                if (mIntensity.frames == 0 && mRng.Chance(0.08f))
                {
                    start(mIntensity, 0.005f, 0.08f);
                    note(events, "intensity-anim");
                }
                if (mMove.frames == 0 && mRng.Chance(0.06f))
                {
                    start(mMove, 0.0f, 0.0f);
                    mMove.velocity = glm::vec2(mRng.Uniform(-12.0f, 12.0f), mRng.Uniform(-12.0f, 12.0f));
                    // Whole and fractional px both: a fraction moves every
                    // edge off the pixel grid.
                    if (mRng.Chance(0.5f))
                    {
                        mMove.velocity = glm::floor(mMove.velocity);
                    }
                    note(events, "move-anim");
                }
                if (mSize.frames == 0 && mRng.Chance(0.06f))
                {
                    start(mSize, 0.5f, 10.0f);
                    note(events, "size-anim");
                }
                if (mBloom.frames == 0 && mRng.Chance(0.05f))
                {
                    start(mBloom, 0.01f, 0.08f);
                    note(events, "bloom-anim");
                }
                if (mGlow.frames == 0 && mRng.Chance(0.05f))
                {
                    start(mGlow, 0.1f, 2.0f);
                    note(events, "glow-anim");
                }
            }

            void stepAnims()
            {
                NeonConfig &n = mConfig.neon;
                RectGeometry &g = mConfig.geometry;
                if (mArc.frames > 0 && mArc.index < int(n.arcs.size()))
                {
                    Arc &a = n.arcs[size_t(mArc.index)];
                    if (mArc.index % 2 == 0)
                    {
                        a.length = std::min(std::max(a.length + mArc.rate, 0.0f), 1.0f);
                    }
                    else
                    {
                        a.start = wrap(a.start + mArc.rate);
                    }
                }
                if (mSegment.frames > 0 && mSegment.index < int(n.segmentBoosts.size()))
                {
                    SegmentBoost &s = n.segmentBoosts[size_t(mSegment.index)];
                    s.position = wrap(s.position + mSegment.rate);
                    if (mSegment.index % 2 == 1)
                    {
                        s.length = std::min(std::max(s.length + mSegment.rate * 0.5f, 0.01f), 0.4f);
                    }
                }
                if (mIntensity.frames > 0)
                {
                    n.intensity = std::min(std::max(n.intensity + mIntensity.rate, 0.2f), 3.0f);
                }
                if (mMove.frames > 0)
                {
                    g.position += mMove.velocity;
                }
                if (mSize.frames > 0)
                {
                    g.width = std::min(std::max(g.width + mSize.rate, 24.0f), float(mViewport.x));
                    g.height = std::min(std::max(g.height + mSize.rate * 0.6f, 16.0f), float(mViewport.y));
                }
                if (mBloom.frames > 0)
                {
                    n.bloomStrength = std::min(std::max(n.bloomStrength + mBloom.rate, 0.0f), 1.5f);
                }
                if (mGlow.frames > 0)
                {
                    n.glowRadius = std::min(std::max(n.glowRadius + mGlow.rate, 0.5f), 40.0f);
                }
                for (Anim *a : {&mArc, &mSegment, &mIntensity, &mMove, &mSize, &mBloom, &mGlow})
                {
                    a->frames = std::max(a->frames - 1, 0);
                }
            }

            /// The discrete changes: each at its own small chance.
            void discrete(std::string &events)
            {
                NeonConfig &n = mConfig.neon;
                RectGeometry &g = mConfig.geometry;
                if (mRng.Chance(0.05f))
                {
                    n.hueRotationRate = n.hueRotationRate != 0.0f ? 0.0f : (mRng.Chance(0.5f) ? 0.5f : -0.3f);
                    note(events, n.hueRotationRate != 0.0f ? "hue-on" : "hue-off");
                }
                if (mRng.Chance(0.04f))
                {
                    n.resolutionScale = SCALES[mRng.Pick(SCALE_COUNT)];
                    note(events, "scale");
                }
                if (mRng.Chance(0.03f))
                {
                    pickViewport();
                    note(events, "viewport");
                }
                if (mRng.Chance(0.025f))
                {
                    n.enable = !n.enable;
                    note(events, n.enable ? "enable" : "disable");
                }
                if (!n.enable && mRng.Chance(0.3f))
                {
                    n.enable = true;
                    note(events, "enable");
                }
                if (mRng.Chance(0.02f) || (mConfig.debug.opaqueOnly && mRng.Chance(0.3f)))
                {
                    mConfig.debug.opaqueOnly = !mConfig.debug.opaqueOnly;
                    note(events, "opaque-only");
                }
                if (mRng.Chance(0.03f))
                {
                    n.glowSide = GlowSide(mRng.Pick(3));
                    note(events, "glow-side");
                }
                if (mRng.Chance(0.03f))
                {
                    n.insideCutoff = Cutoff{mRng.Chance(0.6f), mRng.Uniform(2.0f, 60.0f), mRng.Uniform(0.0f, 16.0f)};
                    note(events, "inside-cutoff");
                }
                if (mRng.Chance(0.03f))
                {
                    n.outsideCutoff = Cutoff{mRng.Chance(0.6f), mRng.Uniform(2.0f, 60.0f), mRng.Uniform(0.0f, 16.0f)};
                    note(events, "outside-cutoff");
                }
                if (mRng.Chance(0.02f))
                {
                    n.opaqueMode = OpaqueMode(mRng.Pick(5));
                    n.opaqueInsideCutoff = Cutoff{mRng.Chance(0.5f), mRng.Uniform(0.0f, 40.0f), mRng.Uniform(0.0f, 8.0f)};
                    n.opaqueOutsideCutoff = Cutoff{mRng.Chance(0.5f), mRng.Uniform(0.0f, 40.0f), mRng.Uniform(0.0f, 8.0f)};
                    note(events, "opaque-mode");
                }
                if (mRng.Chance(0.03f))
                {
                    n.colorStops = randomStops();
                    note(events, "color-stops");
                }
                if (mRng.Chance(0.015f))
                {
                    n.colorTransitionDuration = n.colorTransitionDuration > 0.0f ? 0.0f : 0.25f;
                    note(events, "transition");
                }
                if (mRng.Chance(0.015f))
                {
                    n.blendSpace = BlendSpace(mRng.Pick(3));
                    note(events, "blend-space");
                }
                if (mRng.Chance(0.03f))
                {
                    const uint32_t kind = mRng.Pick(4);
                    if (kind == 0)
                    {
                        randomArcs();
                    }
                    else if (kind == 1 && !n.arcs.empty())
                    {
                        n.arcs.erase(n.arcs.begin() + long(mRng.Pick(uint32_t(n.arcs.size()))));
                    }
                    else if (kind == 2)
                    {
                        n.arcs = {Arc{}}; // the default: one arc, the whole ring
                    }
                    else if (!n.arcs.empty())
                    {
                        n.arcs[mRng.Pick(uint32_t(n.arcs.size()))].intensity = mRng.Uniform(0.3f, 2.0f);
                    }
                    note(events, "arcs");
                }
                if (mRng.Chance(0.035f))
                {
                    if (n.segmentBoosts.empty() || mRng.Chance(0.6f))
                    {
                        addSegment();
                    }
                    else
                    {
                        n.segmentBoosts.erase(n.segmentBoosts.begin() +
                                              long(mRng.Pick(uint32_t(n.segmentBoosts.size()))));
                    }
                    note(events, "segments");
                }
                if (mRng.Chance(0.015f))
                {
                    if (n.preservedSegmentBoosts.empty() || mRng.Chance(0.5f))
                    {
                        PreservedSegment p;
                        p.id = ++mNextPreservedId;
                        p.segment.position = mRng.Uniform(0.0f, 1.0f);
                        p.segment.length = mRng.Uniform(0.02f, 0.2f);
                        p.segment.boost = mRng.Uniform(0.3f, 1.5f);
                        n.preservedSegmentBoosts.push_back(p);
                    }
                    else
                    {
                        n.preservedSegmentBoosts.pop_back();
                    }
                    note(events, "preserved");
                }
                // One field per change from here on, as far as it goes: a gate
                // that misses one field is hidden on any frame another field
                // it does read moved with it.
                if (mRng.Chance(0.02f))
                {
                    n.lineWidth = mRng.Uniform(0.75f, 10.0f);
                    note(events, "line-width");
                }
                if (mRng.Chance(0.02f))
                {
                    n.filamentFalloff = mRng.Uniform(0.5f, 3.0f);
                    note(events, "filament-falloff");
                }
                if (mRng.Chance(0.015f))
                {
                    n.glowSideSoftness = mRng.Chance(0.3f) ? 0.0f : mRng.Uniform(0.5f, 8.0f);
                    note(events, "side-softness");
                }
                if (mRng.Chance(0.015f))
                {
                    Cutoff &cut = mRng.Chance(0.5f) ? n.insideCutoff : n.outsideCutoff;
                    if (mRng.Chance(0.5f))
                    {
                        cut.size = mRng.Uniform(2.0f, 60.0f);
                    }
                    else
                    {
                        cut.softness = mRng.Uniform(0.0f, 16.0f);
                    }
                    note(events, "cutoff-field");
                }
                if (mRng.Chance(0.015f))
                {
                    Cutoff &cut = mRng.Chance(0.5f) ? n.opaqueInsideCutoff : n.opaqueOutsideCutoff;
                    const uint32_t which = mRng.Pick(3);
                    if (which == 0)
                    {
                        cut.enable = !cut.enable;
                    }
                    else if (which == 1)
                    {
                        cut.size = mRng.Uniform(0.0f, 40.0f);
                    }
                    else
                    {
                        cut.softness = mRng.Uniform(0.0f, 8.0f);
                    }
                    note(events, "opaque-cutoff-field");
                }
                if (mRng.Chance(0.02f) && !n.segmentBoosts.empty())
                {
                    SegmentBoost &seg = n.segmentBoosts[mRng.Pick(uint32_t(n.segmentBoosts.size()))];
                    const uint32_t which = mRng.Pick(4);
                    if (which == 0)
                    {
                        seg.boost = mRng.Uniform(0.3f, 2.0f);
                    }
                    else if (which == 1)
                    {
                        seg.length = mRng.Uniform(0.02f, 0.3f);
                    }
                    else if (which == 2)
                    {
                        seg.colorStops = mRng.Chance(0.3f) ? std::vector<ColorStop>() : randomStops();
                    }
                    else
                    {
                        seg.blendSpace = BlendSpace(mRng.Pick(3));
                    }
                    note(events, "segment-field");
                }
                if (mRng.Chance(0.02f) && !n.arcs.empty())
                {
                    Arc &arc = n.arcs[mRng.Pick(uint32_t(n.arcs.size()))];
                    const uint32_t which = mRng.Pick(4);
                    if (which == 0)
                    {
                        arc.start = mRng.Uniform(0.0f, 1.0f);
                    }
                    else if (which == 1)
                    {
                        arc.length = mRng.Chance(0.2f) ? 1.0f : mRng.Uniform(0.0f, 1.0f);
                    }
                    else if (which == 2)
                    {
                        arc.colorStops = mRng.Chance(0.3f) ? std::vector<ColorStop>() : randomStops();
                    }
                    else
                    {
                        arc.blendSpace = BlendSpace(mRng.Pick(3));
                    }
                    note(events, "arc-field");
                }
                if (mRng.Chance(0.015f))
                {
                    if (mRng.Chance(0.5f))
                    {
                        g.width = std::floor(mRng.Uniform(0.1f, 0.85f) * float(mViewport.x));
                    }
                    else
                    {
                        g.height = std::floor(mRng.Uniform(0.1f, 0.85f) * float(mViewport.y));
                    }
                    note(events, "rect-size");
                }
                if (mRng.Chance(0.015f))
                {
                    // A jump, often to a frame edge, where the viewport clips
                    // the buffers' regions.
                    g.position.x = mRng.Chance(0.5f) ? mRng.Uniform(-0.5f, 0.2f) * g.width
                                                     : float(mViewport.x) - mRng.Uniform(0.2f, 1.2f) * g.width;
                    note(events, "rect-jump");
                }
                if (mRng.Chance(0.01f))
                {
                    n.intensity = mRng.Uniform(0.2f, 3.0f);
                    note(events, "intensity");
                }
                if (mRng.Chance(0.01f))
                {
                    n.bloomStrength = mRng.Uniform(0.0f, 1.5f);
                    note(events, "bloom");
                }
                if (mRng.Chance(0.01f))
                {
                    n.glowRadius = mRng.Uniform(0.5f, 40.0f);
                    note(events, "glow-radius");
                }
                if (mRng.Chance(0.02f))
                {
                    g.cornerRadius = mRng.Chance(0.2f) ? 0.0f : mRng.Uniform(0.0f, 0.5f) * std::min(g.width, g.height);
                    note(events, "corner");
                }
                if (mRng.Chance(0.01f))
                {
                    g.winding = g.winding == Winding::CLOCKWISE ? Winding::COUNTER_CLOCKWISE : Winding::CLOCKWISE;
                    note(events, "winding");
                }
                if (mRng.Chance(0.015f))
                {
                    static const int SAMPLES[] = {NEON_MAX_LOOP_SAMPLES, 96, 64, 32};
                    n.numSamples = SAMPLES[mRng.Pick(4)];
                    note(events, "samples");
                }
                if (mRng.Chance(0.01f))
                {
                    n.gradientLutSize = mRng.Chance(0.5f) ? 256 : 64;
                    note(events, "lut-size");
                }
                if (mRng.Chance(0.015f))
                {
                    randomRect();
                    note(events, "rect");
                }
            }

            void advance(std::string &events)
            {
                startAnims(events);
                stepAnims();
                discrete(events);
                if (events.empty())
                {
                    note(events, "-");
                }
            }

        private:
            Random mRng;
            Config mConfig;
            glm::ivec2 mViewport{0};
            int mHold = 0;
            uint32_t mNextPreservedId = 0;
            Anim mArc, mSegment, mIntensity, mMove, mSize, mBloom, mGlow;
        };

        /// The neon's shared buffer as a reader's Render is handed it - as
        /// `shared-buffer`'s probe takes it - read back as floats.
        class BufferProbe : public BaseRenderer
        {
        public:
            bool Initialize() override { return true; }
            void Update(float, double, const Config &) override {}
            void Render(int, int, double, const Config &) override
            {
                const std::shared_ptr<const SharedBuffer> buffer = GetSharedBufferRegistry()->Find(SharedBufferId::NEON);
                mReady = buffer && buffer->IsReady();
                mTexels = mReady ? buffer->GetTexels() : glm::ivec2(0);
                mData.clear();
                if (!mReady)
                {
                    return;
                }
                mData.assign(size_t(mTexels.x) * size_t(mTexels.y) * 4, 0.0f);
                GLint previous = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer->GetTextureId(), 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                {
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, mTexels.x, mTexels.y, GL_RGBA, GL_FLOAT, mData.data());
                }
                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
                glDeleteFramebuffers(1, &fbo);
            }
            void OnConfigChanged(const Config &) override {}
            RendererLayer GetLayer() const override { return RendererLayer::CUSTOM; }

            /// Whether the last Render was handed a ready buffer.
            bool IsReady() const { return mReady; }
            /// Its size, texels; 0 when not ready.
            glm::ivec2 GetTexels() const { return mTexels; }
            /// Its texels as RGBA floats, bottom row first; empty when not ready.
            const std::vector<float> &GetData() const { return mData; }

        private:
            bool mReady = false;
            glm::ivec2 mTexels{0};
            std::vector<float> mData;
        };

        /// The buffer as 8-bit RGBA, top row first - for --dump.
        RGBA BufferPixels(const BufferProbe &probe)
        {
            RGBA out(probe.GetData().size());
            const int w = probe.GetTexels().x;
            const int h = probe.GetTexels().y;
            for (int row = 0; row < h; ++row)
            {
                for (int x = 0; x < w; ++x)
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        const float c = probe.GetData()[(size_t(h - 1 - row) * w + x) * 4 + k];
                        out[(size_t(row) * w + x) * 4 + k] =
                            static_cast<unsigned char>(std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f));
                    }
                }
            }
            return out;
        }

        /// How a run is held to `forced`.
        typedef enum class Bound
        {
            EXACT,  ///< Byte for byte, the frame and every buffer texel.
            NORMAL, ///< NORMAL_TOLERANCE, INTENSITY_TOLERANCE where the intensity moved.
        } Bound;

        /// One effect of the comparison.
        typedef struct Run
        {
            std::string name;
            uint32_t bypass = 0;
            Bound bound = Bound::EXACT;
            std::unique_ptr<EdgeLightingEffect> effect;
            std::shared_ptr<BufferProbe> probe;
            RGBA frame;
            // Over the whole check.
            int worstFrame = 0;
            int worstBuffer = 0;
            int worstStillFrame = 0; ///< worstFrame over the frames whose intensity held.
            int failures = 0;
        } Run;

        void CreateRun(Run &run)
        {
            run.effect.reset(new EdgeLightingEffect());
            CreateEffect(*run.effect);
            for (const std::shared_ptr<BaseRenderer> &r : run.effect->GetRenderers())
            {
                if (const std::shared_ptr<NeonRenderer> neon = std::dynamic_pointer_cast<NeonRenderer>(r))
                {
                    neon->SetBypass(run.bypass);
                }
            }
            run.probe = std::make_shared<BufferProbe>();
            run.effect->AddRenderer(run.probe);
        }

        /// Largest per-channel difference, and how many pixels differ at all.
        int FrameDifference(const RGBA &a, const RGBA &b, long &differing)
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

        /// The shared buffers against each other, in 8-bit levels rounded
        /// up, so any texel that differs at all reads at least 1. -1 when one
        /// is ready and the other not, or their sizes differ.
        int BufferDifference(const BufferProbe &a, const BufferProbe &b, long &differing)
        {
            differing = 0;
            if (a.IsReady() != b.IsReady() || a.GetTexels() != b.GetTexels())
            {
                return -1;
            }
            int worst = 0;
            for (size_t i = 0; i < a.GetData().size(); i += 4)
            {
                float d = 0.0f;
                for (size_t k = 0; k < 4; ++k)
                {
                    d = std::max(d, std::fabs(a.GetData()[i + k] - b.GetData()[i + k]));
                }
                worst = std::max(worst, int(std::ceil(d * 255.0f)));
                differing += d != 0.0f;
            }
            return worst;
        }

        std::string BypassName(uint32_t bit)
        {
            switch (NeonBypass(bit))
            {
                case NeonBypass::EMISSION:
                {
                    return "emission";
                }
                case NeonBypass::GLOW_COVER:
                {
                    return "glow-cover";
                }
                case NeonBypass::OFFSCREEN:
                {
                    return "offscreen";
                }
                case NeonBypass::CLEAR:
                {
                    return "clear";
                }
                case NeonBypass::REBUILD:
                {
                    return "rebuild";
                }
                case NeonBypass::GATHER:
                {
                    return "gather";
                }
                case NeonBypass::GLOW_FIELD:
                {
                    return "glow-field";
                }
                case NeonBypass::RING_FIELD:
                {
                    return "ring-field";
                }
                default:
                {
                    return "?";
                }
            }
        }

        /// The config's state in one line, for a failure report.
        std::string Describe(const Step &s)
        {
            const NeonConfig &n = s.config.neon;
            const RectGeometry &g = s.config.geometry;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%dx%d rect %.1fx%.1f@(%.1f,%.1f) r%.1f scale %.3f hue %.2f int %.3f bloom %.3f glow %.2f "
                          "line %.2f side %d cut %d/%d opaque %d arcs %zu segs %zu+%zu%s%s",
                          s.viewport.x, s.viewport.y, g.width, g.height, g.position.x, g.position.y, g.cornerRadius,
                          n.resolutionScale, n.hueRotationRate, n.intensity, n.bloomStrength, n.glowRadius,
                          n.lineWidth, int(n.glowSide), int(n.insideCutoff.enable), int(n.outsideCutoff.enable),
                          int(n.opaqueMode), n.arcs.size(), n.segmentBoosts.size(), n.preservedSegmentBoosts.size(),
                          n.enable ? "" : " DISABLED", s.config.debug.opaqueOnly ? " opaqueOnly" : "");
            return line;
        }
    }

    int SequenceCheck(int argc, char **argv)
    {
        int sequences = 16;
        int frames = 90;
        uint32_t seed = 1;
        int only = -1;
        bool isolate = false;
        std::string dumpDir;
        for (int i = 2; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--sequences") == 0 && i + 1 < argc)
            {
                sequences = std::max(1, std::atoi(argv[++i]));
            }
            else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            {
                frames = std::max(1, std::atoi(argv[++i]));
            }
            else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            {
                seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
            }
            else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            {
                only = std::atoi(argv[++i]);
            }
            else if (std::strcmp(argv[i], "--isolate") == 0)
            {
                isolate = true;
            }
            else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            {
                dumpDir = argv[++i];
            }
        }

        const GLSession gl;
        std::printf("%s\n", RendererName().c_str());

        // forced first: every other run is compared against it.
        std::vector<Run> runs;
        auto addRun = [&](const std::string &name, uint32_t bypass, Bound bound)
        {
            Run run;
            run.name = name;
            run.bypass = bypass;
            run.bound = bound;
            runs.push_back(std::move(run));
        };
        addRun("forced", uint32_t(NeonBypass::ALL), Bound::EXACT);
        addRun("exact", uint32_t(NeonBypass::APPROXIMATE), Bound::EXACT);
        addRun("normal", uint32_t(NeonBypass::NONE), Bound::NORMAL);
        // The isolating runs: `normal` and `exact` each with ONE more skip
        // bypassed. A failure of either main run that one of these does not
        // share names its skip. Not "only that skip active": the skips depend
        // on each other - the gather's key holds the emission table's
        // version, and the offscreen reuse needs that table current - so a
        // skip alone among bypassed ones is never reached.
        const size_t firstIsolating = 3;
        if (isolate)
        {
            for (uint32_t bit = 1; bit <= uint32_t(NeonBypass::ALL); bit <<= 1)
            {
                addRun("normal-no-" + BypassName(bit), bit, Bound::NORMAL);
            }
            for (uint32_t bit = 1; bit <= uint32_t(NeonBypass::EXACT); bit <<= 1)
            {
                addRun("exact-no-" + BypassName(bit), uint32_t(NeonBypass::APPROXIMATE) | bit, Bound::EXACT);
            }
        }

        std::printf("%d sequence(s) of %d frames, seed %u; runs:", only >= 0 ? 1 : sequences, frames, seed);
        for (const Run &run : runs)
        {
            std::printf(" %s (bypass 0x%02x)", run.name.c_str(), run.bypass);
        }
        std::printf("\n");

        long framesDrawn = 0;
        long intensityFrames = 0;
        for (int k = 0; k < sequences; ++k)
        {
            if (only >= 0 && k != only)
            {
                continue;
            }
            // Fresh effects per sequence: each starts from a renderer that has
            // drawn nothing, as a host's does.
            for (Run &run : runs)
            {
                CreateRun(run);
            }
            SequenceGenerator generator(seed + uint32_t(k));
            for (int f = 0; f < frames; ++f)
            {
                const Step step = generator.Next();
                ++framesDrawn;
                intensityFrames += step.intensityMoved;
                for (Run &run : runs)
                {
                    run.effect->SetConfig(step.config);
                    run.effect->Update(DT);
                    run.frame = RenderOnce(*run.effect, step.viewport.x, step.viewport.y);
                }

                const Run &reference = runs[0];
                // Which runs are out of bounds on this frame.
                std::vector<bool> failed(runs.size(), false);
                for (size_t r = 1; r < runs.size(); ++r)
                {
                    Run &run = runs[r];
                    long framePixels = 0;
                    long bufferTexels = 0;
                    const int frameDiff = FrameDifference(run.frame, reference.frame, framePixels);
                    const int bufferDiff = BufferDifference(*run.probe, *reference.probe, bufferTexels);
                    run.worstFrame = std::max(run.worstFrame, frameDiff);
                    run.worstBuffer = std::max(run.worstBuffer, bufferDiff);
                    if (!step.intensityMoved)
                    {
                        run.worstStillFrame = std::max(run.worstStillFrame, frameDiff);
                    }
                    int tolerance = 0;
                    if (run.bound == Bound::NORMAL)
                    {
                        tolerance = step.intensityMoved ? INTENSITY_TOLERANCE : NORMAL_TOLERANCE;
                    }
                    const bool bad = frameDiff > tolerance || bufferDiff > tolerance || bufferDiff < 0;
                    if (!bad)
                    {
                        continue;
                    }
                    failed[r] = true;
                    ++run.failures;
                    if (run.failures <= REPORT_LIMIT)
                    {
                        std::printf("  %-18s seq %d frame %3d: frame max %d (%ld px), buffer %s%d (%ld texels), "
                                    "bound %d\n      events: %s\n      config: %s\n",
                                    run.name.c_str(), k, f, frameDiff, framePixels,
                                    bufferDiff < 0 ? "READY/SIZE MISMATCH " : "", bufferDiff, bufferTexels,
                                    tolerance, step.events.c_str(), Describe(step).c_str());
                    }
                    if (!dumpDir.empty() && run.failures <= REPORT_LIMIT)
                    {
                        char name[256];
                        std::snprintf(name, sizeof(name), "%s/seq%02d_f%03d_", dumpDir.c_str(), k, f);
                        WritePNG4(std::string(name) + run.name + ".png", run.frame, step.viewport.x,
                                  step.viewport.y);
                        WritePNG4(std::string(name) + "forced.png", reference.frame, step.viewport.x,
                                  step.viewport.y);
                        // And both shared buffers, over transparent black.
                        if (run.probe->IsReady())
                        {
                            WritePNG4(std::string(name) + run.name + "_buffer.png", BufferPixels(*run.probe),
                                      run.probe->GetTexels().x, run.probe->GetTexels().y);
                        }
                        if (reference.probe->IsReady())
                        {
                            WritePNG4(std::string(name) + "forced_buffer.png", BufferPixels(*reference.probe),
                                      reference.probe->GetTexels().x, reference.probe->GetTexels().y);
                        }
                    }
                }
                // Attribution: the isolating runs of a failed main run's class
                // that this frame passes - bypassing that skip cleared it.
                if (isolate && ((failed[1] && runs[1].failures <= REPORT_LIMIT) ||
                                (failed[2] && runs[2].failures <= REPORT_LIMIT)))
                {
                    std::string cleared;
                    for (size_t r = firstIsolating; r < runs.size(); ++r)
                    {
                        const bool exactClass = runs[r].bound == Bound::EXACT;
                        if (!failed[r] && ((exactClass && failed[1]) || (!exactClass && failed[2])))
                        {
                            cleared += " " + runs[r].name;
                        }
                    }
                    std::printf("      cleared by:%s\n", cleared.empty() ? " (none - more than one skip)" : cleared.c_str());
                }
            }
            std::printf("sequence %2d done\n", k);
            std::fflush(stdout);
        }

        std::printf("\n%ld frames, %ld with the intensity moving\n", framesDrawn, intensityFrames);
        std::printf("%-20s %8s %10s %10s %8s %s\n", "run", "bound", "frame max", "(int held)", "buffer", "failures");
        int failures = 0;
        for (size_t r = 1; r < runs.size(); ++r)
        {
            const Run &run = runs[r];
            std::printf("%-20s %8s %10d %10d %8d %d%s\n", run.name.c_str(),
                        run.bound == Bound::EXACT ? "exact" : "2 / 6", run.worstFrame, run.worstStillFrame,
                        run.worstBuffer, run.failures, run.failures ? "!" : "");
            // The isolated runs say which skip a failure comes from; only the
            // two main runs decide the exit code.
            if (r < firstIsolating)
            {
                failures += run.failures;
            }
        }
        if (failures)
        {
            std::printf("FAIL: %d frame(s) out of bounds. Replay one with --only K (add --isolate to name the skip). "
                        "See tools/neon-scale-check/README.md.\n",
                        failures);
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }
}
