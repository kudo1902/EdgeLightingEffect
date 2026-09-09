#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

#include "renderer/neon-tuning.h" // MAX_SEGMENT_BOOSTS + MAX_ARCS shared with the shaders
#include <glm/glm.hpp>
#include <cstdint>
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
    ///
    /// @c color.a is an EMISSION SCALE at this stop, not a blend opacity: the
    /// renderers bake it into their LUTs' alpha channel and the neon shaders
    /// multiply it into the emission magnitude, so it attenuates the filament,
    /// halo and bloom together. 1 = full brightness (default), 0 = dark at
    /// that perimeter position with the background showing through - which is
    /// how you fade the neon out along part of the ring without touching the
    /// arc or segment gating. It interpolates linearly between stops in every
    /// @ref BlendSpace (the hue-space conversions apply to @c .rgb only).
    typedef struct ColorStop
    {
        float position;  ///< Normalised position along the perimeter [0-1].
        glm::vec4 color; ///< RGB colour + alpha emission scale at this stop.

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

    /// A @ref SegmentBoost tagged with a stable identity for the preserved pool
    /// (@c NeonConfig::preservedSegmentBoosts). Keeping the id out of
    /// @c SegmentBoost leaves that struct pure render data; identity is layered
    /// on only where it is needed. The id is handed out by
    /// @c SegmentUtils::AcquireSegment and is stable for the entry's lifetime -
    /// so an owner can address "its" entry by id regardless of resizes /
    /// releases / compaction of other entries, and independent callers never
    /// clobber each other while their ids are live. The id is unique among the
    /// live entries but not permanently unique: releasing an entry can free its
    /// id for a later acquire to reuse (see @c SegmentUtils::AcquireSegment), so
    /// an id must not be used after its entry is released. @c id 0 is never
    /// handed out (means "invalid").
    typedef struct PreservedSegment
    {
        uint32_t id = 0;      ///< Stable for the entry's lifetime (>= 1 when live).
        SegmentBoost segment; ///< The hotspot's render parameters.

        bool operator==(const PreservedSegment &o) const
        {
            return id == o.id && segment == o.segment;
        }
        bool operator!=(const PreservedSegment &o) const { return !(*this == o); }
    } PreservedSegment;

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

        // --- Resolution and cost knobs ---
        //
        // The renderer draws either straight onto the framebuffer it was handed
        // (@c resolutionScale 1.0) or into a scaled offscreen buffer that is
        // bilinear-blitted back (below 1.0). The defaults here are the full-res
        // path, so a config left untouched renders as it always has.

        /// Resolution scale for the internal neon buffer. 1.0 draws the gather
        /// directly onto the caller's framebuffer - no offscreen buffer and no
        /// blit. Below 1.0 the gather runs into a buffer of that fraction of
        /// the viewport and is composited back with bilinear filtering
        /// (0.5 = half-res, 0.25 = quarter). Clamped to (0, 1] at draw time.
        ///
        /// What this buys is fragment work, which dominates the effect: the
        /// glow quad is large and every fragment inside it walks the sample
        /// loop.
        float resolutionScale = 1.0f;

        /// Number of perimeter gather samples per fragment. Capped at
        /// @c NEON_MAX_LOOP_SAMPLES - the UBO and the shader's array are both
        /// sized by it - and clamped to >= 1 at upload time. Lower is faster
        /// and makes the halo grainier, since the gap between lit samples
        /// grows.
        int numSamples = NEON_MAX_LOOP_SAMPLES;

        /// Width in texels of the baked colour-ring LUT (power of two, 32-256).
        /// 256 resolves any gradient the eye can; a smaller ring bakes faster
        /// and costs less texture memory. A change to this SNAPS rather than
        /// cross-fading - two rings of different length cannot be blended
        /// element-wise (see @ref GradientRingLUT).
        int gradientLutSize = 256;

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

        /// TOTAL feather width in pixels at the opaque-mode fill's cutoff
        /// boundaries: the fade runs from fully covered to fully clear over
        /// this many pixels, centred on the boundary. Used only when
        /// @c opaqueMode != NONE. 0 = hard fill edge (floored to a 1 px
        /// anti-aliasing ramp so it cannot stair-step); larger values soften
        /// where the fill fades into the background. Kept independent of the
        /// per-side @c Cutoff::softness so the emission and the fill can taper
        /// at different rates - e.g. a wide fill fade under a tight emission
        /// fall-off.
        ///
        /// NOTE the shader used to spread this over 2x the stated width, so
        /// values tuned before that fix render half as wide now (and finally
        /// match this doc). @c Cutoff::softness still carries the old 2x
        /// convention in the neon shaders - the two are not comparable.
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

        /// Maximum number of active segment boosts (matches the shader array
        /// size). This is the cap on the *merged* set
        /// (@c SegmentUtils::FillEffectiveSegments) - the transient and preserved
        /// pools share these slots.
        static constexpr int MAX_SEGMENT_BOOSTS_CAP = MAX_SEGMENT_BOOSTS;

        /// Transient, index-addressed hotspots. This is the "freely overwritten"
        /// pool: @c set_segment_boost_count resizes it, @c clear_segment_boosts
        /// empties it, and the index-based animations (@ref SegmentTravel etc.)
        /// write into it by slot. Because index *is* identity here, independent
        /// writers to the same slot clobber each other - by design, for callers
        /// that own the whole pool. Use @c preservedSegmentBoosts when you need
        /// an entry that survives those bulk overrides.
        std::vector<SegmentBoost> segmentBoosts;

        // --- Preserved segment boosts (id-addressed, override-proof) ---------
        //
        // A second, independent pool. Its entries are addressed by
        // @c PreservedSegment::id (handed out by @c SegmentUtils::AcquireSegment),
        // never by index, and nothing that overrides the *transient* pool touches it:
        // clearing / resizing / rebuilding @c segmentBoosts leaves preserved
        // entries intact. This is the storage a caller reaches for when a
        // hotspot must persist regardless of what other actions do to the
        // segment set. The renderer composites both pools
        // (@c SegmentUtils::FillEffectiveSegments).

        /// Preserved, id-addressed hotspots (each a @ref PreservedSegment: an id
        /// plus its @c SegmentBoost). Only the id-based API / animation bindings
        /// mutate these; the transient bulk operations never do.
        ///
        /// The id allocator and the pool operations (acquire / find-by-id /
        /// release) live in util/segment-utils.h (@c SegmentUtils::AcquireSegment
        /// etc.), not here - this struct is just the data. Ids are derived from
        /// the pool contents (one past the highest live id), so there is no
        /// separate counter to store; the ids themselves travel with the config
        /// in each @ref PreservedSegment entry and survive (de)serialization.
        std::vector<PreservedSegment> preservedSegmentBoosts;

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
                   resolutionScale == o.resolutionScale &&
                   numSamples == o.numSamples &&
                   gradientLutSize == o.gradientLutSize &&
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
                   preservedSegmentBoosts == o.preservedSegmentBoosts &&
                   arcs == o.arcs &&
                   colorTransitionDuration == o.colorTransitionDuration;
        }
        bool operator!=(const NeonConfig &o) const { return !(*this == o); }
    } NeonConfig;

    /// Debug configuration - everything that exists to inspect the effect
    /// rather than to be part of it.
    ///
    /// Two kinds live here, and they reach the frame differently:
    ///
    ///   - @c showGradientLUT / @c showColorStops / @c showWireframe are
    ///     ANNOTATIONS, drawn on top of the neon layer by @ref DebugRenderer.
    ///     Nothing in the neon renderer knows about them.
    ///   - @c opaqueOnly is a debug MODE of the neon layer: it changes which
    ///     of that renderer's passes run. @ref NeonRenderer reads it directly.
    ///
    /// So the read arrows point both ways across this struct and
    /// @ref NeonConfig - the debug renderer reads neon state to know what it
    /// is annotating, and the neon renderer reads this one flag to know
    /// whether to stop after the fill. That is the cost of collecting the
    /// debug surface in one place, and it is the right trade: a host looking
    /// for "the debug knobs" finds all of them here, and none of them are
    /// mixed in among the fields that describe how the effect should look.
    typedef struct DebugConfig
    {
        /// Master switch for the overlay layer. Defaults ON, and so does
        /// @c showWireframe, so a registered @ref DebugRenderer draws the
        /// bounding box out of the box exactly as the old WireframeRenderer
        /// did. This is the mute for turning the whole layer off without
        /// disturbing which overlays were selected.
        ///
        /// Applies to the three overlays only. @c opaqueOnly is a mode of the
        /// neon renderer, not something this layer draws, so it is not gated
        /// by this flag.
        bool enable = true;

        /// Draw the baked gradient LUT texture as a horizontal strip at the
        /// centre of the rectangle, so the colour ring actually going to the
        /// shader can be eyeballed.
        bool showGradientLUT = false;

        /// Draw a coloured dot at each colour-stop position on the perimeter,
        /// so the mapping (perimeter position -> colour) can be verified
        /// against the LUT strip and the on-screen glow.
        bool showColorStops = false;

        /// Draw a 1 px bounding box around the rectangle. Deliberately SHARP
        /// even when @c RectGeometry::cornerRadius is set: it shows the extent
        /// the config asked for, not the rounded outline the neon traces, so
        /// the two can be compared.
        ///
        /// Defaults ON, unlike the two overlays above - this is the old
        /// @c WireframeConfig::enable, kept at its original default so a host
        /// that had the box sees no change. Still gated by @c enable.
        bool showWireframe = true;

        /// Colour of that box. Opaque green by default: it reads against the
        /// dark backdrop and against most glow colours, and it is obviously
        /// not part of the effect.
        glm::vec4 wireframeColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

        /// Render ONLY the @c NeonConfig::opaqueMode fill and skip the neon
        /// emission entirely - no filament, no halo, no bloom. Isolates the
        /// fill's silhouette so it can be compared against where the light
        /// actually reaches.
        ///
        /// @ref DebugRenderer suppresses its two glow overlays while this is
        /// set - the LUT strip and the stop markers annotate the glow, and
        /// there is no glow to annotate here. The bounding box is NOT
        /// suppressed: it annotates the geometry, which is there whether or
        /// not anything is lit, so a fill-only frame still carries it unless
        /// @c showWireframe or @c enable is cleared. See the gating in
        /// @c DebugRenderer::Render.
        ///
        /// The two disagree at a sharp corner: @c cornerRadius 0 makes the
        /// fill's outer boundary square (the band is offset per-axis, see
        /// bandOuterDistance in the shaders) while the emission is shaped by
        /// the plain Euclidean SDF distance and stays radial. The wedge
        /// between the square mask and the round glow renders as fill with no
        /// light on it, and this flag is how you see its extent directly.
        ///
        /// No-ops when @c opaqueMode is NONE - there is no fill pass to keep,
        /// so nothing is drawn at all. The renderers read it at draw time and
        /// it triggers no rebuilds, so it is safe to toggle per-frame. Not
        /// exposed through the C API.
        bool opaqueOnly = false;

        bool operator==(const DebugConfig &o) const
        {
            return enable == o.enable &&
                   showGradientLUT == o.showGradientLUT &&
                   showColorStops == o.showColorStops &&
                   showWireframe == o.showWireframe &&
                   wireframeColor == o.wireframeColor &&
                   opaqueOnly == o.opaqueOnly;
        }
        bool operator!=(const DebugConfig &o) const { return !(*this == o); }
    } DebugConfig;

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
        /// Signed distance in pixels from the rect edge. Positive pushes the
        /// sun outward (away from the rect centre), negative pulls it inward.
        /// The sun rides the constant-distance offset of the rect - itself a
        /// rounded rect with radius @c cornerRadius + offset - and
        /// @c perimeterPosition advances by arc length along *that* curve, so
        /// the sun keeps a constant speed through the corners. An offset below
        /// -cornerRadius clamps the corners square rather than inverting them.
        float perimeterOffset = 0.0f;
        /// Radius scale for the sun disc. 1.0 = reference look; larger values
        /// grow the visible disc proportionally. Independent of @c intensity:
        /// the shader's sun terms carry a softening floor that gives each
        /// falloff a finite, size-invariant peak, so this sets the radius and
        /// @c intensity sets the brightness. (A raw power-law falloff is scale
        /// free, which made an uncompensated size scale behave as a second,
        /// non-linear brightness knob - see lens-flare.frag.)
        ///
        /// Does not scale the ray extent: the ray term has no distance falloff,
        /// so how far the rays reach is set by the shader's global envelope.
        /// Ghosts sit along the axis in normalised viewport space and are not
        /// affected by size either.
        float size = 1.0f;
        /// Warm tint applied to the sun core + rays. Ghosts stay procedural.
        glm::vec4 color = glm::vec4(1.0f, 0.92f, 0.75f, 1.0f);
        /// Master brightness multiplier.
        float intensity = 1.0f;
        /// Ghost / hex-aperture strength (0 = suppress ghosts, 1 = reference).
        float spread = 1.0f;
        /// Stretches the ghost placement along the sun-to-centre axis
        /// (1.0 = reference spacing). Because reference spacing is
        /// proportional to the sun-to-centre distance, ghosts crowd together
        /// when the sun sits near a screen edge (e.g. top-centre) and spread
        /// out at a corner; raise this to push them apart in the crowded case.
        /// Affects placement only - per-ghost colour and size are unchanged.
        float ghostSpacing = 1.0f;
        /// Uniform ghost size / falloff exponent shared by every ghost. The
        /// reference gave each ghost a random size in ~[1.4, 4.7]; this fixes
        /// them all to one value so they read as the same size. Larger = bigger
        /// softer ghosts. Default matches the reference's average look.
        float ghostSize = 2.2f;
        /// Signed shift of every ghost's distance along the sun-to-centre
        /// axis. dist 0 is the screen centre and dist ~ -1 sits on the sun, so
        /// 0.0 = reference (ghosts bloom around the centre) and negative values
        /// pull the whole cluster off centre and up against the sun / border
        /// edge. Default biases the ghosts toward the border.
        float ghostOffset = -1.5f;
        /// Colour the ghosts lean toward when @c ghostTint > 0 (linear RGB).
        glm::vec3 ghostColor = glm::vec3(1.0f, 1.0f, 1.0f);
        /// Blend from the procedural per-ghost rainbow (0.0 = reference look)
        /// to a single @c ghostColor for every ghost (1.0). Ghost brightness /
        /// falloff is unaffected; only the hue is tinted.
        float ghostTint = 0.0f;
        /// Ghost convergence / reference point in normalised screen coords
        /// (0..1, origin top-left, y-down). The ghosts pivot about this point
        /// and their sun->centre axis runs through it, instead of always using
        /// the screen centre. (0.5, 0.5) = screen centre = the historical look.
        /// The sun's own rays and vignette are unaffected.
        glm::vec2 flareCenter = glm::vec2(0.5f, 0.5f);
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

        /// Fraction of the viewport the flare is rendered at before being
        /// bilinear-blitted back to full resolution. 1.0 draws straight onto
        /// the target framebuffer with no offscreen buffer and no blit; below
        /// that costs @c resolutionScale^2 as many shaded fragments.
        ///
        /// Nearly lossless here, more so than for the neon: the flare is all
        /// smooth low-frequency light (soft glow, ghosts, rays) and the shader
        /// normalises every term by @c uResolution, so it is scale invariant -
        /// drawing into a smaller buffer reproduces the same picture rather
        /// than a differently-shaped one.
        ///
        /// Clamped to (0, 1] at draw time. Above 1.0 is refused rather than
        /// supersampled: the point of the knob is to shade FEWER fragments,
        /// and honouring 2.0 would quietly allocate four times the viewport.
        float resolutionScale = 1.0f;
