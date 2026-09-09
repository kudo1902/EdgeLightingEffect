#include "renderer/droplets-renderer.h"
#include "renderer/droplets-tuning.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    namespace
    {
        /// The signed-distance interval, in px from the rect edge, that
        /// @c droplets.frag can write into. Its early bail discards everything
        /// else, so this is exactly what the geometry has to cover - and
        /// exactly what it may leave out.
        typedef struct BandExtent
        {
            float inner; ///< Nearest distance written (negative = inside the rect).
            float outer; ///< Furthest distance written.
        } BandExtent;

        /// @ref BandExtent for the live glow side.
        ///
        /// This inverts the shader's @c BandAcross for sd over the window it
        /// keeps, @c across in [-GUARD, 1 + GUARD] (@ref DROPLET_BAND_GUARD,
        /// shared with it verbatim through droplets-tuning.h):
        ///
        ///   INSIDE   across = (-sd - offset) / bw
        ///   OUTSIDE  across = ( sd - offset) / bw
        ///   BOTH     across = ( sd + bw / 2 - offset) / bw
        ///
        /// The if / if / fallthrough shape is the shader's own, deliberately:
        /// "anything that is not INSIDE or OUTSIDE is BOTH" then holds on both
        /// sides, so no glow side can ever reach a different branch here than
        /// it does there. A switch with a default would let the two disagree.
        ///
        /// The quad this used to build only needed @c outer - it was centred
        /// on the rect and grown, so its interior was covered whatever the
        /// side. The ring cuts a hole, so @c inner has to be right too, and
        /// being loose about it costs coverage rather than just fill.
        inline BandExtent GetBandExtent(const Config &config)
        {
            // The shader floors the band at 1 px (`max(uBandWidth, 1.0)`) and
            // then divides by it, so the CPU has to floor it identically or
            // the two disagree about where `across` 1.0 sits.
            const float bw = std::max(config.droplets.bandWidth, 1.0f);
            const float offset = config.droplets.bandOffset;
            const float guard = static_cast<float>(DROPLET_BAND_GUARD);
            const float lo = -guard;       ///< Lowest `across` the shader keeps.
            const float hi = 1.0f + guard; ///< Highest.

            if (config.neon.glowSide == GlowSide::INSIDE)
            {
                // Negating flips the ends, so hi gives the inner bound here.
                return {-(offset + hi * bw), -(offset + lo * bw)};
            }
            if (config.neon.glowSide == GlowSide::OUTSIDE)
            {
                return {offset + lo * bw, offset + hi * bw};
            }
            return {offset + (lo - 0.5f) * bw, offset + (hi - 0.5f) * bw};
        }

        /// How far in from the straight-edged box of a rounded rect with radius
        /// r the largest INSCRIBED axis-aligned box sits: r * (1 - 1/sqrt(2)).
        ///
        /// The inscribed box touches the 45-degree point of each corner arc.
        /// The ring's hole is a rounded rect, but the strips that surround it
        /// are axis-aligned, so the hole they may leave uncovered is this box
        /// and not the rounded shape itself - the difference is four corner
        /// slivers, which the strips cover rather than omit.
        constexpr float CORNER_INSET = 0.292893219f;

        /// Rasterisation slack, in px, on every boundary the geometry derives.
        /// The shader's bound is a hard discard, not a fade, so this only has
        /// to absorb pixel-centre rounding.
        constexpr float SAFETY_PX = 1.0f;
    }

    bool DropletsRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link DropletsRenderer shaders.");
            return false;
        }
        // mCurrentConfig is whatever the last OnConfigChanged left - the effect
        // calls it on registration, so by here it is usually the host's real
        // config rather than the defaults. Either way the quad exists from
        // this point on, and OnConfigChanged re-sizes it on every change.
        setupGeometry(mCurrentConfig);
        return true;
    }

    void DropletsRenderer::Update(float, float, const Config &)
    {
    }

    void DropletsRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.droplets.enable || viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        // Rect centre in framebuffer pixel space (origin at bottom-left, matching
        // gl_FragCoord). Config::geometry uses a top-left origin, hence the flip.
        glm::vec2 rectCenter(config.geometry.position.x + halfRectW,
                             static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // The quad is built in rect-local px around the origin (@ref
        // setupGeometry), so the transform is just "put the origin at the rect
        // centre" under a projection that maps px 1:1 onto gl_FragCoord. This
        // is what carries geometry.position - setupGeometry does not read it,
        // which is why a pure move never re-uploads the quad.
        //
        // The shader shapes everything from gl_FragCoord, not vPos, so the
        // transform only decides WHICH fragments are rasterised. Every one it
        // drops was discarded by the shader anyway.
        const glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                          0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(rectCenter, 0.0f));

        // Premultiplied-alpha "over" - the band feathers into the existing
        // framebuffer at its discard boundary.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uRectCenter", rectCenter);
        mShaderProgram.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uAmount", config.droplets.amount);
        mShaderProgram.SetUniform("uSpeed", config.droplets.speed);
        mShaderProgram.SetUniform("uLanes", std::max(config.droplets.lanes, 1));
        mShaderProgram.SetUniform("uBandWidth", config.droplets.bandWidth);
        mShaderProgram.SetUniform("uBandOffset", config.droplets.bandOffset);
        mShaderProgram.SetUniform("uTint", config.droplets.tint);
        // The band's side-mask tracks the neon's live glow-side directly -
        // there is no droplet-side duplicate. Change the neon's side (or
        // softness) and the wet region re-masks automatically.
        mShaderProgram.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mVertexArray.DrawArrays(GL_TRIANGLES, mVertexCount);

        mShaderProgram.Unuse();

        // Restore the blend state convention the other renderers leave behind.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void DropletsRenderer::OnConfigChanged(const Config &config)
    {
        // The quad is sized from the rect's extents and the band's outward
        // reach, so it is rebuilt when either moves and left alone otherwise -
        // every other droplet parameter is a per-frame uniform. Dragging Rain
        // Amount or Speed must not re-upload a VBO.
        //
        // geometry.position is compared along with the rest of RectGeometry
        // even though setupGeometry ignores it (Render's transform carries the
        // position instead): one redundant rebuild on a move is cheaper than a
        // gate that silently stops matching if the quad ever grows a
        // dependency on it.
        const bool geometryDirty = config.geometry != mCurrentConfig.geometry ||
                                   config.droplets.bandWidth != mCurrentConfig.droplets.bandWidth ||
                                   config.droplets.bandOffset != mCurrentConfig.droplets.bandOffset;

        mCurrentConfig = config;

        if (geometryDirty)
        {
            setupGeometry(config);
        }
    }

    bool DropletsRenderer::setupShaders()
    {
        // Reuses the standard neon vertex shader (uMVP + aPos -> vPos) so the
        // pane quad lives in the same rect-local space as the glow quad.
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::DROPLETS_FRAG_SRC,
                                       "DropletsRenderer");
        return mShaderProgram.IsValid();
    }

    void DropletsRenderer::setupGeometry(const Config &config)
    {
        // A RING of four strips around the band, in rect-local px centred on
        // the origin - not a fullscreen quad, and no longer a solid one.
        //
        // The band is a thin rounded ring hugging the perimeter, so a
        // fullscreen quad spent almost every fragment computing a band
        // coordinate and discarding: on an 800x600 rect in a 1600x1200
        // viewport, ~1.9M invocations to shade a band of ~100k. Bounding the
        // geometry by the rect took that to ~570k; cutting the rect's own
        // interior out takes it to ~140k. Neither changes a drawn pixel -
        // everything removed was discarded by the shader anyway.
        const BandExtent band = GetBandExtent(config);

        const float halfW = config.geometry.width * 0.5f;
        const float halfH = config.geometry.height * 0.5f;

        // Outer bound. Offsetting the rounded rect outward by the furthest
        // distance written gives a shape contained in the axis-aligned box of
        // these half-extents, whatever the corner radius. Floored at zero: a
        // band that sits far enough inside erodes the rect away entirely, and
        // a zero-size ring correctly draws nothing.
        const float ox = std::max(halfW + band.outer + SAFETY_PX, 0.0f);
        const float oy = std::max(halfH + band.outer + SAFETY_PX, 0.0f);

        // Inner bound - the hole. Offsetting the rounded rect by band.inner
        // gives a rounded rect of these half-extents and this radius; the ring
        // may only omit what fits strictly inside it.
        //
        // The effective radius is the one the shader is handed, so the two
        // agree on the shape being offset. It never exceeds min(halfW, halfH),
        // and the offset moves radius and half-extents together, so holeR
        // cannot outgrow the box it rounds.
        const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry);
        const float holeR = std::max(radius + band.inner, 0.0f);
        const float ix = halfW + band.inner - holeR * CORNER_INSET - SAFETY_PX;
        const float iy = halfH + band.inner - holeR * CORNER_INSET - SAFETY_PX;

        if (ix <= 0.0f || iy <= 0.0f)
        {
            // The band reaches the middle - a BOTH or INSIDE band as wide as
            // the rect, or a deep negative offset - so there is no hole to
            // cut. One quad, which is what the ring degenerates to.
            // clang-format off
            const float quad[] = {
                -ox,  oy,  -ox, -oy,   ox, -oy,
                -ox,  oy,   ox, -oy,   ox,  oy,
            };
            // clang-format on
            mVertexCount = 6;
            mVertexArray.SetVertexData(quad, sizeof(quad));
            mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
            return;
        }

        // Four strips tiling the outer box minus the hole: top and bottom run
        // the full width, left and right fill the remaining height between
        // them.
        //
        // They must TILE, not merely cover. The pass blends premultiplied, so
        // a pixel covered by two strips would be composited twice and come out
        // brighter - a visible seam, not just wasted fill. Sharing exact edge
        // coordinates is what prevents that: GL's fill rule hands a pixel on a
        // shared edge to exactly one of the two triangles. Perturb one of these
        // values without the other and the guarantee is gone in both
        // directions - a bright seam, or a missing pixel line.
        // clang-format off
        const float ring[] = {
            // top strip: full width, above the hole
            -ox,  oy,  -ox,  iy,   ox,  iy,
            -ox,  oy,   ox,  iy,   ox,  oy,
            // bottom strip: full width, below the hole
            -ox, -iy,  -ox, -oy,   ox, -oy,
            -ox, -iy,   ox, -oy,   ox, -iy,
            // left strip: between the two, left of the hole
            -ox,  iy,  -ox, -iy,  -ix, -iy,
            -ox,  iy,  -ix, -iy,  -ix,  iy,
            // right strip: between the two, right of the hole
             ix,  iy,   ix, -iy,   ox, -iy,
             ix,  iy,   ox, -iy,   ox,  iy,
        };
        // clang-format on

        mVertexCount = 24;
        mVertexArray.SetVertexData(ring, sizeof(ring));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }
}
