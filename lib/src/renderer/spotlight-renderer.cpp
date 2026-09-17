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
        constexpr int MAX_VERTS = SPOT_MAX_LIGHTS * VERTS_PER_LAMP;
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

        /// Floors matching the ones spotlight.frag applies, so the solve below
        /// and the shader agree about what a degenerate lamp means instead of
        /// disagreeing near zero.
        constexpr float MIN_APERTURE = 1.0f;
        constexpr float MIN_THROW = 1.0f;

        /// Floor under SpotlightConfig::resolutionScale. Below this the blit
        /// is reading so few texels that the light turns to blocks, and the
        /// strips are already the cheap part.
        constexpr float MIN_RESOLUTION_SCALE = 0.125f;

        float GetClampedSpotScale(const Config &config)
        {
            return std::min(std::max(config.spotlight.resolutionScale, MIN_RESOLUTION_SCALE), 1.0f);
        }

        /// Whether this config gives @c mScaledBuffer anything to do.
        ///
        /// ONE predicate for two questions that have to agree: @ref Render asks
        /// it to pick the path, and @ref OnConfigChanged asks it to decide
        /// whether to release the buffer. Split them and a config that stops
        /// using the buffer can leave it allocated forever, or - worse - one
        /// that still needs it can have it freed underneath.
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
            float tanHalf;   ///< tan(beamAngle / 2)
            float thr;       ///< throwLength, floored
            float softK;     ///< gaussian exponent across the beam
            float nearW;     ///< apertureWidth, floored
            float intensity;
            float bloom;
            float bloomRadius;
            float bloomWindow; ///< Where spotlight.frag's window closes.
            float bloomBound;  ///< Where the bloom actually stops being visible.
            float floor;       ///< This lamp's share of the half-step budget.
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
        ///   solveIntensity * exp(-a / thr) * (nearW / halfW) * exp(-lat^2 * softK)
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
            const float halfW = s.nearW + alongPos * s.tanHalf;
            const float headroom = std::log(s.solveIntensity) - std::log(s.floor) -
                                   alongPos / s.thr -
                                   std::log(halfW / s.nearW);
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

            float hi = s.thr;
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
            if (s.bloomBound > 0.0f && std::fabs(a) < s.bloomBound)
            {
                c = std::max(c, std::sqrt(s.bloomBound * s.bloomBound - a * a));
            }
            return c + 1.0f;
        }

        /// Build the per-lamp derived values, or report the lamp draws nothing.
        ///
        /// @p floor is this lamp's share of the half-8-bit-step budget - see
        /// SPOT_VISIBILITY_FLOOR. Everything the solve cuts is below it, so the
        /// whole rig's clipped remainder stays under one half step.
        bool DeriveLamp(const SpotLight &light, float floor, LampSolve &out)
        {
            if (!light.enable || light.intensity <= 0.0f)
            {
                return false;
            }

            // Clamped below 180 so the half-angle stays under 90 and tan
            // stays finite: a lamp is a cone, and a "cone" at 180 degrees is
            // a half-plane with no axis left to speak of.
            const float beam = std::min(std::max(light.beamAngle, 0.0f), 170.0f);
            out.tanHalf = std::max(std::tan(beam * 0.5f * DEG_TO_RAD), 0.0f);
            out.thr = std::max(light.throwLength, MIN_THROW);
            out.nearW = std::max(light.apertureWidth, MIN_APERTURE);
            out.intensity = light.intensity;
            out.bloom = std::max(light.bloom, 0.0f);
            out.bloomRadius = std::max(light.bloomRadius, 1.0f);
            out.color = KelvinToRgb(light.colorTemp) * light.tint;
            out.floor = floor;

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

            const float softness = std::min(std::max(light.softness, 0.0f), 1.0f);
            out.softK = static_cast<float>(SPOT_SOFT_MIN) +
                        (1.0f - softness) *
                            static_cast<float>(SPOT_SOFT_MAX - SPOT_SOFT_MIN);

            out.bloomWindow = out.bloomRadius * static_cast<float>(SPOT_BLOOM_SUPPORT);

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
            out.bloomBound = 0.0f;
            const float peak = out.solveIntensity * out.bloom;
            if (peak > out.floor)
            {
                const float ratio = peak / out.floor - 1.0f;
                const float visible = out.bloomRadius * std::sqrt(std::max(ratio, 0.0f));
                out.bloomBound = std::min(out.bloomWindow, visible);
            }

            return true;
        }

        /// Warn when a host hands over more lamps than one pass can draw.
        /// @ref SpotlightRenderer::buildStrips clamps the list with
        /// @c std::min against @c SPOT_MAX_LIGHTS and everything past it never
        /// reaches the VBO, so without this the excess disappears with no log
        /// line and no result code. Both demo UIs cap their Add button, so
        /// neither of them can see it; a library host writing
        /// @c SpotlightConfig::lights directly, or a C-ABI host calling
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
            if (static_cast<int>(now) > SPOT_MAX_LIGHTS &&
                static_cast<int>(prev) <= SPOT_MAX_LIGHTS)
            {
                LOG_E("SpotlightRenderer: %zu lamps configured but only %d fit - "
                      "the rest are ignored.",
                      now, static_cast<int>(SPOT_MAX_LIGHTS));
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
        // into the context. buildStrips winds for the flip, and
        // EdgeLightingEffect::Render's NoCullScope covers every cull
        // configuration for every layer; see both for the measurement. No other
        // handedness has to be kept in step, since the fragment stage reads no
        // gl_FragCoord.
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
        WarnOnLampOverflow(mCurrentSpotlight.lights.size(), config.spotlight.lights.size());

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

        const int lampCount = std::min(static_cast<int>(spotlight.lights.size()),
                                       static_cast<int>(SPOT_MAX_LIGHTS));

        // First pass: how many lamps actually draw. Each one's strip is then
        // solved against its share of the half-step budget, so the rig's total
        // clipped remainder stays under one half step however they overlap.
        // A lone lamp gets the whole budget and pays nothing for the sharing.
        int drawing = 0;
        for (int i = 0; i < lampCount; i++)
        {
            const SpotLight &light = spotlight.lights[static_cast<size_t>(i)];
            if (light.enable && light.intensity > 0.0f)
            {
                drawing++;
            }
        }
        if (drawing == 0)
        {
            mVertexCount = 0;
            return;
        }
        const float sharedFloor = static_cast<float>(SPOT_VISIBILITY_FLOOR) /
                                  static_cast<float>(drawing);

        for (int i = 0; i < lampCount; i++)
        {
            const SpotLight &light = spotlight.lights[static_cast<size_t>(i)];

            LampSolve s{};
            if (!DeriveLamp(light, sharedFloor, s))
            {
                continue;
            }

            // The strip spans [a0, a1] along the axis. a0 reaches back far
            // enough to cover spotlight.frag's near-end fade (which cannot
            // extend past 2 aperture widths for any SPOT_NEAR_FADE <= 2) and
            // the bloom disc behind the lamp, whichever is further.
            const float a0 = -std::max(2.0f * s.nearW, s.bloomBound);
            const float a1 = std::max(SolveConeReach(s), s.bloomBound);
            if (a1 <= a0)
            {
                continue;
            }

            // Sample the support, then widen every sample to the MAXIMUM of
            // the support across the half-intervals it shares with its
            // neighbours. A straight chord between two samples can then only
            // bulge OUTSIDE the true support, never cut inside it - which is
            // the whole correctness argument for approximating a curve with
            // SPOT_STRIP_SEGMENTS quads.
            //
            // Sub-sampled rather than checking the two midpoints alone because
            // c(a) falls to zero at the far end with a near-vertical tangent
            // (it carries a sqrt of a term going to zero), so a midpoint pair
            // does not bound the last chord. Measured: it changed no pixel in
            // any verification scene, so this is a guarantee being made true
            // rather than a bug being fixed - and it is also where most of
            // this method's arithmetic goes, so read WIDEN_SUBSAMPLES before
            // deciding it is free.
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
            }

            // App space is +y down, so a clockwise-increasing angle is just
            // the usual (cos, sin) - no sign juggling. The perpendicular
            // completes the frame; which way it points does not matter, since
            // the cross-beam falloff is symmetric.
            const float rad = light.angle * DEG_TO_RAD;
            const float ca = std::cos(rad);
            const float sa = std::sin(rad);
            const float ox = light.position.x;
            const float oy = light.position.y;

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
                // EdgeLightingEffect::Render's NoCullScope covers the rest of
                // the ways a host can cull (GL_FRONT, a GL_CW front face) for
                // every layer at once. This is the half that stops THIS
                // geometry from being wrong in the first place, so the strips
                // are correct on their own terms rather than only because
                // something upstream switches culling off.
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
                    v.p0[1] = s.thr;
                    v.p0[2] = s.softK;
                    v.p0[3] = s.intensity;
                    v.p1[0] = s.nearW;
                    v.p1[1] = s.bloom;
                    v.p1[2] = s.bloomRadius;
                    v.p1[3] = s.bloomWindow;
                    v.color[0] = s.color.r;
                    v.color[1] = s.color.g;
                    v.color[2] = s.color.b;
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
