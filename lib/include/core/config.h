#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

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
        HSV  ///< Convert to HSV, interpolate, convert back (avoids muddy mid-tones).
    } BlendSpace;

    /// A colour stop along the perimeter.
    typedef struct ColorStop
    {
        float position;  ///< Normalised position along the perimeter [0-1].
        glm::vec4 color; ///< RGBA colour at this stop.
    } ColorStop;

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
    } RectGeometry;

    /// Neon-style LED strip rendering configuration.
    ///
    /// The renderer composites three layers: a crisp **filament** (the bright
    /// line), a sharp coloured **halo** around it, and a wide soft background
    /// **bloom**. All three are summed in HDR and tone-mapped together.
    typedef struct NeonConfig
    {
        bool enable = false; ///< Enable or disable the neon renderer

        // --- Compositing ---

        /// How the effect combines with whatever is already in the framebuffer.
        /// false (default): premultiplied-alpha "over" — the dark surround is
        ///   transparent, so the effect composites onto the background.
        /// true: opaque — blending is disabled and the effect's pixels (a dark
        ///   surround around the lit line) are written directly, occluding the
        ///   background within the effect's draw region.
        bool opaque = false;

        // --- Filament (the bright line itself) ---

        /// Width of the bright filament line in pixels.
        /// Sets the line *width*; peak brightness stays constant regardless of value.
        float lineWidth = 4.0f;

        // --- Glow ---

        /// Master brightness multiplier applied to filament + halo + bloom.
        float intensity = 1.0f;
        /// Halo reach in pixels — how far the coloured glow spreads from the line.
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

        /// Maximum number of colour stops.
        static constexpr int MAX_COLOR_STOPS = 16;
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

        // --- Travelling segment (spatial brightness bump) ---
        //
        // A Gaussian-shaped brightness peak rides on top of the base neon
        // line. When @c segmentBoost == 0 (default) the feature is effectively
        // inactive. Drive @c segmentPosition over time with @ref SegmentTravel
        // for a moving spot.

        /// Centre of the bright segment, in [0, 1) perimeter position
        /// (same parametrisation as the colour ring).
        float segmentPosition = 0.0f;
        /// Segment width as a fraction of the perimeter (~Gaussian σ × 2).
        /// 0.15 ≈ "comfortable spot", 0.05 ≈ "tight dot", 0.5 ≈ "broad swell".
        float segmentLength = 0.15f;
        /// Brightness multiplier added at the head. 0 = disabled, 4 = strong.
        float segmentBoost = 0.0f;

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
    } NeonConfig;

    /// Multi-pass gradient-to-texture neon configuration.
    ///
    /// Pass 1 bakes the coloured stroke band into a screen-sized FBO.
    /// Pass 2 reads that texture via analytical closest-point projection
    /// and adds SDF-based filament / halo / bloom layers around it.
    typedef struct MultiPassNeonConfig
    {
        bool enable = false; ///< Enable or disable the multi-pass neon renderer

        // --- Debug visualisations (FBO verification) ---

        /// Debug: bypass the glow composite and show the gradient FBO directly
        /// as it was baked along the perimeter. Useful to verify that Pass 1 wrote
        /// the right colours in the right places.
        bool showPerimeterGradient = false;
        /// Debug: tell Pass 1 to write the gradient at *every* pixel (no perimeter
        /// mask), then show the FBO. Useful to verify the colour ring is correct
        /// across the whole rect.
        bool showFullGradient = false;

        // --- Filament (the bright line itself) ---

        /// Width of the bright filament line in pixels.
        float lineWidth = 4.0f;

        // --- Glow ---

        /// Master brightness multiplier applied to filament + halo + bloom.
        float intensity = 1.0f;
        /// Halo reach in pixels — how far the coloured glow spreads from the line.
        float glowRadius = 5.0f;
        /// Strength of the wide soft background spill layered on top of the halo.
        float bloomStrength = 0.30f;
        /// Restrict the glow to one side of the line, or let it spill both ways.
        GlowSide glowSide = GlowSide::BOTH;
        /// Softness of the one-sided cut in pixels. 0 = hard edge.
        float glowSideSoftness = 0.0f;

        // --- Color ---

        /// Maximum number of colour stops.
        static constexpr int MAX_COLOR_STOPS = 16;
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
    } MultiPassNeonConfig;

    /// Half-resolution optimized neon renderer configuration.
    ///
    /// Renders the neon shader at half resolution then bilinear-blits to full res.
    /// The perf wins are the half-res FBO + reduced gather samples (not reduced
    /// precision — the shader uses highp; mediump = fp16 on ANGLE overflowed the
    /// large fragment coordinates and produced NaN "noise dots").
    /// Visual parameters (line width, intensity, colour stops, etc.) are shared
    /// with Config::neon — adjust them in the Neon section of the debug UI.
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
    } OptimizedNeonConfig;

    /// Wireframe debug overlay configuration.
    typedef struct WireframeConfig
    {
        bool enable = true;                                  ///< Show or hide the wireframe bounding box
        glm::vec4 color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); ///< Wireframe color
    } WireframeConfig;

    // -----------------------------------------------------------------------
    // Top-level configuration
    // -----------------------------------------------------------------------

    /// Top-level configuration for the EdgeLightingEffect pipeline.
    ///
    /// Holds one sub-config per renderer. Renderers are independent — enable
    /// any subset; their visual layers composite via additive blending.
    typedef struct Config
    {
        RectGeometry geometry;             ///< Rectangle geometry
        NeonConfig neon;                   ///< Single-pass neon settings
        MultiPassNeonConfig multipassNeon; ///< Multi-pass neon settings
        OptimizedNeonConfig optimizedNeon; ///< Half-res optimized neon settings
        WireframeConfig wireframe;         ///< Wireframe overlay settings
    } Config;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
