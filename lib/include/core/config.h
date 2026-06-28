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

    /// A straight edge of the rectangle. Used by GeometryUtils::GetEdgeArc to
    /// map an edge to its span in perimeter [0, 1] space (e.g. to place a
    /// LitSegment exactly over the left / right side edge).
    typedef enum class Edge
    {
        TOP,
        RIGHT,
        BOTTOM,
        LEFT
    } Edge;

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

    /// A bright accent nested inside a LitSegment.
    ///
    /// Its @c position / @c length are expressed in the parent segment's local
    /// space [0, 1] (0 = start of the segment, 1 = end). @c position is the
    /// accent's START; the accent covers [position, position + length] of the
    /// segment, so animating @c position slides it *along* its bar. The accent's
    /// @c boost adds on top of the segment's brightness, and it can carry its own colour
    /// (e.g. a white-hot core); leave @c colorStops empty to reuse the segment's
    /// colour. @c boost == 0 (default) disables it.
    typedef struct Spot
    {
        float position = 0.4f; ///< Start, in the parent segment's local [0, 1].
        float length = 0.2f;   ///< Width as a fraction of the parent segment.
        float boost = 0.0f;    ///< Extra emission added on top of the segment (0 = no accent).
        BlendSpace blendSpace = BlendSpace::RGB;
        std::vector<ColorStop> colorStops = {}; ///< Empty => inherit the segment colour.
    } Spot;

    /// A lit span along the perimeter.
    ///
    /// Several segments can light disjoint regions (e.g. the two side edges)
    /// while the rest of the perimeter stays dark. @c position is the START of
    /// the span; it runs forward to @c position + @c length (wrapping over 0/1).
    /// The @c brightness is combined across overlapping segments via max().
    ///
    /// Each segment carries its own colour (its @c colorStops sampled across the
    /// span) and an optional nested @c spot accent. All of this is baked
    /// per-sample on the CPU by @ref ColorUtils::BuildSampleData, so it costs
    /// nothing extra in the shader.
    typedef struct LitSegment
    {
        // --- Span ---
        float position = 0.0f;   ///< Start of the span, perimeter position [0, 1); runs to position + length.
        float length = 0.2f;     ///< Width as a fraction of the perimeter.
        float brightness = 1.0f; ///< Brightness (1 = normal, >1 = brighter, 0 = off).
                                 ///< Scales brightness only; the filament gate caps at coverage,
                                 ///< so raising it never extends the segment's lit length.

        // --- Colour (empty colorStops => solid white) ---
        BlendSpace blendSpace = BlendSpace::RGB;
        float hueRotationRate = 0.0f; ///< Revolutions/sec of this segment's gradient.
        std::vector<ColorStop> colorStops = {};

        // --- Nested accent ---
        Spot spot = {};
    } LitSegment;

    /// The classic 4-colour neon gradient (red → green → blue → yellow).
    inline std::vector<ColorStop> DefaultColorStops()
    {
        return {
            {0.00f, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)},
            {0.25f, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)},
            {0.50f, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)},
            {0.75f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f)},
        };
    }

    /// A single segment that fills the whole perimeter with the default rotating
    /// colour gradient — the out-of-the-box "colour ring around the rect" look,
    /// and the default contents of NeonConfig::segments. A full-ring segment
    /// (length 1) wraps its gradient seamlessly across the 0/1 seam.
    inline LitSegment DefaultRingSegment()
    {
        LitSegment s;
        s.position = 0.0f;
        s.length = 1.0f;
        s.brightness = 1.0f;
        s.hueRotationRate = 0.0f; // static by default; stops sit where placed
        s.colorStops = DefaultColorStops();
        return s;
    }

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

        // --- Lit segments (the whole effect; there is no separate global ring) ---
        //
        // Every lit region is a LitSegment carrying its own colour, rotation and
        // optional spot. The perimeter is dark wherever no segment covers it.
        // The default is a single full-ring segment (so out of the box this looks
        // like a colour ring around the rect); add more for side edges, arcs,
        // travelling dots, etc.
        //
        //   {DefaultRingSegment()}                  → colour ring (default)
        //   {left, right}                           → only those spans lit
        //   {position 0, length t}                  → a growing arc ("draw" it)
        //   {ring, short bright dot}                → ring + a bright spot
        //
        // Per-sample colour + weight are precomputed on the CPU by
        // @ref ColorUtils::BuildSampleData and uploaded as uSampleData[] (one
        // vec4 per sample: rgb = colour, a = weight), so the shader stays
        // segment-count agnostic, branch-free, and texture-free. Use
        // @ref GeometryUtils::GetEdgeArc to place a segment exactly over an edge.

        /// Maximum colour stops per segment gradient.
        static constexpr int MAX_COLOR_STOPS = 16;

        /// Maximum number of lit segments (caps the uploaded array / C API array).
        static constexpr int MAX_SEGMENTS = 8;

        /// Lit spans along the perimeter. Empty = nothing lit. Defaults to a
        /// single full-ring segment (see @ref DefaultRingSegment).
        std::vector<LitSegment> segments = {DefaultRingSegment()};
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
    /// Uses mediump precision + 64 gather samples for edge-device performance.
    /// Visual parameters (line width, intensity, colour stops, etc.) are shared
    /// with Config::neon — adjust them in the Neon section of the debug UI.
    typedef struct OptimizedNeonConfig
    {
        bool enable = false; ///< Enable or disable the optimized neon renderer

        /// Resolution scale factor for the internal FBO (0.5 = half, 0.25 = quarter).
        float resolutionScale = 0.5f;
        /// Number of gather samples per fragment (max 64, lower = faster).
        int numSamples = 64;

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
