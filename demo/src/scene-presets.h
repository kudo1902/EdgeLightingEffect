#ifndef _EDGE_LIGHTING_DEMO_SCENE_PRESETS_H_
#define _EDGE_LIGHTING_DEMO_SCENE_PRESETS_H_

#include "core/config.h"
#include "renderer/spotlight-tuning.h"
#include <algorithm>

namespace EdgeLightingDemo
{
    /// Whole-scene presets: a preset here rewrites @ref EdgeLighting::Config
    /// across SEVERAL renderers at once, unlike @ref AnimationPreset, which
    /// composes an animation and touches no config field directly.
    ///
    /// Each one is a worked example of making the library imitate a real
    /// fixture, so a preset deliberately also turns OFF the layers that
    /// fixture does not use. Applying one is not additive - it is "show me
    /// this scene".

    /// Geometry, neon and lamp settings for @ref ApplyPictureLight, all in px
    /// at the reference bar length. Everything pixel-valued is scaled by
    /// @c barW / PL_REF_BAR_WIDTH at apply time, so the rig stays in
    /// proportion at any viewport size.
    ///
    /// The reference numbers came off an offscreen sweep against a photograph
    /// of a brass LED picture light; see the block comment on
    /// @ref ApplyPictureLight for what each one is doing.
    namespace PictureLight
    {
        constexpr float REF_BAR_WIDTH = 580.0f; ///< Bar length the numbers below were tuned at.

        constexpr float BAR_WIDTH_FRAC = 0.45f;   ///< Bar length as a fraction of the viewport.
        constexpr float TUBE_DIA_FRAC = 0.021f;   ///< Tube diameter as a fraction of the bar length.
        constexpr float BAR_TOP_FRAC = 0.167f;    ///< Bar's y as a fraction of viewport height.

        // --- The tube (NeonRenderer) ---

        /// Height of the rect the neon traces. The tube is a ROD, so this is
        /// deliberately tiny: it collapses the rect to a single straight run
        /// whose top and bottom perimeter edges coincide, and the rod's
        /// diameter comes from @c NeonConfig::lineWidth instead.
        ///
        /// Give the rect a real height and the two edges separate into a
        /// HOLLOW CAPSULE OUTLINE - two parallel filaments with a dark
        /// channel between them, closed by a U-turn at each cap. That is a
        /// loop of neon tubing, not an LED bar. Measured on the version that
        /// had it, the cross-section through the tube read
        /// 219, 218, 211, 200, 193, 193, 198, 209, 224: a dip of 43/255 in
        /// the middle of what should be the brightest part.
        ///
        /// NOT scaled by the bar: the requirement is only that it stay well
        /// under @c lineWidth, which does scale, so a fixed epsilon gets
        /// relatively safer as the fixture grows and still merges at the
        /// smallest bar this preset will build.
        ///
        /// Not 0, though 0 renders correctly today (measured: same profile,
        /// same peak, no NaN). A zero-extent rect is a degenerate input
        /// @ref EdgeLighting::RectGeometry makes no promise about, and 1 px
        /// is invisible against any @c lineWidth this preset produces.
        constexpr float ROD_RECT_HEIGHT = 1.0f;

        /// Flatter than the default, because the rod wants a defined edge:
        /// a real tube is a physical object with a boundary, not a smear.
        /// Below ~2 it blends into its own halo; above ~3 the shoulder goes
        /// hard and it reads as a painted stripe.
        constexpr float FILAMENT_FALLOFF = 2.5f;
        constexpr float NEON_INTENSITY = 0.80f;
        constexpr float GLOW_RADIUS = 15.0f;
        constexpr float BLOOM_STRENGTH = 0.10f;

        // --- The wash (SpotlightRenderer) ---
        constexpr int LAMP_COUNT = SPOT_MAX_LIGHTS;
        constexpr float BEAM_ANGLE = 30.0f;
        constexpr float THROW_LENGTH = 280.0f;
        constexpr float LAMP_INTENSITY = 0.70f;
        constexpr float COLOR_TEMP = 2900.0f;

