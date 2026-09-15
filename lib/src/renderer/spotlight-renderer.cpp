#include "renderer/spotlight-renderer.h"
#include "renderer/spotlight-tuning.h"
#include "util/log-util.h"
#include "shaders.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace EdgeLighting
{
    namespace
    {
        /// One vertex of a lamp's strip. 15 floats; the four non-position
        /// members are constant across a whole strip (see spotlight.vert).
        struct StripVertex
        {
            float pos[2];   ///< App px, top-left origin, +y down.
            float local[2]; ///< (along, across) px in the lamp's frame.
            float p0[4];    ///< tanHalfBeam, throwLength, softK, intensity.
            float p1[4];    ///< apertureWidth, bloom, bloomRadius, bloomSupport.
            float color[3]; ///< Linear RGB.
        };

        static_assert(sizeof(StripVertex) == 15 * sizeof(float),
                      "StripVertex must be tightly packed - the attribute "
                      "pointers below use sizeof(StripVertex) as the stride.");

        constexpr int VERTS_PER_LAMP = SPOT_STRIP_SEGMENTS * 6;
        constexpr int MAX_VERTS = SPOT_MAX_LIGHTS * VERTS_PER_LAMP;
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

        /// Floors matching the ones spotlight.frag applies, so the solve below
        /// and the shader agree about what a degenerate lamp means instead of
        /// disagreeing near zero.
        constexpr float MIN_APERTURE = 1.0f;
        constexpr float MIN_THROW = 1.0f;

        /// Sub-samples per half-interval when widening a strip sample to the
        /// support's maximum over the span its chords cover. CPU-only and
        /// cheap: it buys a tighter guarantee, never a different image.
        constexpr int WIDEN_SUBSAMPLES = 8;

        /// Blackbody colour anchors, linearly interpolated between. Local to
        /// this renderer because it is the only consumer - the same split
        /// droplets-tuning.h's comment describes for one-consumer constants.
        struct KelvinAnchor
        {
            float k, r, g, b;
        };

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
        struct LampSolve
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
            glm::vec3 color;
        };

        /// Half-width of the CONE's support at @p a, in the lamp's frame.
        ///
        /// This inverts spotlight.frag's falloff. Taking logs of
        ///
        ///   intensity * exp(-a / thr) * (nearW / halfW) * exp(-lat^2 * softK)
        ///       >= SPOT_VISIBILITY_FLOOR
        ///
        /// and solving for lat gives the expression below. `smoothstep`'s
        /// near-end fade is not inverted - it only ever REDUCES the term, and
        /// the strip's start is bounded separately and conservatively.
        ///
        /// Returns 0 where the cone cannot reach the visibility floor at all.
        float SolveConeAcross(const LampSolve &s, float a)
        {
            const float alongPos = std::max(a, 0.0f);
            const float halfW = s.nearW + alongPos * s.tanHalf;
            const float headroom = std::log(s.intensity) - std::log(s.floor) -
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

        /// Half-width of the WHOLE support at @p a: the cone, unioned with the
        /// aperture bloom's disc. The +1 is a rasterisation safety margin, the
        /// same spirit as droplets-renderer.cpp's SAFETY_PX.
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
            out.color = KelvinToRgb(light.colorTemp);
            out.floor = floor;

            const float softness = std::min(std::max(light.softness, 0.0f), 1.0f);
            out.softK = static_cast<float>(SPOT_SOFT_MIN) +
                        (1.0f - softness) *
                            static_cast<float>(SPOT_SOFT_MAX - SPOT_SOFT_MIN);

            out.bloomWindow = out.bloomRadius * static_cast<float>(SPOT_BLOOM_SUPPORT);

            // The bloom's own support is the TIGHTER of two bounds: where
            // spotlight.frag's window closes, and where the inverse-square
            // core itself drops under the visibility floor. Solving
            //   intensity * bloom * r^2 / (d^2 + r^2) = floor
            // for d gives the second. Taking the min is exact, not
            // conservative - past either one the term is provably zero.
            out.bloomBound = 0.0f;
            const float peak = out.intensity * out.bloom;
            if (peak > out.floor)
            {
                const float ratio = peak / out.floor - 1.0f;
                const float visible = out.bloomRadius * std::sqrt(std::max(ratio, 0.0f));
                out.bloomBound = std::min(out.bloomWindow, visible);
            }

            return true;
        }
    }

    bool SpotlightRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link SpotlightRenderer shaders.");
            return false;
        }

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

        // A y-FLIPPED ortho: note bottom and top are swapped relative to the
        // one the other renderers build. That is what lets the VBO hold app
        // coordinates verbatim - top-left origin, +y down, the same space as
        // Config::geometry.position - instead of framebuffer coordinates, so
        // the buffer depends only on Config::spotlight and a resize costs this
        // uniform rather than a rebuild.
        //
        // The flip reverses triangle winding. Harmless here: nothing in this
        // library enables GL_CULL_FACE, and the fragment stage reads no
        // gl_FragCoord, so there is no other handedness to keep in step.
        const glm::mat4 mvp = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                         static_cast<float>(viewportHeight), 0.0f,
                                         -1.0f, 1.0f);

        // Premultiplied "over" with alpha 0 throughout, which is pure
        // addition: light only ever adds. Also order-independent, so the lamp
        // order in the config cannot change the image.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);

        mVertexArray.DrawArrays(GL_TRIANGLES, mVertexCount);

        mShaderProgram.Unuse();

        // Restore the blend state convention the other renderers leave behind.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SpotlightRenderer::OnConfigChanged(const Config &config)
    {
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
        return mShaderProgram.IsValid();
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

        std::vector<StripVertex> verts;
        verts.reserve(static_cast<size_t>(MAX_VERTS));

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
            // rather than a bug being fixed.
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
                const int idx[6] = {0, 1, 2, 0, 2, 3};

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
                    verts.push_back(v);
                }
            }
        }

        mVertexCount = static_cast<int>(verts.size());
        if (mVertexCount == 0)
        {
            return;
        }

        // Sub-data into the ceiling-sized allocation from ensureBuffer, never
        // a fresh glBufferData: under an animation this runs every frame, and
        // a reallocation per frame is exactly what the ceiling is for.
        mVertexArray.BindBuffer();
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(verts.size() * sizeof(StripVertex)),
                        verts.data());
    }
}
