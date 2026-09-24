#include "renderer/droplets-renderer.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    namespace
    {
        /// One bounding shape of the droplets' region: a rounded rect.
        typedef struct RegionShape
        {
            /// Centre in APP coordinates (origin top-left, +y DOWN), the same
            /// space as @c RectGeometry::position. @c Render flips it into
            /// framebuffer px at upload; keeping the flip out of the resolve is
            /// what lets @c setupGeometry run without a viewport.
            glm::vec2 center{0.0f};
            glm::vec2 halfSize{0.0f}; ///< Half extents in px.
            float radius = 0.0f;      ///< Corner radius in px, already clamped.
        } RegionShape;

        /// The droplets' region: an outer rounded rect minus an inner one.
        ///
        /// The two shapes are INDEPENDENT - different centres, extents and
        /// corner radii are all legal - which is what makes a band of varying
        /// thickness, an off-centre hole or a pane with a bite out of it
        /// expressible. A plain perimeter band is the concentric special case,
        /// and nothing downstream relies on it being one.
        typedef struct Region
        {
            RegionShape outer;
            RegionShape inner; ///< Unread when @c hasInner is false.
            bool hasInner;
        } Region;

        /// A @ref RegionShape for a @c RectGeometry. @c winding is not read -
        /// the droplets never traverse a perimeter.
        inline RegionShape ShapeOf(const RectGeometry &g)
        {
            const float halfW = g.width * 0.5f;
            const float halfH = g.height * 0.5f;
            return {glm::vec2(g.position.x + halfW, g.position.y + halfH),
                    glm::vec2(halfW, halfH),
                    GeometryUtils::GetEffectiveCornerRadius(g)};
        }

        /// The droplets' region for the live config.
        ///
        /// The two shapes are read straight off @c DropletsConfig, and nothing
        /// here touches @c Config::geometry or @c NeonConfig - this layer's
        /// geometry is its own. A host that wants the rain to sit on the
        /// shared rect copies those numbers into @c outer itself, which is the
        /// point: the agreement is deliberate rather than structural.
        ///
        /// Thin enough to be one line, and kept as a function anyway: it is
        /// the single place the region is defined, and both the draw geometry
        /// and the uniform upload go through it so they cannot disagree. Its
        /// ancestor had to re-derive backwards what the shader computed
        /// forwards, and keeping that impossible is worth a function.
        inline Region ResolveRegion(const Config &config)
        {
            const DropletsConfig &d = config.droplets;
            const RegionShape inner = ShapeOf(d.inner);

            // A zero- or negative-size inner shape is NOT an empty set to a
            // distance field. @c sdRoundBox with zero half extents reduces to
            // @c length(p) - a POINT - and with one zero extent to a line
            // segment; both have a real distance field, which the shader's
            // @c min() reads as a real boundary. The result is a phantom hole
            // at @c inner.position with no size, and - because the region's
            // local width is @c dOut + @c dIn - a span that varies radially
            // around it, re-scaling the drop fade, the orientation mix and the
            // trail cut across the WHOLE region. Measured on a 520x360 outer
            // with a 0x0 inner at its centre: 23% of lit pixels lost, with
            // differences 300 px away from a shape that has no size.
            //
            // @ref DropletsRenderer::setupGeometry already reaches the right
            // conclusion for such an inner shape - its inscribed box comes out
            // empty, so it cuts no hole - and this is what stops the two
            // disagreeing. Note the test is only for DEGENERACY: a small but
            // real inner shape is a real boundary and is honoured here, even
            // when it is too small for the geometry to bother cutting around.
            const bool hasInner = d.hasInner &&
                                  inner.halfSize.x > 0.0f &&
                                  inner.halfSize.y > 0.0f;

            return {ShapeOf(d.outer), inner, hasInner};
        }

        /// Droplet grid pitch in px, before @c lanes divides it.
        ///
        /// Pitch has to be a GLOBAL scalar, never a local one: the droplet
        /// field is hashed in screen space and every region-relative term in
        /// the shader is an amplitude, never a position, precisely so the grid
        /// never shears. A pitch that tracked the region would shear it - and
        /// the region's local width is not even constant once the two shapes
        /// stop being concentric.
        inline float GetDropPitch(const Config &config)
        {
            return std::max(config.droplets.dropSize, 1.0f) /
                   static_cast<float>(std::max(config.droplets.lanes, 1));
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
        // config rather than the defaults. Either way the geometry exists from
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

        // The region's two shapes, and the flip out of app coordinates (origin
        // top-left, +y down) into framebuffer px, which is the space
        // gl_FragCoord is in. ResolveRegion deliberately does not flip, so the
        // draw geometry it also feeds needs no viewport.
        const Region region = ResolveRegion(config);
        const float flipY = static_cast<float>(viewportHeight);
        const glm::vec2 outerCenter(region.outer.center.x, flipY - region.outer.center.y);
        const glm::vec2 innerCenter(region.inner.center.x, flipY - region.inner.center.y);

        // The geometry is built in OUTER-LOCAL px around the origin (@ref
        // setupGeometry), so the transform is just "put the origin at the outer
        // shape's centre" under a projection that maps px 1:1 onto
        // gl_FragCoord. This is what carries the shapes' positions -
        // setupGeometry reads only their extents and their relative offset,
        // which is why a pure move never re-uploads the buffer.
        //
        // The shader shapes everything from gl_FragCoord, not vPos, so the
        // transform only decides WHICH fragments are rasterised. Every one it
        // drops was discarded by the shader anyway.
        const glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                          0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(outerCenter, 0.0f));

        // Premultiplied-alpha "over" - the region feathers into the existing
        // framebuffer at its discard boundary.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uAmount", config.droplets.amount);
        mShaderProgram.SetUniform("uSpeed", config.droplets.speed);
        mShaderProgram.SetUniform("uTint", config.droplets.tint);

        // The region, as the two rounded rects that bound it - the whole of
        // this layer's geometry, and none of anyone else's.
        mShaderProgram.SetUniform("uOuterCenter", outerCenter);
        mShaderProgram.SetUniform("uOuterHalf", region.outer.halfSize);
        mShaderProgram.SetUniform("uOuterRadius", region.outer.radius);
        mShaderProgram.SetUniform("uInnerCenter", innerCenter);
        mShaderProgram.SetUniform("uInnerHalf", region.inner.halfSize);
        mShaderProgram.SetUniform("uInnerRadius", region.inner.radius);
        mShaderProgram.SetUniform("uHasInner", region.hasInner ? 1 : 0);
        // `lanes` and `dropSize` reach the shader only through the pitch.
        mShaderProgram.SetUniform("uDropPitch", GetDropPitch(config));
        // The one read of another layer's config, and deliberately so: this is
        // a LOOK knob, not geometry. The region is the host's alone, but the
        // feather across its boundary still tracks the glow's softness.
        mShaderProgram.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);

        mVertexArray.DrawArrays(GL_TRIANGLES, mVertexCount);

        mShaderProgram.Unuse();

        // Restore the blend state convention the other renderers leave behind.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void DropletsRenderer::OnConfigChanged(const Config &config)
    {
        // The geometry is sized from the region's two shapes, so it is rebuilt
        // when either moves and left alone otherwise - every other droplet
        // parameter is a per-frame uniform. Dragging Rain Amount or Speed must
        // not re-upload a VBO.
        //
        // Positions are compared along with the rest of each RectGeometry even
        // though setupGeometry reads only extents and the relative offset
        // (Render's transform carries the position instead): one redundant
        // rebuild on a move is cheaper than a gate that silently stops matching
        // if the buffer ever grows a dependency on it.
        //
        // Config::geometry is NOT here, and that is the point: this layer's
        // region is its own, so the shared rect moving is not a reason to
        // rebuild anything.
        const DropletsConfig &d = config.droplets;
        const DropletsConfig &prev = mCurrentConfig.droplets;
        const bool geometryDirty = d.hasInner != prev.hasInner ||
                                   d.outer != prev.outer ||
                                   d.inner != prev.inner;

        mCurrentConfig = config;

        if (geometryDirty)
        {
            setupGeometry(config);
        }
    }

    bool DropletsRenderer::setupShaders()
    {
        // Reuses the standard neon vertex shader (uMVP + aPos -> vPos) so the
        // region's geometry lives in the same local px space as the glow quad.
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::DROPLETS_FRAG_SRC,
                                       "DropletsRenderer");
        return mShaderProgram.IsValid();
    }

    void DropletsRenderer::setupGeometry(const Config &config)
    {
        // A RING of four strips around the region, in OUTER-LOCAL px centred on
        // the origin - not a fullscreen quad, and not a solid one.
        //
        // A perimeter band is a thin rounded ring, so a fullscreen quad spent
        // almost every fragment computing a region coordinate and discarding:
        // on an 800x600 rect in a 1600x1200 viewport, ~1.9M invocations to
        // shade a band of ~100k. Bounding the geometry by the rect took that to
        // ~570k; cutting the interior out takes it to ~140k. Neither changes a
        // drawn pixel - everything removed was discarded by the shader anyway.
        const Region region = ResolveRegion(config);

        // Outer bound. Dilating the outer shape by the furthest distance
        // written gives a shape contained in the axis-aligned box of these
        // half-extents, whatever the corner radius. Floored at zero: a region
        // eroded past its own extent vanishes, and a zero-size quad correctly
        // draws nothing.
        const float ox = std::max(region.outer.halfSize.x + SAFETY_PX, 0.0f);
        const float oy = std::max(region.outer.halfSize.y + SAFETY_PX, 0.0f);

        // Inner bound - the hole, which may now sit ANYWHERE relative to the
        // outer shape, so it is a box with four independent sides rather than a
        // centred pair of half-extents. Under SHARED the two are concentric and
        // the four collapse back to the symmetric pair this used to build.
        bool cut = region.hasInner;
        float hx0 = 0.0f, hx1 = 0.0f, hy0 = 0.0f, hy1 = 0.0f;
        if (cut)
        {
            // The y flip between app and framebuffer coordinates negates the
            // centres' y difference; x is unaffected. That sign is the only
            // part of the transform this has to know about.
            const glm::vec2 delta(region.inner.center.x - region.outer.center.x,
                                  -(region.inner.center.y - region.outer.center.y));
            // The corner inset is for an ARC, so a non-positive radius
            // contributes none. Left signed it would widen the hole past the
            // shape it is meant to sit inside, omitting lit pixels rather than
            // merely wasting fill.
            const float inset = std::max(region.inner.radius, 0.0f) * CORNER_INSET + SAFETY_PX;
            const float hix = region.inner.halfSize.x - inset;
            const float hiy = region.inner.halfSize.y - inset;

            // Clipped to the outer box: a hole that reaches past it removes
            // nothing out there, and a strip with inverted bounds would wind
            // backwards.
            hx0 = std::max(delta.x - hix, -ox);
            hx1 = std::min(delta.x + hix, ox);
            hy0 = std::max(delta.y - hiy, -oy);
            hy1 = std::min(delta.y + hiy, oy);
            cut = hx1 > hx0 && hy1 > hy0;
        }

        if (!cut)
        {
            // Nothing to cut out: a filled shape, a hole that misses the outer
            // box entirely, or one so large it swallows it - a band as wide as
            // the rect, or a deep negative offset. One quad, which is what the
            // ring degenerates to.
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
            -ox,  oy,  -ox, hy1,   ox, hy1,
            -ox,  oy,   ox, hy1,   ox,  oy,
            // bottom strip: full width, below the hole
            -ox, hy0,  -ox, -oy,   ox, -oy,
            -ox, hy0,   ox, -oy,   ox, hy0,
            // left strip: between the two, left of the hole
            -ox, hy1,  -ox, hy0,  hx0, hy0,
            -ox, hy1,  hx0, hy0,  hx0, hy1,
            // right strip: between the two, right of the hole
            hx1, hy1,  hx1, hy0,   ox, hy0,
            hx1, hy1,   ox, hy0,   ox, hy1,
        };
        // clang-format on

        mVertexCount = 24;
        mVertexArray.SetVertexData(ring, sizeof(ring));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }
}