        /// Lamp aperture half-width as a fraction of the lamp SPACING, which
        /// is the ratio that decides whether the row reads as one pool or as
        /// a string of beads. Measured at this value, comparing INTERIOR
        /// lamps against the midpoints between them (the two end lamps have
        /// one neighbour instead of two, so including them measures the
        /// row's edge taper rather than its periodic ripple): at most 0.9%
        /// of full scale, and under 0.2% over most of the throw. Lower it
        /// and the scalloping becomes visible.
        constexpr float APERTURE_PER_SPACING = 0.61f;

        /// Broadest gaussian cross-section. Not a taste setting: the row only
        /// merges into one pool because each lamp reaches its neighbours.
        constexpr float SOFTNESS = 1.0f;

        /// ~2900 K tungsten white, as linear RGB for the neon colour stop.
        /// The lamps reach the same colour through @c COLOR_TEMP instead,
        /// since @ref EdgeLighting::SpotLight bakes blackbody on the CPU.
        constexpr float TUBE_R = 1.00f;
        constexpr float TUBE_G = 0.72f;
        constexpr float TUBE_B = 0.45f;
    } // namespace PictureLight

    /// Imitate a linear LED picture light: a warm bar mounted high on the
    /// wall, throwing a broad short wash down over whatever hangs below it.
    ///
    /// TWO LAYERS, because the fixture is two things at once:
    ///
    ///   The TUBE is @ref EdgeLighting::NeonRenderer. @c geometry becomes the
    ///   bar's AXIS - a rect @c ROD_RECT_HEIGHT tall, i.e. a straight run
    ///   rather than a shape with an interior - and @c lineWidth gives that
    ///   run the tube's diameter, so the filament IS a solid rod with round
    ///   caps. See @c ROD_RECT_HEIGHT for why the rect is not a stadium.
    ///   The rod's symmetric halo is also the soft spill on the wall ABOVE
    ///   the fixture, which a real one has.
    ///
    ///   The WASH is @ref EdgeLighting::SpotlightRenderer, as a ROW of lamps
    ///   rather than one. A picture light is a LINE source: a single cone
    ///   reads as a triangle, where a bar throws a flat-topped pool that only
    ///   tapers well below the fixture. @c LAMP_COUNT lamps spread along the
    ///   bar, each with @c bloom 0 (the neon halo already owns the near
    ///   field, and a bloom per lamp would read as a string of beads).
    ///
    /// WHAT IT TURNS OFF: droplets, the lens flare, and the three debug
    /// overlays. A wireframe box around a 12 px tube, or rain falling through
    /// the beam, would both just be noise over the scene this is showing.
    /// Arcs and segments are reset too, so the tube lights evenly.
    ///
    /// WHAT IT CANNOT DO, and no setting here changes: nothing in this
    /// library occludes light. There is no shadow under the frame's top rail
    /// and no shading on its bevel, because a cone crosses geometry freely.
    /// The falloff is screen-space 2D, not a 3D light on a surface, so this
    /// is a head-on view only.
    ///
    /// @param cfg        Rewritten in place.
    /// @param viewportW  Viewport width in px - the same space as
    ///                   @c RectGeometry::position, i.e. what the demo passes
    ///                   to @ref EdgeLighting::EdgeLightingEffect::Render.
    /// @param viewportH  Viewport height in px.
    inline void ApplyPictureLight(EdgeLighting::Config &cfg,
                                  float viewportW, float viewportH)
    {
        using namespace EdgeLighting;
        namespace PL = PictureLight;

        const float barW = std::max(viewportW * PL::BAR_WIDTH_FRAC, 64.0f);
        const float tubeDia = std::max(barW * PL::TUBE_DIA_FRAC, 4.0f);
        const float barX = (viewportW - barW) * 0.5f;
        const float barY = viewportH * PL::BAR_TOP_FRAC;

        // Every px-valued number below was tuned at REF_BAR_WIDTH. Scaling by
        // the bar keeps the wash in proportion to the fixture instead of
        // shrinking against it on a larger viewport.
        const float s = barW / PL::REF_BAR_WIDTH;

        // --- Geometry: the rect is the tube's AXIS, not its outline ---
        // Offset y so the rod's axis lands where a tubeDia-tall bar's centre
        // line would, which keeps the rod occupying barY .. barY + tubeDia
        // and leaves the lamp row below it unchanged.
        cfg.geometry.width = barW;
        cfg.geometry.height = PL::ROD_RECT_HEIGHT;
        cfg.geometry.position = glm::vec2(barX, barY + (tubeDia - PL::ROD_RECT_HEIGHT) * 0.5f);
        cfg.geometry.cornerRadius = PL::ROD_RECT_HEIGHT * 0.5f;

        // --- The tube ---
        cfg.neon.enable = true;
        cfg.neon.opaqueMode = OpaqueMode::NONE;
        // The rod's diameter. Already proportional to the bar, so it does
        // NOT take the s scale a second time.
        cfg.neon.lineWidth = tubeDia;
        cfg.neon.filamentFalloff = PL::FILAMENT_FALLOFF;
        cfg.neon.intensity = PL::NEON_INTENSITY;
        cfg.neon.glowRadius = PL::GLOW_RADIUS * s;
        cfg.neon.bloomStrength = PL::BLOOM_STRENGTH;
        cfg.neon.glowSide = GlowSide::BOTH;
        cfg.neon.insideCutoff = {false, 0.0f, 0.0f};
        cfg.neon.outsideCutoff = {false, 0.0f, 0.0f};
        // A lamp does not cycle hue, and the default rate is 0.5.
        cfg.neon.hueRotationRate = 0.0f;
        cfg.neon.colorStops = {{0.0f, glm::vec4(PL::TUBE_R, PL::TUBE_G, PL::TUBE_B, 1.0f)}};
        // Full perimeter, evenly lit: an arc slice or a travelling segment
        // left over from another scene would put a bright spot on the tube.
        cfg.neon.arcs = {Arc{}};
        cfg.neon.segmentBoosts.clear();

        // --- The wash ---
        cfg.spotlight.enable = true;
        cfg.spotlight.resolutionScale = 1.0f;
        cfg.spotlight.lights.clear();
        cfg.spotlight.lights.reserve(static_cast<size_t>(PL::LAMP_COUNT));

        const float spacing = barW / static_cast<float>(PL::LAMP_COUNT);
        for (int i = 0; i < PL::LAMP_COUNT; i++)
        {
            SpotLight l;
            // Half-spacing in from each end, so the row is centred on the bar
            // and no lamp sits on a cap.
            l.position = glm::vec2(barX + (static_cast<float>(i) + 0.5f) * spacing,
                                   barY + tubeDia);
            l.angle = 90.0f; // straight down
            l.beamAngle = PL::BEAM_ANGLE;
            l.throwLength = PL::THROW_LENGTH * s;
            l.apertureWidth = spacing * PL::APERTURE_PER_SPACING;
            l.softness = PL::SOFTNESS;
            l.intensity = PL::LAMP_INTENSITY;
            l.bloom = 0.0f;
            l.bloomRadius = 20.0f; // unused while bloom is 0; left at the default
            l.colorTemp = PL::COLOR_TEMP;
            l.tint = glm::vec3(1.0f, 1.0f, 1.0f);
            l.enable = true;
            cfg.spotlight.lights.push_back(l);
        }

        // --- Layers this fixture does not use ---
        cfg.droplets.enable = false;
        cfg.lensFlare.enable = false;
        cfg.debug.showWireframe = false;
        cfg.debug.showGradientLUT = false;
        cfg.debug.showColorStops = false;
    }

} // namespace EdgeLightingDemo

#endif