bool operator==(const LensFlareConfig &o) const
        {
            return enable == o.enable &&
                   perimeterPosition == o.perimeterPosition &&
                   perimeterOffset == o.perimeterOffset &&
                   size == o.size &&
                   color == o.color &&
                   intensity == o.intensity &&
                   spread == o.spread &&
                   ghostSpacing == o.ghostSpacing &&
                   ghostSize == o.ghostSize &&
                   ghostOffset == o.ghostOffset &&
                   ghostColor == o.ghostColor &&
                   ghostTint == o.ghostTint &&
                   flareCenter == o.flareCenter &&
                   rayDensity == o.rayDensity &&
                   rotationRate == o.rotationRate &&
                   resolutionScale == o.resolutionScale;
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
        RectGeometry geometry;                       ///< Rectangle geometry
        NeonConfig neon;                             ///< Neon stroke settings, including its resolution scale
        DebugConfig debug;                           ///< LUT strip / colour-stop marker overlays
        DropletsConfig droplets;                     ///< Rain-on-glass droplets settings
        LensFlareConfig lensFlare;                   ///< Sun + lens flare (rays, chromatic ghosts)

        bool operator==(const Config &o) const
        {
            return geometry == o.geometry &&
                   neon == o.neon &&
                   debug == o.debug &&
                   droplets == o.droplets &&
                   lensFlare == o.lensFlare;
        }
        bool operator!=(const Config &o) const { return !(*this == o); }
    } Config;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
