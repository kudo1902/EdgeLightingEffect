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

    /// GL_RENDERER of the current context - the GPU every timing belongs to.
    std::string RendererName();

    /// A neon-only effect, initialised. One per measurement where the method
    /// asks for a freshly initialised effect (timing); otherwise reused.
    void CreateEffect(EdgeLightingEffect &effect);

    /// Set @p config, tick the clock once by 0, and render one frame into an
    /// offscreen FRAME_WIDTH x FRAME_HEIGHT target over rgb(5, 5, 8).
    RGB Render(EdgeLightingEffect &effect, const Config &config);

    /// Minimum over 5 runs of the mean frame time of 40 Render() calls between
    /// glFinish, after 5 warm-up frames, on a freshly initialised effect, into
    /// a @p width x @p height target. ms. @p initMs, when non-null, receives
    /// how long constructing and initialising that effect took - the shader
    /// compiles, which a host pays once per effect.
    double TimeRender(const Config &config, int width = FRAME_WIDTH, int height = FRAME_HEIGHT,
                      double *initMs = nullptr);

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
}

#endif
