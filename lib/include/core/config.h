#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

#include "renderer/neon-tuning.h" // MAX_SEGMENT_BOOSTS shared with the shaders
#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    // -----------------------------------------------------------------------
    // Shared types
    // -----------------------------------------------------------------------

    /// Direction of traversal around the perimeter.
    typedef enum class Winding
    {
        CLOCKWISE,
        COUNTER_CLOCKWISE
    } Winding;

    /// Which side of the rectangle edge the neon glow is emitted from.
    typedef enum class GlowSide
    {
        BOTH,   ///< Glow on both sides of the line (default neon look)
        INSIDE, ///< Glow only inside the rectangle (outer half goes dark)
        OUTSIDE ///< Glow only outside the rectangle (interior goes dark)
    } GlowSide;

    /// Interpolation colour space for multi-stop blending.
    typedef enum class BlendSpace
    {
        RGB, ///< Blend directly in linear RGB.
        HSV, ///< Convert to HSV, interpolate, convert back (avoids muddy mid-tones).
        HSL  ///< Convert to HSL, interpolate; smoother mid-tones through neutral gray.
    } BlendSpace;

    /// A colour stop along the perimeter.
    typedef struct ColorStop
    {
        float position;  ///< Normalised position along the perimeter [0-1].
        glm::vec4 color; ///< RGBA colour at this stop.

        bool operator==(const ColorStop &o) const
        {
            return position == o.position && color == o.color;
        }
        bool operator!=(const ColorStop &o) const { return !(*this == o); }
    } ColorStop;

    /// A travelling brightness peak on the perimeter. Several of these can be
    /// active at once; the shader sums their Gaussian bells per sample.
    typedef struct SegmentBoost
    {
        float position = 0.0f; ///< Centre in [0, 1) perimeter position.
        float length = 0.15f;  ///< Width as a fraction of the perimeter (~2σ).
        float boost = 0.0f;    ///< Peak brightness multiplier at the centre.

        bool operator==(const SegmentBoost &o) const
        {
            return position == o.position &&
                   length == o.length &&
                   boost == o.boost;
        }
        bool operator!=(const SegmentBoost &o) const { return !(*this == o); }
    } SegmentBoost;

    // -----------------------------------------------------------------------
    // Per-renderer configuration
    // -----------------------------------------------------------------------

    /// Geometry of the target rectangle.
    typedef struct RectGeometry
    {
        float width = 800.0f;                       ///< Rectangle width in pixels
        float height = 600.0f;                      ///< Rectangle height in pixels
        glm::vec2 position = glm::vec2(0.0f, 0.0f); ///< Top-left corner in viewport coordinates
        float cornerRadius = 40.0f;                 ///< Corner radius in pixels (0 = sharp corners)
        /// Traversal direction around the perimeter.
        /// CW starts at top-left and goes top → right → bottom → left (clockwise).
        /// CCW starts at top-left and goes left → bottom → right → top (counter-clockwise).
        Winding winding = Winding::COUNTER_CLOCKWISE;

        bool operator==(const RectGeometry &o) const
        {
            return width == o.width &&
                   height == o.height &&
                   position == o.position &&
                   cornerRadius == o.cornerRadius &&
                   winding == o.winding;
        }
        bool operator!=(const RectGeometry &o) const { return !(*this == o); }
    } RectGeometry;

    /// Neon-style LED strip rendering configuration.
    ///
    /// The renderer composites three layers: a crisp **filament** (the bright
    /// line), a sharp coloured **halo** around it, and a wide soft background
    /// **bloom**. All three are summed in HDR and tone-mapped together.
    typedef struct NeonConfig
    {
        bool enable = false; ///< Enable or disable the neon renderer

        // --- Debug visualisations ---

        /// Debug: draw the baked gradient LUT texture as a horizontal strip at
        /// the centre of the rectangle so you can eyeball the colour ring
        /// that's actually going to the shader.
        bool showGradientLUT = false;

        /// Debug: draw a coloured dot at each colour-stop position on the
        /// perimeter so the mapping (perimeter position → colour) can be
        /// verified against the LUT and the on-screen glow.
        bool showColorStops = false;

        // --- Compositing ---

        /// How the effect combines with whatever is already in the framebuffer.
        /// false (default): premultiplied-alpha "over" - the dark surround is
        ///   transparent, so the effect composites onto the background.
        /// true: opaque - the effect's surround pixels are filled with
        ///   @c opaqueColor, occluding the background within the effect's draw
        ///   region. The neon glow is composited on top.
        bool opaque = false;
        /// Fill colour for the opaque-mode background pass. Applied only when
        /// @c opaque is true. Linear RGB in [0,1]; default is black.
        glm::vec3 opaqueColor = glm::vec3(0.0f);

        // --- Filament (the bright line itself) ---

        /// Width of the bright filament line in pixels.
        /// Sets the line *width*; peak brightness stays constant regardless of value.
        float lineWidth = 4.0f;

        /// Filament brightness falloff shape from the axis outward. The shader
        /// uses a generalized Gaussian `exp(-ln(2) * (ad/sigma)^N)` where
        /// N = 2 * filamentFalloff and sigma = half-brightness radius. Peak
        /// brightness on the axis (ad = 0) is always exactly 1.0; this value
        /// controls how the sides fall off:
        ///   0.5 = Laplace-like (N=1) - heavier tails, very smooth peak
        ///   1.0 = pure Gaussian (N=2) - clean smooth falloff (default)
        ///   2.0 = platykurtic (N=4) - flatter top, sharper shoulder
        ///   >3  = near-rectangular (N>=6) - plateau IS the line, edges crisp
        /// The Gaussian has no power-law tail, so the filament reads as a
        /// cleaner thin line even at wider lineWidth values. Lower values
        /// give a smoother, softer roll-off; higher values sharpen the edge.
        float filamentFalloff = 1.0f;

        // --- Glow ---

        /// Master brightness multiplier applied to filament + halo + bloom.
        float intensity = 1.0f;
        /// Halo reach in pixels - how far the coloured glow spreads from the line.
        /// Also seeds the wider background bloom and corner colour cross-fade widths.
        float glowRadius = 5.0f;
        /// Strength of the wide soft background spill layered on top of the halo.
        /// 0 = halo only, ~0.3 = subtle ambient bleed, 1.0+ = strong wash.
        float bloomStrength = 0.30f;
        /// Restrict the glow to one side of the line, or let it spill both ways.
        GlowSide glowSide = GlowSide::BOTH;
        /// Softness of the one-sided cut in pixels. 0 = hard edge, 2 = subtle
        /// feather. Ignored when glowSide == BOTH.
        float glowSideSoftness = 0.0f;

        // --- Color ---

        /// Maximum number of colour stops. Sized to match the shader's
        /// perimeter loop-sample count (NUM_LOOP_SAMPLES = 128) so the picker
        /// can produce one stop per loop sample for near-1:1 image-to-neon
        /// colour reproduction. LUT baking is CPU-side, so no shader array
        /// limit applies.
        static constexpr int MAX_COLOR_STOPS = 128;
        /// Blend space for interpolating between colour stops.
        BlendSpace blendSpace = BlendSpace::RGB;
        /// Colour stops around the perimeter
        /// (1 stop = solid, 2 = gradient, 3+ = multi-stop circular).
        std::vector<ColorStop> colorStops = {
            {0.00f, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)},
            {0.25f, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)},
            {0.50f, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)},
            {0.75f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f)},
        };

        // --- Animation ---

        /// Hue rotation rate in revolutions per second around the perimeter. 0 = static.
        float hueRotationRate = 0.5f;

        // --- Travelling segments (spatial brightness bumps) ---
        //
        // Zero or more Gaussian-shaped brightness peaks ride on top of the base
        // neon line. Each entry has its own position / length / boost. The
        // shader sums the per-entry bell weights, so peaks can overlap.
        // Empty vector (default) means "no boost anywhere" - the feature costs
        // one skip in the gather loop and is otherwise free.
        //
        // Drive an entry's `position` over time with @ref SegmentTravel or
        // @ref SegmentBounce for a moving spot; keep another entry static for
        // a fixed hotspot, etc.

        /// Maximum number of active segment boosts (matches the shader array size).
        static constexpr int MAX_SEGMENT_BOOSTS_CAP = MAX_SEGMENT_BOOSTS;
        std::vector<SegmentBoost> segmentBoosts;

        // --- Arc gating (which slice of the perimeter is "on") ---
        //
        // The lit region starts at @c arcStart and extends @c arcLength of the
        // perimeter forwards (wrapping over 0/1 if needed). Samples outside
        // contribute zero brightness, so the bar / halo / bloom render only
        // inside the arc.
        //
        //   arcStart = 0.0, arcLength = 1.0 → full perimeter lit (default)
        //   arcStart = 0.0, arcLength = 0.0 → nothing lit
        //   arcStart = 0.0, arcLength = 0.5 → first half lit
        //   arcStart = 0.5, arcLength = 1.0 → still full, but the implicit
        //                                    "draw direction" is 0.5 → 1 → 0 → 0.5
        //   arcStart = 0.8, arcLength = 0.4 → wraps: lit from 0.8 to 0.2
        //
        // The @ref OutlineTracer animation drives @c arcLength from 0 → 1 to
        // "draw" the rect outline (with @c arcStart fixed at the start phase).
        float arcStart = 0.0f;
        float arcLength = 1.0f;

        bool operator==(const NeonConfig &o) const
        {
            return enable == o.enable &&
                   showGradientLUT == o.showGradientLUT &&
                   showColorStops == o.showColorStops &&
                   opaque == o.opaque &&
                   opaqueColor == o.opaqueColor &&
                   lineWidth == o.lineWidth &&
                   filamentFalloff == o.filamentFalloff &&
                   intensity == o.intensity &&
                   glowRadius == o.glowRadius &&
                   bloomStrength == o.bloomStrength &&
                   glowSide == o.glowSide &&
                   glowSideSoftness == o.glowSideSoftness &&
                   blendSpace == o.blendSpace &&
                   colorStops == o.colorStops &&
                   hueRotationRate == o.hueRotationRate &&
                   segmentBoosts == o.segmentBoosts &&
                   arcStart == o.arcStart &&
                   arcLength == o.arcLength;
        }
        bool operator!=(const NeonConfig &o) const { return !(*this == o); }
    } NeonConfig;

    /// Half-resolution optimized neon renderer configuration.
    ///
    /// Renders the neon shader at half resolution then bilinear-blits to full res.
    /// The perf wins are the half-res FBO + reduced gather samples (not reduced
    /// precision - the shader uses highp; mediump = fp16 on ANGLE overflowed the
    /// large fragment coordinates and produced NaN "noise dots").
    /// Visual parameters (line width, intensity, colour stops, etc.) are shared
    /// with Config::neon - adjust them in the Neon section of the debug UI.
    typedef struct OptimizedNeonConfig
    {
        bool enable = false; ///< Enable or disable the optimized neon renderer

        /// Resolution scale factor for the internal FBO (0.5 = half, 0.25 = quarter).
        float resolutionScale = 0.5f;
        /// Number of gather samples per fragment (max 64, lower = faster).
        int numSamples = 64;
        /// Size of the precomputed gradient look-up texture (power-of-two, 32–256).
        int gradientLutSize = 256;

        // --- Debug visualisations ---

        /// Show the raw half-res FBO (nearest-neighbour upscale) instead of
        /// the bilinear-blitted result. Useful to verify pass-1 rendering.
        bool showHalfRes = false;

        bool operator==(const OptimizedNeonConfig &o) const
        {
            return enable == o.enable &&
                   resolutionScale == o.resolutionScale &&
                   numSamples == o.numSamples &&
                   gradientLutSize == o.gradientLutSize &&
                   showHalfRes == o.showHalfRes;
        }
        bool operator!=(const OptimizedNeonConfig &o) const { return !(*this == o); }
    } OptimizedNeonConfig;

    /// Wireframe debug overlay configuration.
    typedef struct WireframeConfig
    {
        bool enable = true;                                  ///< Show or hide the wireframe bounding box
        glm::vec4 color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); ///< Wireframe color

        bool operator==(const WireframeConfig &o) const
        {
            return enable == o.enable && color == o.color;
        }
        bool operator!=(const WireframeConfig &o) const { return !(*this == o); }
    } WireframeConfig;

    // -----------------------------------------------------------------------
    // Top-level configuration
    // -----------------------------------------------------------------------

    /// Top-level configuration for the EdgeLightingEffect pipeline.
    ///
    /// Holds one sub-config per renderer. Renderers are independent - enable
    /// any subset; their visual layers composite via additive blending.
    typedef struct Config
    {
        RectGeometry geometry;             ///< Rectangle geometry
        NeonConfig neon;                   ///< Single-pass neon settings
        OptimizedNeonConfig optimizedNeon; ///< Half-res optimized neon settings
        WireframeConfig wireframe;         ///< Wireframe overlay settings

        bool operator==(const Config &o) const
        {
            return geometry == o.geometry &&
                   neon == o.neon &&
                   optimizedNeon == o.optimizedNeon &&
                   wireframe == o.wireframe;
        }
        bool operator!=(const Config &o) const { return !(*this == o); }
    } Config;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
