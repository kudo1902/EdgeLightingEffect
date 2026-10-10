#ifndef _NEON_SCALE_CHECK_HARNESS_H_
#define _NEON_SCALE_CHECK_HARNESS_H_

// Rendering and measurement for neon-scale-check. Everything here is fixed by
// docs/neon-resolution-scale-comparison.html: the frame, the clear colour and
// every metric are the ones that page reports, and the metric definitions
// reproduce its first version's stored numbers exactly when run on its stored
// images. Change one and the page's numbers stop being comparable across runs.

#include "core/edge-lighting.h"
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    using namespace EdgeLighting;

    static const int FRAME_WIDTH = 1280;
    static const int FRAME_HEIGHT = 720;

    /// Tightly packed RGB, top row first, FRAME_WIDTH x FRAME_HEIGHT unless
    /// stated otherwise.
    typedef std::vector<unsigned char> RGB;
    /// Tightly packed RGBA, top row first: a frame with the layer's own
    /// coverage alpha, rendered over transparent black.
    typedef std::vector<unsigned char> RGBA;

    /// Errors against a scene's own 1.0 render, as the page reports them.
    typedef struct Metrics
    {
        int maxDelta;        ///< Largest per-pixel error over the whole frame.
        int p99;             ///< 99th-percentile error over lit pixels.
        double meanLit;      ///< Mean error over lit pixels.
        double psnr;         ///< Over every channel of the whole frame, dB.
        double pctOver8;     ///< Share of lit pixels with an error above 8, percent.
        double energyRatio;  ///< Total Rec. 709 luma above the clear, against 1.0's.
        long litPx;          ///< Pixels whose brightest channel is above 24 in either image.
    } Metrics;

    /// One step of the motion sweep, on the top edge's centre column.
    typedef struct SweepStep
    {
        int peak;            ///< Brightest luma in the window, rounded.
        double centroidErr;  ///< Centroid of the luma above the window's floor, minus the true edge, px.
        double fwhm;         ///< Width at half the height above the floor, px.
        long long energy;    ///< Rec. 709 luma summed over the whole frame.
    } SweepStep;

    /// Hidden GLFW window with a GL 3.3 core context, made current. Exits the
    /// process on failure: nothing in this tool can run without it.
    void InitGL();
    void ShutdownGL();

    /// InitGL for the life of a scope, ShutdownGL when it ends. Declare it
    /// BEFORE anything in that scope that owns GL objects: locals are destroyed
    /// in reverse order, so the context then outlives every destructor that
    /// deletes into it, on every return path.
    ///
    /// The commands used to call ShutdownGL by hand while their
    /// EdgeLightingEffect was still in scope, so the effect's destructor ran
    /// its glDelete* calls into a terminated context. macOS let that pass;
    /// Mesa segfaulted on it, which threw away the buffered report and turned
    /// `check`'s exit code into 139 whatever it had found.
    class GLSession
    {
    public:
        GLSession() { InitGL(); }
        ~GLSession() { ShutdownGL(); }
        GLSession(const GLSession &) = delete;
        GLSession &operator=(const GLSession &) = delete;
    };

    /// GL_RENDERER of the current context - the GPU every timing belongs to.
    std::string RendererName();

    /// A neon-only effect, initialised. One per measurement where the method
    /// asks for a freshly initialised effect (timing); otherwise reused.
    void CreateEffect(EdgeLightingEffect &effect);

    /// Set @p config, tick the clock once by 0, and render the steady frame -
    /// the second, each over a fresh clear - into an offscreen FRAME_WIDTH x
    /// FRAME_HEIGHT target over rgb(5, 5, 8). The first frame after a config
    /// change can take a different path (the neon's field, I46).
    RGB Render(EdgeLightingEffect &effect, const Config &config);

    /// What changes between the frames @ref TimeRender times.
    ///
    /// STILL is what every figure before these modes measured: one config,
    /// Render() after Render() with nothing in between - so the work a frame
    /// does only when something MOVED (the emission table's re-bake, the glow
    /// coverage table's, anything a renderer skips on an unchanged frame) is
    /// never in it. The other modes put that work back, the way a host's frame
    /// loop does: each frame writes the mode's change through SetConfig and
    /// then calls Update(1/60) - both inside the timed region, so the figure
    /// includes the library's CPU path too. See docs/neon-perf-plan.md.
    typedef enum class TimeMode
    {
        STILL,          ///< Nothing changes; Render() only.
        HUE,            ///< hueRotationRate 0.5 and the clock advancing - the library's default.
        INTENSITY,      ///< intensity pulsing by +/-10% - moves the glow's reach, not its colours.
        ARC_WIPE,       ///< arcs[0].length sweeping 0.3-0.9 - re-bakes the glow coverage table.
        SEGMENT_TRAVEL, ///< segmentBoosts[0] (one is added if the scene has none) moving round the ring.
        LIGHTS,         ///< LightCounts' arcs and segments, every one changing length every frame, the hue rotating.
        RESIZE,         ///< The rect's width and height swinging +/-15% out of phase - re-bakes and resizes everything the rect sizes.
    } TimeMode;

    /// The lights TimeMode::LIGHTS draws, in place of the scene's own: the
    /// frames of a host animating several arcs or segments at once
    /// (docs/neon-animation-perf-analysis.md), which arc-wipe and
    /// segment-travel - one light each - do not cover.
    ///
    /// @c arcs arcs evenly spaced over 0.6 of the ring, each length moving
    /// between 0.3 and 0.9 of its share on its own phase - or, with 0, one
    /// arc over the whole ring, still. @c segments segments of length 0.05 to
    /// 0.15, boost 1, evenly spaced, likewise each on its own phase; none with
    /// 0. The hue rotates at 0.5, as the library's default does.
    typedef struct LightCounts
    {
        int arcs = 8;
        int segments = 0;
    } LightCounts;

    /// The mode named @p name (still, hue, intensity, arc-wipe,
    /// segment-travel, lights, resize). False if there is none.
    bool ParseTimeMode(const char *name, TimeMode &mode);
    /// @p mode's name, as ParseTimeMode takes it.
    const char *TimeModeName(TimeMode mode);

    /// Minimum over 5 runs of the mean frame time of 40 frames between
    /// glFinish, after 5 warm-up frames, on a freshly initialised effect, into
    /// a @p width x @p height target. ms. A frame is one Render() under
    /// TimeMode::STILL, and the mode's SetConfig + Update(1/60) + Render()
    /// under any other. @p initMs, when non-null, receives how long
    /// constructing and initialising that effect took - the shader compiles,
    /// which a host pays once per effect.
    /// @p firstMs, when non-null, receives the first frame's own time,
    /// glFinish to glFinish - where a driver that finishes a program's
    /// compile on its first draw (both of this project's GPUs do) pays for
    /// every program the frame builds.
    double TimeRender(const Config &config, int width = FRAME_WIDTH, int height = FRAME_HEIGHT,
                      double *initMs = nullptr, TimeMode mode = TimeMode::STILL,
                      const LightCounts &lights = LightCounts(), double *firstMs = nullptr);

    /// What one config's frames cost on the GPU.
    ///
    /// READ IT AS A RATIO. On an AMD Radeon Pro 5300M under macOS GL these
    /// queries read 3.6-4.0x the wall-clock time of the same frames (measured
    /// 2026-10-09 over twelve light configs: 120 frames that the timer put at
    /// 4.0 ms each finished in 125 ms, glFinish to glFinish), steadily enough
    /// that ratios between configs and passes hold while the milliseconds do
    /// not. TimeRender's wall-clock figure is the frame's real cost.
    typedef struct GpuTiming
    {
        double frameMs = 0.0; ///< Median over 120 frames of a GL_TIME_ELAPSED query round Render.
        /// Per pass, GPU ms per frame summed over its draws, over 60 more
        /// frames with a query round each draw (PassRecorder): its name
        /// (PassKindName) and its time. Empty unless asked for. Each draw's
        /// query serialises it, so these add up to more than frameMs.
        std::vector<std::pair<std::string, double>> passes;
    } GpuTiming;

    /// The GPU's side of @ref TimeRender: on a freshly initialised effect, 20
    /// warm-up frames of @p mode, then a timer query round each Render of
    /// 120 more, each frame flushed as a host's swap would. With
    /// @p perPass, 60 more frames recorded draw by draw. Needs
    /// PassRecorder::Install for @p perPass.
    GpuTiming TimeGPU(const Config &config, int width, int height, TimeMode mode, const LightCounts &lights,
                      bool perPass);

    /// Render ONE frame of whatever @p effect holds into a @p width x
    /// @p height target cleared to transparent black, and read it back with
    /// its alpha. No SetConfig, no Update: the caller drives the effect.
    RGBA RenderOnce(EdgeLightingEffect &effect, int width, int height);

    /// Errors of @p image against @p reference. Fills @p heatmap with the
    /// page's error ramp when it is non-null.
    Metrics Measure(const RGB &image, const RGB &reference, RGB *heatmap);

    /// The rounded Rec. 709 luma down column @p x, from row @p y0, 73 rows.
    std::vector<int> ProfileColumn(const RGB &image, int x, int y0);

    /// One motion-sweep step: the window is every row whose centre lies within
    /// 12 px of the true edge @p edgeY on column @p x.
    SweepStep MeasureSweep(const RGB &image, int x, double edgeY);

    /// Largest per-channel difference between two equal-sized images.
    int MaxDifference(const RGB &a, const RGB &b);

    bool LoadPNG(const std::string &path, RGB &out, int &width, int &height);
    bool WritePNG(const std::string &path, const RGB &rgb, int width, int height);
    bool LoadPNG4(const std::string &path, RGBA &out, int &width, int &height);
    bool WritePNG4(const std::string &path, const RGBA &rgba, int width, int height);
}

#endif
