#include "renderer/spotlight-renderer.h"
#include "renderer/spotlight-tuning.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include "shaders.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace EdgeLighting
{
    namespace
    {
        // StripVertex lives in the header, next to the mStripVerts member it
        // has to type, along with the packing static_assert that used to sit
        // here.

        constexpr int VERTS_PER_LAMP = SPOT_STRIP_SEGMENTS * 6;
        constexpr int MAX_VERTS = SPOT_MAX_LAMPS * VERTS_PER_LAMP;
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;


        /// Floor under SpotlightConfig::resolutionScale. Below this the blit
        /// is reading so few texels that the light turns to blocks, and the
        /// strips are already the cheap part.
        constexpr float MIN_RESOLUTION_SCALE = 0.125f;

        /// Whether any lamp this pass will actually DRAW honours the clip area.
        ///
        /// Gates the resolution scale - see @ref GetClampedSpotScale. Counts
        /// only lamps that draw, because a lamp that writes no fragments puts
        /// no clip boundary in the buffer whatever its clip bit says. The test
        /// is deliberately the same @c enable && @c intensity > 0 that
        /// @ref SpotlightRenderer::buildStrips uses for its @c drawnLamps count,
        /// rather than a second copy of @ref DeriveLamp's black-tint bail: a
        /// lamp the tint switches off would be counted here and only cost the
        /// scale, which is the harmless direction.
        ///
        /// Indexed like everything else in this renderer, so entries past
        /// @c SPOT_MAX_LAMPS do not count - they never reach the VBO.
        bool HasClippedLamp(const SpotlightConfig &spotlight)
        {
            const int lampCount = std::min(static_cast<int>(spotlight.lamps.size()),
                                           static_cast<int>(SPOT_MAX_LAMPS));
            for (int i = 0; i < lampCount; i++)
            {
                const SpotLight &lamp = spotlight.lamps[static_cast<size_t>(i)];
                if (lamp.clipped && lamp.enable && lamp.intensity > 0.0f)
                {
                    return true;
                }
            }
            return false;
        }

        /// @c SpotlightConfig::resolutionScale clamped to (0, 1] - and PINNED
        /// to 1.0 while any drawn lamp is clipped.
        ///
        /// WHY THE CLIP DISABLES THE SCALE. spotlight.frag evaluates the clip
        /// mask in whatever buffer it is rasterising into, so on the scaled
        /// path the boundary is resolved at THAT buffer's texel pitch and the
        /// blit then bilinearly smears the result back to full resolution. The
        /// mask's geometry is in app coordinates and survives that untouched;
        /// its EDGE does not. Measured across a KEEP_INSIDE boundary crossing
        /// bright light, in 1/255, by destination pixel from the boundary:
        ///
        ///   offset   scale 1.0   scale 0.5   scale 0.25
        ///     -1        255         255         191
        ///      0        183         128         128
        ///     +1          0           0          64
        ///     +2          0          28 (*)       0
        ///                              (*) edgeSoftness 4
        ///
        /// Three things wrong with the right-hand columns: the boundary moves
        /// by up to a destination pixel, light leaks up to 64/255 OUTSIDE an
        /// area whose whole job is to stop it, and @c ClipArea::edgeSoftness
        /// loses its range once a feather is finer than a buffer texel - at
        /// 0.25, softness 1 and softness 4 render identically.
        ///
        /// This is the failure neon-blit.frag records at length, and the neon
        /// fixed it by moving its one-sided cut out of the reduced buffer into
        /// the full-res blit. That fix does not transfer: @c SpotLight::clipped
        /// is PER LAMP, while the buffer this pass blits holds every lamp's
        /// light summed together, so a mask applied at blit time would cut the
        /// lamps that opted out along with the ones that opted in. Separating
        /// them means a second buffer and a second blit - and the blit is
        /// already the fixed cost that makes this scale a marginal bargain
        /// (see @c SpotlightConfig::resolutionScale), so paying it twice would
        /// leave nothing to win.
        ///
        /// Pinning is therefore the honest trade: the scale is a performance
        /// knob whose benefit this layer only sees on a large rig, and the clip
        /// is a correctness statement about where light stops. A host loses
        /// speed it was unlikely to be gaining, and keeps the edge it asked
        /// for. @ref SpotlightRenderer::OnConfigChanged logs the transition so
        /// the loss is never silent.
        float GetClampedSpotScale(const Config &config)
        {
            if (HasClippedLamp(config.spotlight))
            {
                return 1.0f;
            }

            return std::min(std::max(config.spotlight.resolutionScale, MIN_RESOLUTION_SCALE), 1.0f);
        }

        /// Whether this config gives @c mScaledBuffer anything to do.
        ///
        /// ONE predicate for two questions that have to agree: @ref Render asks
        /// it to pick the path, and @ref OnConfigChanged asks it to decide
        /// whether to release the buffer. Split them and a config that stops
        /// using the buffer can leave it allocated forever, or - worse - one
        /// that still needs it can have it freed underneath.
        ///
        /// That it goes through @ref GetClampedSpotScale rather than reading
        /// @c resolutionScale is what makes the clip pin free here: opting a
        /// lamp into the clip area takes this to false and hands the buffer
        /// back, with no second place to teach about it.
        bool UsesScaledBuffer(const Config &config)
        {
            return config.spotlight.enable && GetClampedSpotScale(config) < 1.0f;
        }

        /// Sub-samples per half-interval when widening a strip sample to the
        /// support's maximum over the span its chords cover. CPU-only, and it
        /// buys a tighter guarantee rather than a different image.
        ///
        /// NOT cheap relative to the rest of the solve, which an earlier
        /// version of this comment claimed. At 8 it is 208 SupportAt
        /// evaluations per lamp against 13 for the sampling it corrects -
        /// about 88% of buildStrips' transcendental calls, and the first thing
        /// to lower if the solve ever shows up in a profile. It is still only
        /// a few microseconds of a ~20 us rebuild (see buildStrips' comment),
        /// which is why it is set where the guarantee is comfortable rather
        /// than where the cost is.
        constexpr int WIDEN_SUBSAMPLES = 8;

        /// Blackbody colour anchors, linearly interpolated between. Local to
        /// this renderer because it is the only consumer - the same split
        /// droplets-tuning.h's comment describes for one-consumer constants.
        ///
        /// Free to change. It used to carry an invariant the strip solve
        /// depended on - every adjacent pair sharing a channel at exactly 1.0,
        /// pinned by a static_assert - because the solve bounded a scalar with
        /// no colour in it. @c DeriveLamp now folds the brightest channel into
        /// @c LampSolve::solveIntensity instead, which is what makes
        /// @c SpotLight::tint possible, so any anchors at all are safe here.
        typedef struct KelvinAnchor
        {
            float k, r, g, b;
        } KelvinAnchor;

        constexpr KelvinAnchor KELVIN_TABLE[] = {
            {1800.0f, 1.00f, 0.55f, 0.22f},
            {2400.0f, 1.00f, 0.65f, 0.34f},
            {2700.0f, 1.00f, 0.71f, 0.43f},
            {3200.0f, 1.00f, 0.79f, 0.58f},
            {4000.0f, 1.00f, 0.87f, 0.75f},
            {5000.0f, 1.00f, 0.94f, 0.90f},
            {6500.0f, 1.00f, 1.00f, 1.00f},
            {8000.0f, 0.91f, 0.95f, 1.00f},
        };

        constexpr int KELVIN_COUNT = static_cast<int>(sizeof(KELVIN_TABLE) / sizeof(KELVIN_TABLE[0]));

        glm::vec3 KelvinToRgb(float kelvin)
        {
            if (kelvin <= KELVIN_TABLE[0].k)
            {
                return glm::vec3(KELVIN_TABLE[0].r, KELVIN_TABLE[0].g, KELVIN_TABLE[0].b);
            }

            for (int i = 1; i < KELVIN_COUNT; i++)
            {
                if (kelvin > KELVIN_TABLE[i].k)
                {
                    continue;
                }

                const KelvinAnchor &a = KELVIN_TABLE[i - 1];
                const KelvinAnchor &b = KELVIN_TABLE[i];
                const float t = (kelvin - a.k) / (b.k - a.k);
                return glm::vec3(a.r + (b.r - a.r) * t,
                                 a.g + (b.g - a.g) * t,
                                 a.b + (b.b - a.b) * t);
            }

            const KelvinAnchor &last = KELVIN_TABLE[KELVIN_COUNT - 1];
            return glm::vec3(last.r, last.g, last.b);
        }

        /// Everything the solve and the upload need, derived once per lamp.
        typedef struct LampSolve
        {
            float tanHalf; ///< tan(beamAngle / 2)
            float throwLength; ///< SpotLight::throwLength, floored
            float softK;   ///< gaussian exponent across the beam
            float apertureWidth; ///< SpotLight::apertureWidth, floored
            float intensity;
            float bloom;
            float bloomRadius;
            float bloomWindow; ///< Where spotlight.frag's window closes.
            float bloomReach;  ///< Where the bloom actually stops mattering:
                               ///< min(@ref bloomWindow, the inverse-square
                               ///< core's own visibility limit).
            float visibilityFloor; ///< This lamp's share of the half-step budget.
            /// @c intensity scaled by the BRIGHTEST channel of @c color.
            ///
            /// Everything that bounds geometry reads this; only the shader
            /// reads @c intensity. They are separate because spotlight.frag
            /// writes `color * intensity * (cone + bloom)` while the solve
            /// works on a scalar, so the scalar it works on has to be the
            /// largest value any channel will actually reach. See the note in
            /// @ref DeriveLamp.
            float solveIntensity;
            glm::vec3 color;
        } LampSolve;

        /// Half-width of the CONE's support at @p a, in the lamp's frame.
        ///
        /// This inverts spotlight.frag's falloff. Taking logs of
        ///
        ///   solveIntensity * exp(-a / throwLength) * (apertureWidth / halfW)
        ///       * exp(-lat^2 * softK)
        ///       >= SPOT_VISIBILITY_FLOOR
        ///
        /// and solving for lat gives the expression below. `smoothstep`'s
        /// near-end fade is not inverted - it only ever REDUCES the term, and
        /// the strip's start is bounded separately and conservatively.
        ///
        /// @c solveIntensity, not @c intensity: it already carries the
        /// brightest channel of the lamp's colour, which is what keeps this
        /// bound right for a tinted lamp. See @ref LampSolve::solveIntensity.
        ///
        /// Returns 0 where the cone cannot reach the visibility floor at all.
        float SolveConeAcross(const LampSolve &s, float a)
        {
            const float alongPos = std::max(a, 0.0f);
            const float halfW = s.apertureWidth + alongPos * s.tanHalf;
            const float headroom = std::log(s.solveIntensity) - std::log(s.visibilityFloor) -
                                   alongPos / s.throwLength -
                                   std::log(halfW / s.apertureWidth);
            if (headroom <= 0.0f)
            {
                return 0.0f;
            }

            return std::sqrt(headroom / s.softK) * halfW;
        }

        /// Furthest the cone reaches along its axis.
        ///
        /// @ref SolveConeAcross's headroom is strictly decreasing in @c a for
        /// a >= 0 (both the throw term and the spread term only ever grow), so
        /// the crossing is a bisection. Bracketed by doubling from the throw
        /// length, with a hard cap: a lamp bright enough to need more than
        /// 1e5 px has bigger problems than a loose bound.
        float SolveConeReach(const LampSolve &s)
        {
            if (SolveConeAcross(s, 0.0f) <= 0.0f)
            {
                return 0.0f;
            }

            float hi = s.throwLength;
            while (hi < 1.0e5f && SolveConeAcross(s, hi) > 0.0f)
            {
                hi *= 2.0f;
            }

            float lo = 0.0f;
            while (hi - lo > 1.0f)
            {
                const float mid = (lo + hi) * 0.5f;
                if (SolveConeAcross(s, mid) > 0.0f)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }

            return hi;
        }

        /// Half-width of the WHOLE support at @p a: the cone, UNIONED with the
        /// aperture bloom's disc. The +1 is a rasterisation safety margin, the
        /// same spirit as droplets-renderer.cpp's SAFETY_PX.
        ///
        /// UNION, but spotlight.frag ADDS - `cone + bloom`. So what this
        /// bounds is "each term alone is under the floor", which is not quite
        /// "their sum is". Where the cone's lateral boundary crosses the
        /// bloom's disc, both terms can sit just below the floor and add to
        /// just above it, and a pixel in that thin region can round to 1/255
        /// outside the strip.
        ///
        /// Left as a union rather than solved against floor/2 per term, which
        /// would close it exactly, because two things already cover it and the
        /// tighter bound costs geometry everywhere to buy it:
        ///
        ///   - the +1 px margin. At the cone boundary the gaussian falls by
        ///     roughly a factor of two per pixel for a typical lamp, so the
        ///     drawn edge already sits where the cone is about half the floor.
        ///   - the bloom's window reaches exactly zero at `bloomWindow`, not
        ///     merely "under the floor". Wherever the disc is window-limited -
        ///     which is the common case, and always so for a strong bloom -
        ///     its term is 0 at the boundary and there is nothing to add.
        ///
        /// Measured over fourteen single-lamp scenes at 1280x720 against a CPU
        /// evaluation of the shader: at most 5 clipped channels out of 2.76
        /// million, every one at value 1, which is the rounding coin-flip at
        /// the boundary rather than this gap. See I17 in
        /// docs/review-findings.md.
        float SupportAt(const LampSolve &s, float a)
        {
            float c = SolveConeAcross(s, a);
            if (s.bloomReach > 0.0f && std::fabs(a) < s.bloomReach)
            {
                c = std::max(c, std::sqrt(s.bloomReach * s.bloomReach - a * a));
            }
            return c + 1.0f;
        }

        /// Build the per-lamp derived values, or report the lamp draws nothing.
        ///
        /// @p visibilityFloor is this lamp's share of the half-8-bit-step budget - see
        /// SPOT_VISIBILITY_FLOOR. Everything the solve cuts is below it, so the
        /// whole rig's clipped remainder stays under one half step.
        bool DeriveLamp(const SpotLight &lamp, float visibilityFloor, LampSolve &out)
        {
            if (!lamp.enable || lamp.intensity <= 0.0f)
            {
                return false;
            }

            // Clamped to 170 so the half-angle stays at or under 85 and tan
            // stays finite: a lamp is a cone, and a "cone" at 180 degrees is
            // a half-plane with no axis left to speak of. The 10 degrees of
            // headroom under that limit is what keeps tan away from the knee
            // where it stops being a useful number rather than merely finite.
            const float beam = std::min(std::max(lamp.beamAngle, 0.0f), 170.0f);
            out.tanHalf = std::max(std::tan(beam * 0.5f * DEG_TO_RAD), 0.0f);
            out.throwLength = std::max(lamp.throwLength, static_cast<float>(SPOT_MIN_THROW));
            out.apertureWidth = std::max(lamp.apertureWidth, static_cast<float>(SPOT_MIN_APERTURE));
            out.intensity = lamp.intensity;
            out.bloom = std::max(lamp.bloom, 0.0f);
            out.bloomRadius = std::max(lamp.bloomRadius, 1.0f);
            out.color = KelvinToRgb(lamp.colorTemp) * lamp.tint;
            out.visibilityFloor = visibilityFloor;

            // WHY THE SOLVE GETS ITS OWN INTENSITY.
            //
            // spotlight.frag writes `color * intensity * (cone + bloom)`; the
            // solve below bounds a scalar. Folding the brightest channel of
            // the colour into that scalar makes the bound EXACT for whichever
            // channel reaches furthest and conservative for the other two,
            // whatever the colour is - which is what lets SpotLight::tint be
            // an arbitrary linear RGB value, above 1 included.
            //
            // Without the fold this would only be sound while every colour
            // KelvinToRgb can return has a channel at exactly 1, which is true
            // of KELVIN_TABLE and was once pinned by a static_assert over it.
            // A tint breaks that invariant in both directions: a dim tint
            // makes the strip larger than it needs to be (wasteful), and a
            // tint above 1 makes it CLIP the brightest channel (a straight
            // edge across the dim tail, with nothing in the log). The fold
            // handles both, and subsumes the assert it replaced.
            const float maxChannel = std::max(std::max(out.color.r, out.color.g), out.color.b);
            out.solveIntensity = out.intensity * maxChannel;

            // A black tint is as much an off switch as intensity 0, and has to
            // be caught here: std::log(0) is -inf, which would propagate
            // through every headroom the solve computes. Bail before the
            // arithmetic rather than draw a degenerate strip for a lamp that
            // writes nothing.
            if (out.solveIntensity <= 0.0f)
            {
                return false;
            }

            const float softness = std::min(std::max(lamp.softness, 0.0f), 1.0f);
            out.softK = static_cast<float>(SPOT_SOFT_MIN) +
                        (1.0f - softness) *
                            static_cast<float>(SPOT_SOFT_MAX - SPOT_SOFT_MIN);

            out.bloomWindow = out.bloomRadius * static_cast<float>(SPOT_BLOOM_WINDOW_OUTER);

            // The bloom's own support is the TIGHTER of two bounds: where
            // spotlight.frag's window closes, and where the inverse-square
            // core itself drops under the visibility floor. Solving
            //   intensity * bloom * r^2 / (d^2 + r^2) = floor
            // for d gives the second. Taking the min is right for this TERM -
            // past the window it is exactly zero, past the visible bound it is
            // under the floor.
            //
            // For the term. Not for the fragment, which also carries the cone:
            // see the note on SupportAt for where the two bounds meet and what
            // that is worth in practice.
            out.bloomReach = 0.0f;
            const float peak = out.solveIntensity * out.bloom;
            if (peak > out.visibilityFloor)
            {
                const float ratio = peak / out.visibilityFloor - 1.0f;
                const float visible = out.bloomRadius * std::sqrt(std::max(ratio, 0.0f));
                out.bloomReach = std::min(out.bloomWindow, visible);
            }

            return true;
        }

        /// @c ClipArea resolved into the form both the shader uniforms
        /// and the strip solve want: a CENTRED box with a clamped radius, plus
        /// the one flag that says whether any of it applies.
        ///
        /// Derived once per rebuild and once per frame rather than per lamp,
        /// because the area is shared across the whole rig - the only per-lamp
        /// part of the clip is @c SpotLight::clipped, which is a bit.
        typedef struct ClipSolve
        {
            bool keepInside;  ///< Which side of the area survives.
            glm::vec2 center; ///< App px.
            glm::vec2 half;   ///< Half extent, app px, never negative.
            float radius;     ///< Corner radius, clamped to the shorter half extent.
            float softness;   ///< Feather width in px, never negative.
        } ClipSolve;

        /// Derive the clip area. ONE function for two callers that must not
        /// disagree - @ref SpotlightRenderer::Render uploads it, and
        /// @ref SpotlightRenderer::buildStrips bounds geometry against it -
        /// for the same reason @ref UsesScaledBuffer is one predicate.
        ///
        /// A degenerate area (zero or negative width or height) is derived
        /// like any other rather than being treated as "no clip", and that is
        /// deliberate: under KEEP_INSIDE it is the correct answer that a
        /// clipped lamp writes nothing, and quietly ignoring it would light
        /// the whole beam instead - the opposite of what a host that set width
        /// to 0 asked for. The half extent is floored at zero so the SDF stays
        /// well-formed either way.
        ///
        /// Nothing here says whether the clip APPLIES. @c SpotLight::clipped
        /// alone decides that, per lamp; there is no area-level enable to
        /// agree with.
        ClipSolve DeriveClip(const ClipArea &area)
        {
            ClipSolve out{};
            out.keepInside = (area.mode == ClipMode::KEEP_INSIDE);
            out.half = glm::vec2(std::max(area.width, 0.0f) * 0.5f,
                                 std::max(area.height, 0.0f) * 0.5f);
            out.center = area.position + out.half;
            // Clamped here rather than in the shader so sdRoundBox can take
            // the radius on trust: a radius past the shorter half extent turns
            // `abs(p) - b + r` inside out and the box stops being a box.
            out.radius = std::min(std::max(area.cornerRadius, 0.0f),
                                  std::min(out.half.x, out.half.y));
            out.softness = std::max(area.edgeSoftness, 0.0f);
            return out;
        }

        /// Warn when a host hands over more lamps than one pass can draw.
        /// @ref SpotlightRenderer::buildStrips clamps the list with
        /// @c std::min against @c SPOT_MAX_LAMPS and everything past it never
        /// reaches the VBO, so without this the excess disappears with no log
        /// line and no result code. Both demo UIs cap their Add button, so
        /// neither of them can see it; a library host writing
        /// @c SpotlightConfig::lamps directly, or a C-ABI host calling
        /// @c el_effect_set_spotlight_count, can.
        ///
        /// The truncation itself is INTENDED - `config.h` documents a longer
        /// list as a legitimate way to keep indices (and the animation
        /// bindings that address them) stable while enabling a subset. This
        /// only says which end of the list wins.
        ///
        /// Counted over the whole list, not the enabled lamps: the clamp is by
        /// INDEX, so a disabled entry at slot 3 still costs slot 3.
        ///
        /// Fires on the TRANSITION into overflow - @p prev at or under the cap,
        /// @p now above it - which keeps it to one line per overflow with no
        /// latch to store, exactly as @c WarnOnOverflow does in
        /// neon-renderer.cpp. Two arguments rather than that one's four because
        /// there is a single cap and a single kind of entry here.
        inline void WarnOnLampOverflow(size_t prev, size_t now)
        {
            if (static_cast<int>(now) > SPOT_MAX_LAMPS &&
                static_cast<int>(prev) <= SPOT_MAX_LAMPS)
            {
                LOG_E("SpotlightRenderer: %zu lamps configured but only %d fit - "
                      "the rest are ignored.",
                      now, static_cast<int>(SPOT_MAX_LAMPS));
            }
        }

        /// Say so when a clipped lamp takes @c resolutionScale out of effect,
        /// and when it comes back. @ref GetClampedSpotScale explains why it
        /// has to; this is what stops the host's setting being overridden in
        /// silence, which is the one thing that would make the pin feel like a
        /// bug rather than a trade.
        ///
        /// Fires on the TRANSITION in either direction, the same shape as
        /// @ref WarnOnLampOverflow, so a rig that drags a lamp around under a
        /// clip does not print a line per frame. Informational, not an error:
        /// nothing is wrong and nothing is being dropped.
        ///
        /// Takes both sub-configs rather than two bools so the caller cannot
        /// compute "pinned" two different ways - the predicate is
        /// @ref GetClampedSpotScale's, applied to each of them here.
        inline void LogOnClipPinTransition(const SpotlightConfig &prev,
                                           const SpotlightConfig &now)
        {
            // A scale of 1.0 is what the pin produces anyway, so a config that
            // never asked for less has no transition to report either way.
            if (now.resolutionScale >= 1.0f && prev.resolutionScale >= 1.0f)
            {
                return;
            }

            const bool wasPinned = HasClippedLamp(prev);
            const bool isPinned = HasClippedLamp(now);
            if (isPinned == wasPinned)
            {
                return;
            }

            if (isPinned)
            {
                LOG_I("SpotlightRenderer: resolutionScale %.3f held at 1.0 while a "
                      "clipped lamp is enabled - the clip edge has to be resolved at "
                      "full resolution.",
                      static_cast<double>(now.resolutionScale));
            }
            else
            {
                LOG_I("SpotlightRenderer: no clipped lamp left - resolutionScale %.3f "
                      "back in effect.",
                      static_cast<double>(now.resolutionScale));
            }
        }
    }

    bool SpotlightRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link SpotlightRenderer shaders.");
            return false;
        }

        setupBlitGeometry();

        // mCurrentSpotlight is whatever the last OnConfigChanged left - the
        // effect calls it on registration, so by here it is usually the host's
        // real config rather than the defaults. Either way the strips exist
        // from this point on, and OnConfigChanged rebuilds them on every
        // change.
        buildStrips(mCurrentSpotlight);
        return true;
    }

    void SpotlightRenderer::Update(float, float, const Config &)
    {
    }

    void SpotlightRenderer::Render(int viewportWidth, int viewportHeight, float, const Config &config)
    {
        if (!config.spotlight.enable || mVertexCount <= 0 ||
            viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        const float scale = GetClampedSpotScale(config);
        const bool scaled = UsesScaledBuffer(config);
        const int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        const int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // The render target this renderer was handed - framebuffer AND
        // viewport, saved as a pair because the blit has to put both back. The
        // framebuffer is not always the window's: an offscreen frame capture
        // binds a real FBO, so returning to 0 would redirect the composite to
        // the window. Read BEFORE the resize below.
        //
        // SCALED PATH ONLY, because it is the only one that retargets. Same
        // shape, same reasoning, as LensFlareRenderer::Render.
        RenderTargetState prevTarget;

        // SCALED PATH ONLY: mScaledBuffer is a reduced-size copy of the
        // viewport, so a host scissor box - in the CALLER's window coordinates
        // - lands on the wrong texels of it. See GLUtils::NoScissorScope.
        GLUtils::NoScissorScope noScissor(scaled);

        if (scaled)
        {
            prevTarget = RenderTargetState::Capture();

            // Resize destroys the attachment on its failure path, so a failure
            // would leave mScaledBuffer holding id 0 and Bind would then bind
            // the CALLER'S framebuffer - with ClearBuffer about to wipe it.
            // Bail instead: nothing has been drawn, and noScissor puts the
            // scissor enable back as this return unwinds.
            if (!mScaledBuffer.Resize(bufW, bufH))
            {
                return;
            }
            // Bind, then clear to transparent black. Keep the two adjacent:
            // ClearBuffer acts on whatever is BOUND.
            mScaledBuffer.Bind();
            mScaledBuffer.ClearBuffer();
        }

        // A y-FLIPPED ortho: note bottom and top are swapped relative to the
        // one the other renderers build. That is what lets the VBO hold app
        // coordinates verbatim - top-left origin, +y down, the same space as
        // Config::geometry.position - instead of framebuffer coordinates, so
        // the buffer depends only on Config::spotlight and a resize costs this
        // uniform rather than a rebuild.
        //
        // The flip reverses triangle winding, which used to be written off
        // here as harmless because nothing in THIS library enables
        // GL_CULL_FACE. That was the wrong half of the question - the host owns
        // that state, and on a shared surface view so does whatever else draws
        // into the context. buildStrips winds for the flip, so the strips come
        // out front-facing under the default GL_CCW / GL_BACK configuration
        // like every other layer, rather than being the one piece of geometry
        // a culling host deletes; see it for the measurement. The other ways a
        // host can cull (GL_FRONT, a GL_CW front face) are not covered anywhere
        // in this library, so they are the caller's to settle before it calls
        // Render. No other handedness has to be kept in step, since the
        // fragment stage reads no gl_FragCoord.
        const glm::mat4 mvp = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                         static_cast<float>(viewportHeight), 0.0f,
                                         -1.0f, 1.0f);

        // FULLY ADDITIVE, colour and alpha alike, and the only separate-alpha
        // blend in the library. Both halves are deliberate.
        //
        // COLOUR is GL_ONE / GL_ONE because light only ever adds - which also
        // makes the pass order-independent, so the lamp order in the config
        // cannot change the image. That is unchanged behaviour: this used to
        // be GL_ONE / GL_ONE_MINUS_SRC_ALPHA against a shader that emitted a
        // literal alpha 0, and 1 - 0 is 1, so the destination factor was
        // always exactly GL_ONE anyway. Writing it out is what lets
        // spotlight.frag start emitting a real alpha without the colour
        // channel quietly acquiring an occlusion term the other layers have
        // and this one should not.
        //
        // ALPHA is GL_ONE / GL_ONE because the framebuffer's alpha has to end
        // up saying "there is light here". It did not before. Every other
        // layer writes a coverage alpha; the spotlight wrote 0 and so left the
        // surface transparent wherever it was the only thing that drew. On a
        // desktop window that is invisible - the window is opaque and nobody
        // reads the alpha back. On an embedded surface it is fatal: a
        // compositor or hardware video plane finishes the frame with
        // `out = ui.rgb * ui.a + video * (1 - ui.a)`, and every lit spotlight
        // pixel is multiplied by zero. That is the Tizen report - the layer
        // missing over a playing video while neon, droplets and the flare
        // (all of which write coverage) came through.
        //
        // Measured offscreen at 640x360 over a transparent clear, before the
        // fix: one lamp lit 72,615 pixels of colour and 0 pixels of alpha, and
        // composited to nothing at all.
        //
        // Accumulating the alpha rather than compositing it keeps the
        // order-independence the colour has: N lamps overlapping sum their
        // coverage and saturate at the framebuffer, whatever order they are
        // in.
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);

        mShaderProgram.Use();

        // NOT ONE UNIFORM differs between the two paths - fewer than the
        // flare's two, and none at all. The ortho is over APP coordinates, and
        // the viewport transform alone carries the scale: the same vertex
        // lands at scale * its app pixel in a scale-sized buffer. The fragment
        // stage then reads only vLocal, which interpolates in full-res lamp
        // pixels, against flat per-lamp values that are full-res pixels too -
        // so it never learns, and never needs to learn, which buffer it is
        // shading into. That is what makes 1.0 bit-identical.
        mShaderProgram.SetUniform("uMVP", mvp);

        // The clip area, in the SAME app coordinates the ortho above maps -
        // which is what keeps the sentence two comments up true. A clip is a
        // region of the app's own space, not of the buffer being rasterised
        // into, so neither of these VALUES changes with the scale; the
        // fragment stage reaches them through vApp, a varying, rather than
        // through gl_FragCoord, which would have needed the scale.
        //
        // The uniforms, though - not the resulting edge. The mask is evaluated
        // per fragment, so on a reduced buffer its boundary is resolved at
        // that buffer's texel pitch and the blit smears it. That is why an
        // enabled clipped lamp pins the scale to 1.0 rather than being one
        // more scale-invariant term: see GetClampedSpotScale for the numbers
        // and for why the neon's fix could not be reused. Past that pin, the
        // two paths below are only ever reached with NO lamp clipped, which is
        // what keeps "not one uniform differs" true of both of them.
        //
        // Uploaded unconditionally, including when no lamp is clipped. The
        // per-lamp opt-in is baked into the strips as a vertex attribute (see
        // buildStrips), so these two uniforms are inert rather than wrong when
        // nothing reads them, and skipping them would only buy a branch here
        // in exchange for a stale value the first frame a lamp opts in. There
        // is no area-level enable that could gate them either.
        const ClipSolve clipSolve = DeriveClip(config.spotlight.clipArea);
        mShaderProgram.SetUniform("uClipRect",
                                  glm::vec4(clipSolve.center.x, clipSolve.center.y,
                                            clipSolve.half.x, clipSolve.half.y));
        mShaderProgram.SetUniform("uClipParams",
                                  glm::vec3(clipSolve.radius, clipSolve.softness,
                                            clipSolve.keepInside ? 1.0f : 0.0f));

        mVertexArray.DrawArrays(GL_TRIANGLES, mVertexCount);

        mShaderProgram.Unuse();

        if (scaled)
        {
            // The host's clip comes back BEFORE the composite: this is the one
            // draw here that lands on the caller's framebuffer in the caller's
            // coordinates, so it is the one draw the scissor box describes
            // correctly. Everything above it went into a buffer the box does
            // not address.
            noScissor.Restore();

            // Back to the caller's target and viewport, both at once, then
            // composite. The buffer holds premultiplied colour and the
            // accumulated coverage alpha, and the blend above is still in
            // force, so this blit is a plain bilinear read ADDED onto whatever
            // is already there - colour and alpha alike. That is what carries
            // the coverage through to the caller's framebuffer, which is the
            // whole point of writing it; a blit under the old
            // GL_ONE_MINUS_SRC_ALPHA would instead have let the buffer's new
            // alpha eat the destination it is supposed to be adding to.
            //
            // One behavioural note the direct path does not have: overlapping
            // lamps sum into an RGBA8 buffer and clamp THERE before reaching
            // the target, whereas at 1.0 they clamp once against the target's
            // existing content. Only reachable where several lamps already sum
            // past white, and the same trade every accumulate-then-composite
            // path in this library makes.
            prevTarget.Restore();

            mBlitShader.Use();
            mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));
            mScaledBuffer.BindTexture(0);
            mBlitShader.SetUniform("uSource", 0);
            // neon-blit.frag carries the neon's one-sided glow cut, which this
            // layer has no business applying - so switch it off EXPLICITLY.
            // Every other uniform of that cut is then unread, which is why
            // none of them is set here.
            //
            // It was already off without this line, but only by luck twice
            // over: GlowSide::BOTH happens to be the enum's zero, and GL
            // happens to zero-initialise uniforms. Renumbering GlowSide would
            // have redirected this blit through a rounded-box SDF built from
            // uniforms nobody uploads - silently, and in the lens flare's blit
            // at the same time. Naming the value makes this track a renumber
            // instead, on the same terms neon-renderer.cpp already relies on:
            // the enum's ordinals and neon-blit.frag's GLOW_SIDE_* defines are
            // one numbering, kept in step by hand.
            mBlitShader.SetUniform("uGlowSide", static_cast<int>(GlowSide::BOTH));
            mBlitQuad.DrawArrays(GL_TRIANGLES, 6);
            mBlitShader.Unuse();
        }

        // Restore the blend state convention the other renderers leave behind.
        // glBlendFunc sets the RGB and alpha factors to the same pair, so it
        // also undoes the glBlendFuncSeparate above - nothing downstream
        // inherits this pass's split.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SpotlightRenderer::OnConfigChanged(const Config &config)
    {
        // Give the buffer back the moment a config stops asking for it - the
        // layer switched off, or the scale went back to 1.0 - so a rig that
        // visits a scaled path once does not hold a viewport-sized texture for
        // the rest of the session.
        //
        // Here rather than in Render because Render must not be the thing that
        // deletes a framebuffer. Pre-Initialize (AddRenderer calls this)
        // Release no-ops on the buffer it finds unallocated.
        if (!UsesScaledBuffer(config))
        {
            mScaledBuffer.Release();
        }

        // Before the rebuild gate below, and before mCurrentSpotlight is
        // overwritten, because both halves of the transition are read from it.
        // Ahead of the gate rather than behind it so the diagnostic is never
        // coupled to whether a rebuild happens - an unchanged config cannot be
        // a transition anyway, so the placement costs one size comparison.
        // This is the only place the lamp count can change.
        WarnOnLampOverflow(mCurrentSpotlight.lamps.size(), config.spotlight.lamps.size());
        // Same placement, same reason: both halves of the transition are read
        // from mCurrentSpotlight, and it is about to be overwritten.
        LogOnClipPinTransition(mCurrentSpotlight, config.spotlight);

        if (mBuilt && config.spotlight == mCurrentSpotlight)
        {
            return;
        }

        mCurrentSpotlight = config.spotlight;
        buildStrips(mCurrentSpotlight);
    }

    bool SpotlightRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::SPOTLIGHT_VERT_SRC,
                                       ShaderSource::SPOTLIGHT_FRAG_SRC,
                                       "SpotlightRenderer");
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "SpotlightRenderer.Blit");
        return mShaderProgram.IsValid() && mBlitShader.IsValid();
    }

    void SpotlightRenderer::setupBlitGeometry()
    {
        // Static fullscreen NDC quad for the scaled path's composite. Built
        // unconditionally alongside the blit shader, for the same reason: a
        // buffer upload in the middle of the first scaled frame is a stall
        // where it will be blamed on the scale.
        // clang-format off
        const float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mBlitQuad.SetVertexData(ndc, sizeof(ndc));
        mBlitQuad.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void SpotlightRenderer::ensureBuffer()
    {
        if (mBufferReady)
        {
            return;
        }

        mVertexArray.SetVertexData(nullptr, MAX_VERTS * sizeof(StripVertex), GL_DYNAMIC_DRAW);

        const GLsizei stride = static_cast<GLsizei>(sizeof(StripVertex));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, stride, offsetof(StripVertex, pos));
        mVertexArray.SetAttribPointer(1, 2, GL_FLOAT, stride, offsetof(StripVertex, local));
        mVertexArray.SetAttribPointer(2, 4, GL_FLOAT, stride, offsetof(StripVertex, p0));
        mVertexArray.SetAttribPointer(3, 4, GL_FLOAT, stride, offsetof(StripVertex, p1));
        mVertexArray.SetAttribPointer(4, 3, GL_FLOAT, stride, offsetof(StripVertex, color));
        mVertexArray.SetAttribPointer(5, 1, GL_FLOAT, stride, offsetof(StripVertex, clipWeight));

        mBufferReady = true;
    }

    void SpotlightRenderer::buildStrips(const SpotlightConfig &spotlight)
    {
        ensureBuffer();
        mBuilt = true;

        // Reused, never reallocated: clear() keeps the capacity, and the
        // reserve is a no-op after the first call. Under an animation this
        // method runs every frame, and a local vector would malloc and free
        // its 34 KB ceiling on each one - the same argument ensureBuffer makes
        // for the VBO, applied to the staging that fills it.
        mStripVerts.clear();
        mStripVerts.reserve(static_cast<size_t>(MAX_VERTS));

        const int lampCount = std::min(static_cast<int>(spotlight.lamps.size()),
                                       static_cast<int>(SPOT_MAX_LAMPS));

        // First pass: how many lamps actually draw. Each one's strip is then
        // solved against its share of the half-step budget, so the rig's total
        // clipped remainder stays under one half step however they overlap.
        // A lone lamp gets the whole budget and pays nothing for the sharing.
        int drawnLamps = 0;
        for (int i = 0; i < lampCount; i++)
        {
            const SpotLight &lamp = spotlight.lamps[static_cast<size_t>(i)];
            if (lamp.enable && lamp.intensity > 0.0f)
            {
                drawnLamps++;
            }
        }
        if (drawnLamps == 0)
        {
            mVertexCount = 0;
            return;
        }
        const float sharedFloor = static_cast<float>(SPOT_VISIBILITY_FLOOR) /
                                  static_cast<float>(drawnLamps);

        // Resolved once for the whole rig: the area is shared, and the only
        // per-lamp part of a clip is SpotLight::clipped, which is a bit.
        //
        // Not folded into the `drawnLamps` count above, deliberately. A lamp that
        // a KEEP_INSIDE area happens to cut down to nothing still counts
        // towards the shared visibility budget, which makes the budget
        // conservative rather than wrong - and it keeps that count a pure
        // function of the lamps, so moving the clip area cannot change how
        // brightly the UNCLIPPED lamps are bounded.
        //
        // Resolved unconditionally, including when no lamp is clipped: there
        // is no area-level enable to test, and the derivation is a handful of
        // min/max against a struct already in cache.
        const ClipSolve clipSolve = DeriveClip(spotlight.clipArea);

        for (int i = 0; i < lampCount; i++)
        {
            const SpotLight &lamp = spotlight.lamps[static_cast<size_t>(i)];

            LampSolve s{};
            if (!DeriveLamp(lamp, sharedFloor, s))
            {
                continue;
            }

            // App space is +y down, so a clockwise-increasing angle is just
            // the usual (cos, sin) - no sign juggling. The perpendicular
            // completes the frame; which way it points does not matter, since
            // the cross-beam falloff is symmetric.
            //
            // Needed here, ahead of the bound solve, because the clip
            // narrowing below works in this frame.
            const float rad = lamp.angle * DEG_TO_RAD;
            const float ca = std::cos(rad);
            const float sa = std::sin(rad);
            const float ox = lamp.position.x;
            const float oy = lamp.position.y;

            const bool clipped = lamp.clipped;

            // The strip spans [a0, a1] along the axis. a0 reaches back far
            // enough to cover spotlight.frag's near-end fade (which cannot
            // extend past 2 aperture widths for any SPOT_NEAR_FADE <= 2) and
            // the bloom disc behind the lamp, whichever is further.
            float a0 = -std::max(2.0f * s.apertureWidth, s.bloomReach);
            float a1 = std::max(SolveConeReach(s), s.bloomReach);

            // NARROW TO THE CLIP, for KEEP_INSIDE only.
            //
            // A clipped lamp writes nothing outside the area, so the strip
            // only has to cover the part of its support that lands inside -
            // and that region is bounded, which makes this the one case where
            // a clip buys fragments back rather than only hiding them. The
            // bound is the area's axis-aligned box IN THIS LAMP'S FRAME, taken
            // from its four SHARP corners: cornerRadius only ever cuts the
            // shape back, so the sharp box contains the rounded one.
            //
            // KEEP_OUTSIDE gets none of this. What survives there is the
            // complement of a bounded region, which is unbounded, so there is
            // nothing to intersect the support with and the strip stands as
            // solved.
            //
            // Across the beam the narrowing stays SYMMETRIC, because the strip
            // is: acrossCap = max(|acrossMin|, |acrossMax|) covers every point
            // of [acrossMin, acrossMax], which is conservative always and
            // exact whenever the area straddles the beam axis. It leaves area
            // on the table only when the clip sits entirely to one side of the
            // lamp - an asymmetric strip would close that, at the price of a
            // second sample array and a second widening pass. Not measured:
            // the symmetric cap was enough for the scenes this was verified
            // against, and correctness does not depend on the difference.
            float acrossCap = 0.0f;
            if (clipped && clipSolve.keepInside)
            {
                if (clipSolve.half.x <= 0.0f || clipSolve.half.y <= 0.0f)
                {
                    // A KEEP_INSIDE area with no area keeps no light. That is
                    // the honest answer, not a reason to fall back to drawnLamps
                    // the whole beam - see DeriveClip.
                    continue;
                }

                // Half a feather is how far past the sharp boundary the mask
                // is still non-zero; the +1 is the same rasterisation margin
                // SupportAt adds.
                const float margin = clipSolve.softness * 0.5f + 1.0f;

                float alongMin = 0.0f;
                float alongMax = 0.0f;
                float acrossMin = 0.0f;
                float acrossMax = 0.0f;
                for (int c = 0; c < 4; c++)
                {
                    const float cx = clipSolve.center.x + ((c & 1) ? clipSolve.half.x : -clipSolve.half.x);
                    const float cy = clipSolve.center.y + ((c & 2) ? clipSolve.half.y : -clipSolve.half.y);
                    const float dx = cx - ox;
                    const float dy = cy - oy;
                    // The inverse of the corner transform at the bottom of
                    // this loop body - a rotation, so its transpose.
                    const float alongC = dx * ca + dy * sa;
                    const float acrossC = -dx * sa + dy * ca;
                    if (c == 0)
                    {
                        alongMin = alongMax = alongC;
                        acrossMin = acrossMax = acrossC;
                        continue;
                    }
                    alongMin = std::min(alongMin, alongC);
                    alongMax = std::max(alongMax, alongC);
                    acrossMin = std::min(acrossMin, acrossC);
                    acrossMax = std::max(acrossMax, acrossC);
                }

                a0 = std::max(a0, alongMin - margin);
                a1 = std::min(a1, alongMax + margin);
                acrossCap = std::max(std::fabs(acrossMin), std::fabs(acrossMax)) + margin;
            }

            if (a1 <= a0)
            {
                continue;
            }

            // Sample the support, then widen every sample to the MAXIMUM of
            // the support across the half-intervals it shares with its
            // neighbours, so a straight chord between two samples bulges
            // OUTSIDE the true support rather than cutting inside it.
            //
            // Sub-sampled rather than checking the two midpoints alone because
            // c(a) falls to zero at the far end with a near-vertical tangent
            // (it carries a sqrt of a term going to zero), so a midpoint pair
            // does not bound the last chord. It is also where most of this
            // method's arithmetic goes, so read WIDEN_SUBSAMPLES before
            // deciding it is free.
            //
            // WHAT THIS DOES AND DOES NOT PROVE, because it is easy to read it
            // as the whole correctness argument and it is not.
            //
            // Widening bounds the chord wherever the widened curve is CONVEX
            // over the segment. It does not where that curve is concave, and
            // there is one such place by construction: SolveConeAcross reads
            // max(a, 0), so the support is FLAT for a < 0 and decreasing after
            // - a corner at a = 0 that a long first segment cuts straight
            // across. Measured against this solve, throwLength 900 cuts 1.52
            // px inside it at a = -7.4, and intensity 50 under a 4x tint cuts
            // 1.66 px. Neither is covered by the +1 px margin SupportAt adds.
            //
            // NO FRAGMENT IS LOST THERE, and the reason is the term this solve
            // deliberately does not invert. Every cut lands at NEGATIVE a,
            // where spotlight.frag's near-end fade is still closing:
            // smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, a) is 0.33 at
            // a = 0 and 0.07 at a = -7.4, so over exactly the span where the
            // chord cuts, the bound is 3x to 13x more conservative than the
            // shader it bounds. Verified by evaluating the shader's own term
            // stack on a 1 px grid of the lamp frame: across twelve parameter
            // sets (throws to 4000 px, beams from 2 to 150 degrees, large
            // apertures, large blooms, boosted tints, an eight-lamp floor) and
            // sixteen clip scenes, ZERO fragments above the visibility floor
            // fell outside the strip.
            //
            // So the guarantee holds on two legs, not one: widening plus the
            // margin bound the strip wherever the solve is exact, which is
            // a >= SPOT_NEAR_FADE * apertureWidth, and the un-inverted near fade
            // covers the corner at the start. That makes SPOT_NEAR_FADE load
            // bearing beyond the a0 bound its own comment describes - raising
            // it spends this slack. Re-run the grid if it moves.
            float sampleA[SPOT_STRIP_SEGMENTS + 1];
            float sampleC[SPOT_STRIP_SEGMENTS + 1];
            for (int k = 0; k <= SPOT_STRIP_SEGMENTS; k++)
            {
                const float t = static_cast<float>(k) / static_cast<float>(SPOT_STRIP_SEGMENTS);
                sampleA[k] = a0 + (a1 - a0) * t;
                sampleC[k] = SupportAt(s, sampleA[k]);
            }
            for (int k = 0; k <= SPOT_STRIP_SEGMENTS; k++)
            {
                const float prev = sampleA[std::max(k - 1, 0)];
                const float next = sampleA[std::min(k + 1, SPOT_STRIP_SEGMENTS)];
                for (int t = 1; t <= WIDEN_SUBSAMPLES; t++)
                {
                    const float f = static_cast<float>(t) / static_cast<float>(WIDEN_SUBSAMPLES);
                    sampleC[k] = std::max(sampleC[k],
                                          std::max(SupportAt(s, sampleA[k] + (prev - sampleA[k]) * f * 0.5f),
                                                   SupportAt(s, sampleA[k] + (next - sampleA[k]) * f * 0.5f)));
                }
                // AFTER the widening, not before it: the widening takes a
                // maximum, so a cap applied first would just be undone.
                if (acrossCap > 0.0f)
                {
                    sampleC[k] = std::min(sampleC[k], acrossCap);
                }
            }

            const float clipWeight = clipped ? 1.0f : 0.0f;

            for (int seg = 0; seg < SPOT_STRIP_SEGMENTS; seg++)
            {
                const float aA = sampleA[seg];
                const float cA = sampleC[seg];
                const float aB = sampleA[seg + 1];
                const float cB = sampleC[seg + 1];

                const float corner[4][2] = {
                    {aA, -cA}, {aB, -cB}, {aB, cB}, {aA, cA}};
                // COUNTER-CLOCKWISE once Render's y-flipped ortho has been
                // applied, which is why the indices run backwards round the
                // quad. The corners above are listed the natural way for APP
                // space (+y down); the projection's negative y scale reverses
                // winding, so listing them {0,1,2, 0,2,3} emitted triangles
                // that were back-facing on screen.
                //
                // That made this the ONLY back-facing geometry in the library
                // - every other renderer projects y-up and comes out CCW - and
                // so the only layer a host with GL_CULL_FACE on deletes
                // outright. Invisible on any desktop demo, where culling is off
                // by default and nothing ever enables it; very visible on an
                // embedded surface view sharing its context with a video
                // pipeline or web engine that left culling behind.
                //
                // This winding is what stops THIS geometry from being wrong
                // in the first place, so the strips are correct on their own
                // terms under the default GL_CCW / GL_BACK configuration
                // rather than only because something upstream switched
                // culling off. The rest of the ways a host can cull (GL_FRONT,
                // a GL_CW front face) are not covered anywhere in the library
                // and are the caller's to settle.
                //
                // Nothing about the image changes: GL's fill rule is
                // orientation-independent, and every vertex of a lamp's strip
                // carries the same flat attributes, so the provoking vertex
                // moving cannot change them either.
                const int idx[6] = {0, 2, 1, 0, 3, 2};

                for (int k = 0; k < 6; k++)
                {
                    const float along = corner[idx[k]][0];
                    const float across = corner[idx[k]][1];

                    StripVertex v{};
                    v.pos[0] = ox + along * ca - across * sa;
                    v.pos[1] = oy + along * sa + across * ca;
                    v.local[0] = along;
                    v.local[1] = across;
                    v.p0[0] = s.tanHalf;
                    v.p0[1] = s.throwLength;
                    v.p0[2] = s.softK;
                    v.p0[3] = s.intensity;
                    v.p1[0] = s.apertureWidth;
                    v.p1[1] = s.bloom;
                    v.p1[2] = s.bloomRadius;
                    v.p1[3] = s.bloomWindow;
                    v.color[0] = s.color.r;
                    v.color[1] = s.color.g;
                    v.color[2] = s.color.b;
                    v.clipWeight = clipWeight;
                    mStripVerts.push_back(v);
                }
            }
        }

        mVertexCount = static_cast<int>(mStripVerts.size());
        if (mVertexCount == 0)
        {
            return;
        }

        // Sub-data into the ceiling-sized allocation from ensureBuffer, never
        // a fresh glBufferData: under an animation this runs every frame, and
        // a reallocation per frame is exactly what the ceiling is for.
        mVertexArray.BindBuffer();
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(mStripVerts.size() * sizeof(StripVertex)),
                        mStripVerts.data());
    }
}
