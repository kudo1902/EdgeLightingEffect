#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

#include "renderer/neon-tuning.h" // MAX_SEGMENT_BOOSTS + MAX_ARCS shared with the shaders
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

    /// Per-side hard geometric limit for the neon glow, packaged as a small
    /// struct so both sides can be toggled and tuned independently.
    ///
    ///   enable   - true = clamp the emission at @c size (with a @c softness
    ///              feather); false = the side is uncapped and the emission's
    ///              natural halo/bloom decay bounds it.
    ///   size     - distance in pixels from the rect edge to the cutoff
    ///              boundary along this side (always positive; sign is
    ///              implicit in whether it's the inside or outside cutoff).
    ///   softness - feather width in pixels at the cutoff boundary. 0 = hard
    ///              edge; larger values fade the neon emission smoothly to
    ///              zero over the boundary. Independent per side.
    typedef struct Cutoff
    {
        bool enable = true;
        float size = 32.0f;
        float softness = 4.0f;

        bool operator==(const Cutoff &o) const
        {
            return enable == o.enable && size == o.size && softness == o.softness;
        }
        bool operator!=(const Cutoff &o) const { return !(*this == o); }
    } Cutoff;

    /// Where the opaque-mode fill covers pixels. All modes fill @c
    /// NeonConfig::opaqueColor; the neon emission still composites on top of the
    /// fill inside the glow band. Cutoff distances come from @c
    /// NeonConfig::insideCutoff / @c outsideCutoff (both positive pixel values
    /// measured from the rect edge along their respective sides).
    typedef enum class OpaqueMode
    {
        NONE,    ///< No opaque pass; the effect composites transparently over whatever's behind it.
        OUTSIDE, ///< Fill only the outer half of the band: 0 <= d <= outsideCutoff.
        INSIDE,  ///< Fill only the inner half of the band: -insideCutoff <= d <= 0.
        BOTH,    ///< Fill the whole band: -insideCutoff <= d <= +outsideCutoff.
        ALL      ///< Fill the whole viewport (matches the old opaque=true + glowSide=BOTH behaviour).
    } OpaqueMode;

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

    /// A travelling coloured light on the perimeter. Several can be active at
    /// once; each contributes additively to the shader output (independent of
    /// @c NeonConfig::intensity), so a segment can shine on a dark arc.
    ///
    /// The @c boost sets its peak brightness (was a multiplier under the old
    /// multiplicative model; now the segment's absolute amplitude in the
    /// additive compose). The @c length sets the segment's visible span on the
    /// perimeter - @c colorStops are laid across that span head-to-tail.
    ///
    /// If @c colorStops is empty, the segment inherits the base gradient at
    /// its current perimeter samples - so a plain boost without stops looks
    /// like a bright spot in the base colour, matching the old feel.
    typedef struct SegmentBoost
    {
        float position = 0.0f; ///< Centre in [0, 1) perimeter position.
        float length = 0.15f;  ///< Width as a fraction of the perimeter (~2σ).
        float boost = 0.0f;    ///< Peak brightness (absolute; added on top of the base arc).
        /// Colour stops laid across the segment's span, head-to-tail. Empty
        /// means "inherit the base gradient" - the segment then reads its
        /// colour from @c NeonConfig::colorStops at each perimeter sample it
        /// touches.
        std::vector<ColorStop> colorStops;
        /// Blend space for interpolating @c colorStops. Ignored when
        /// @c colorStops is empty (the base gradient's blend space applies).
        BlendSpace blendSpace = BlendSpace::RGB;

        bool operator==(const SegmentBoost &o) const
        {
            return position == o.position &&
                   length == o.length &&
                   boost == o.boost &&
                   colorStops == o.colorStops &&
                   blendSpace == o.blendSpace;
        }
        bool operator!=(const SegmentBoost &o) const { return !(*this == o); }
    } SegmentBoost;

    /// A slice of the perimeter that is "on". Several can coexist; overlap
    /// resolves winner-take-all (the arc with the largest mask * intensity
    /// at a given sample owns the emission there), so two adjacent arcs of
    /// different colours crossfade smoothly at the seam because the arc mask
    /// is already smoothstepped one sample-wide.
    ///
    /// The @c intensity multiplier is independent of @c NeonConfig::intensity -
    /// dropping one arc's intensity to 0 leaves the others unaffected.
    ///
    /// If @c colorStops is empty, the arc inherits the base gradient at its
    /// current perimeter samples, matching the pre-multi-arc behaviour.
    typedef struct Arc
    {
        float start = 0.0f;     ///< Arc start in [0, 1) perimeter position.
        float length = 1.0f;    ///< Fraction of the perimeter lit (0 = off, 1 = full).
        float intensity = 1.0f; ///< Per-arc brightness multiplier (independent of NeonConfig::intensity).
        /// Colour stops laid across the arc's span, head-to-tail. Empty means
        /// "inherit the base gradient" - the arc then reads its colour from
        /// @c NeonConfig::colorStops at each perimeter sample it touches.
        std::vector<ColorStop> colorStops;
        /// Blend space for interpolating @c colorStops. Ignored when
        /// @c colorStops is empty (the base gradient's blend space applies).
        BlendSpace blendSpace = BlendSpace::RGB;

        bool operator==(const Arc &o) const
        {
            return start == o.start &&
                   length == o.length &&
                   intensity == o.intensity &&
                   colorStops == o.colorStops &&
                   blendSpace == o.blendSpace;
        }
        bool operator!=(const Arc &o) const { return !(*this == o); }
    } Arc;

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

        /// Where (if anywhere) an opaque background fill is drawn behind the
        /// neon emission. NONE keeps the effect purely additive over whatever
        /// was previously in the framebuffer; the other modes rasterise a
        /// coloured shape defined by @c insideCutoff / @c outsideCutoff (see
        /// the @c OpaqueMode enum for exact geometry). The neon glow composites
        /// on top of the fill inside the band.
        OpaqueMode opaqueMode = OpaqueMode::NONE;

        /// Fill colour for the opaque-mode background pass. Applied whenever
        /// @c opaqueMode != NONE. Linear RGBA in [0,1]; only @c .rgb is used
        /// today - the @c .a channel is reserved for a later premultiplied
        /// partial-fill pass and is applied by neither the renderer nor the
        /// shader yet. Default is black.
        glm::vec4 opaqueColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

        /// Feather width in pixels applied at the opaque-mode fill's cutoff
        /// boundaries. Used only when @c opaqueMode != NONE. 0 = hard fill
        /// edge; larger values soften where the fill fades into the
        /// background. Kept independent of the per-side @c Cutoff::softness
        /// so the emission and the fill can taper at different rates - e.g.
        /// a wide fill fade under a tight emission fall-off.
        float opaqueSoftness = 0.0f;

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

        /// Master brightness multiplier applied to the arc emission
        /// (filament + halo + bloom). Multiplies the whole effect uniformly -
        /// use per-arc @c Arc::intensity to fade an individual slice while
        /// others stay lit. Segments deliberately bypass this multiplier so
        /// a segment can shine on a dark arc.
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
        /// feather. Ignored when glowSide == BOTH. Does NOT affect the
        /// inside/outside cutoff boundaries - those use @c cutoffSoftness
        /// below so the two feathers can be tuned independently.
        float glowSideSoftness = 0.0f;

        /// Inside cutoff (rect interior side). See @ref Cutoff for the fields.
        /// The emission fades to zero over @c insideCutoff.softness at
        /// @c d = -insideCutoff.size and is culled past it. Also caps the
        /// geometric footprint of @c OpaqueMode::INSIDE / @c BOTH fills.
        /// @c insideCutoff.enable = false leaves the interior uncapped.
        Cutoff insideCutoff = {false, 0.0f, 0.0f};

        /// Outside cutoff (rect exterior side). Mirror of @c insideCutoff.
        /// Also caps @c OpaqueMode::OUTSIDE / @c BOTH fills and sizes the
        /// neon draw quad so far-exterior pixels are rasteriser-culled.
        /// @c outsideCutoff.enable = false leaves the exterior uncapped
        /// (natural halo / bloom decay bounds the emission instead).
        Cutoff outsideCutoff = {false, 0.0f, 0.0f};

        // --- Color ---
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

        // --- Arc gating (which slices of the perimeter are "on") ---
        //
        // Each @ref Arc in @c arcs is a slice of the perimeter that
        // emits: it starts at @c Arc::start and extends @c length of the
        // perimeter forwards (wrapping over 0/1 if needed). Overlapping arcs
        // resolve winner-take-all in the shader - the arc with the largest
        // effective mask (arcMask * intensity) at a sample owns its colour
        // and intensity there.
        //
        //   arcs = { {0, 1, 1} }         → full perimeter lit (default)
        //   arcs = { {0, 0.5, 1} }       → first half lit
        //   arcs = { {0.8, 0.4, 1} }     → wraps: lit from 0.8 to 0.2
        //   arcs = { {0, 0.3, 1},
        //            {0.5, 0.3, 0.5} }   → two independent slices, second dimmer
        //
        // The @ref OutlineTracer / @ref ArcWipe animations drive @c arcs[0]
        // for backward-compat with the pre-multi-arc single-slice API.
        static constexpr int MAX_ARCS_CAP = MAX_ARCS;
        std::vector<Arc> arcs = {Arc{}};

        // --- Colour transition ---

        /// Seconds to cross-fade the baked gradient when the colour stops (or
        /// blend space) change. 0 = instant snap (the old behaviour). Because
        /// the fade blends the whole 256-texel LUT rather than pairing stops,
        /// it works even when the two stop sets differ in count or position.
        float colorTransitionDuration = 0.3f;

        bool operator==(const NeonConfig &o) const
        {
            return enable == o.enable &&
                   showGradientLUT == o.showGradientLUT &&
                   showColorStops == o.showColorStops &&
                   opaqueMode == o.opaqueMode &&
                   opaqueColor == o.opaqueColor &&
                   lineWidth == o.lineWidth &&
                   filamentFalloff == o.filamentFalloff &&
                   intensity == o.intensity &&
                   glowRadius == o.glowRadius &&
                   bloomStrength == o.bloomStrength &&
                   glowSide == o.glowSide &&
                   glowSideSoftness == o.glowSideSoftness &&
                   insideCutoff == o.insideCutoff &&
                   outsideCutoff == o.outsideCutoff &&
                   opaqueSoftness == o.opaqueSoftness &&
                   blendSpace == o.blendSpace &&
                   colorStops == o.colorStops &&
                   hueRotationRate == o.hueRotationRate &&
                   segmentBoosts == o.segmentBoosts &&
                   arcs == o.arcs &&
                   colorTransitionDuration == o.colorTransitionDuration;
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
        /// Number of gather samples per fragment (max = NEON_MAX_LOOP_SAMPLES,
        /// lower = faster). Clamped by the renderer at upload time.
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

    /// Rain-on-glass droplets configuration.
    ///
    /// Droplets live in a band that follows the rounded-rect perimeter, whose
    /// thickness is @c bandWidth and whose side comes from
    /// @c NeonConfig::glowSide. Rain falls with screen-space gravity, so it
    /// streaks down the vertical runs of the band and beads along the
    /// horizontal ones. Droplet size scales with @c bandWidth, so the effect
    /// holds up however thin the band is.
    ///
    /// Drops are self-lit (transparent body + crescent rim + specular dot);
    /// there is no framebuffer capture or refraction pass. Refraction had
    /// nothing to displace over the smooth neon gradient this band lives on,
    /// so the whole capture/lens/wet-glass path was removed.
    typedef struct DropletsConfig
    {
        bool enable = false; ///< Enable or disable the droplets renderer

        /// Rain amount [0-1]. Drives the density of all droplet layers:
        /// low values leave only static condensation, high values add two
        /// trickling layers with trails.
        float amount = 0.7f;
        /// Trickle speed multiplier. 1 = the reference pace; 0 freezes the rain.
        float speed = 1.0f;
        /// Number of droplet lanes across the band, clamped to >= 1.
        /// 1 = drops as wide as the band; 2 = two lanes of half-width drops,
        /// and so on. Cell size follows from this and @c bandWidth: one lane
        /// is @c bandWidth / @c lanes pixels wide, so drops fit the band at
        /// any thickness.
        int lanes = 1;

        /// Band thickness in pixels. This is the droplets' entire world: the
        /// field is parameterised across it, and droplet size scales with it.
        /// Which side of the rect edge the band occupies is taken from
        /// @c NeonConfig::glowSide - OUTSIDE grows outward, INSIDE inward,
        /// BOTH straddles the edge centred on it.
        float bandWidth = 24.0f;
        /// Gap in pixels between the rect edge and the band's inner boundary.
        /// 0 puts the band flush against the edge.
        float bandOffset = 0.0f;
        /// Drop colour multiplier (.rgb used; .a reserved). Slightly blue by
        /// default for a cold-water cast; white = untinted. Only the drop's
        /// faint body tint uses this; the rim and specular highlights stay
        /// white so drops read against any glow colour.
        glm::vec4 tint = glm::vec4(0.85f, 0.90f, 1.0f, 1.0f);

        bool operator==(const DropletsConfig &o) const
        {
            return enable == o.enable &&
                   amount == o.amount &&
                   speed == o.speed &&
                   lanes == o.lanes &&
                   bandWidth == o.bandWidth &&
                   bandOffset == o.bandOffset &&
                   tint == o.tint;
        }
        bool operator!=(const DropletsConfig &o) const { return !(*this == o); }
    } DropletsConfig;

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

    /// Lens-flare renderer configuration. Draws a full lens flare (sun core
    /// with rays + hex-aperture chromatic ghosts) as a single fullscreen pass.
    /// See lens-flare.frag for the port notes and licence caveat.
    ///
    /// The sun rides the rectangle's perimeter (same parameter space as
    /// @c SegmentBoost::position and @c Arc::start) so it animates with
    /// the same modulators the neon uses, and stays visually tied to the
    /// frame no matter where the geometry moves.
    typedef struct LensFlareConfig
    {
        bool enable = false;
        /// Sun position as a perimeter progress in [0, 1). 0 = top-left,
        /// winding follows @c RectGeometry::winding. The renderer converts
        /// this to a viewport pixel via @c GeometryUtils::GetPointOnRectangle.
        float perimeterPosition = 0.0f;
        /// Signed offset in pixels along the edge normal at
        /// @c perimeterPosition. Positive pushes the sun outward (away from
        /// the rect centre), negative pulls it inward.
        float perimeterOffset = 0.0f;
        /// Size scale for the sun core + rays. 1.0 = reference look; larger
        /// values grow the visible disc and rays proportionally. Ghosts sit
        /// along the axis in normalised viewport space, so they are not
        /// affected by size.
        float size = 1.0f;
        /// Warm tint applied to the sun core + rays. Ghosts stay procedural.
        glm::vec4 color = glm::vec4(1.0f, 0.92f, 0.75f, 1.0f);
        /// Master brightness multiplier.
        float intensity = 1.0f;
        /// Ghost / hex-aperture strength (0 = suppress ghosts, 1 = reference).
        float spread = 1.0f;
        /// Angular density of the ray pattern in [0, 1]. 0 = a single broad
        /// ray, 1 = the densest packed sunburst. The renderer quantises this
        /// to an integer slot count internally (so the shader's ray pattern
        /// closes cleanly at the 2 PI wrap); this field is a fraction so the
        /// caller doesn't have to think in slot counts.
        ///
        /// Not directly countable on screen: each slot's ray gets a random
        /// length in [0.15x, 1.0x] (see lens-flare.frag), so some slots
        /// produce visible spikes and others produce short stubs.
        float rayDensity = 0.25f;
        /// Sun / ray rotation rate in revolutions per second. 0 = static;
        /// positive = counter-clockwise (screen space, y-up). Ghosts stay
        /// anchored on the sun-to-centre axis and are not rotated.
        float rotationRate = 0.0f;

        bool operator==(const LensFlareConfig &o) const
        {
            return enable == o.enable &&
                   perimeterPosition == o.perimeterPosition &&
                   perimeterOffset == o.perimeterOffset &&
                   size == o.size &&
                   color == o.color &&
                   intensity == o.intensity &&
                   spread == o.spread &&
                   rayDensity == o.rayDensity &&
                   rotationRate == o.rotationRate;
        }
        bool operator!=(const LensFlareConfig &o) const { return !(*this == o); }
    } LensFlareConfig;

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
        DropletsConfig droplets;           ///< Rain-on-glass droplets settings
        WireframeConfig wireframe;         ///< Wireframe overlay settings
        LensFlareConfig lensFlare;         ///< Sun + lens flare (rays, chromatic ghosts)

        bool operator==(const Config &o) const
        {
            return geometry == o.geometry &&
                   neon == o.neon &&
                   optimizedNeon == o.optimizedNeon &&
                   droplets == o.droplets &&
                   wireframe == o.wireframe &&
                   lensFlare == o.lensFlare;
        }
        bool operator!=(const Config &o) const { return !(*this == o); }
    } Config;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
