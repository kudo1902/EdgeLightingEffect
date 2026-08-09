#include "renderer/debug-renderer.h"
#include "util/geometry-utils.h"
#include "util/segment-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    namespace
    {
        /// Glyph ids, mirroring debug-marker.frag's uMarkerShape.
        constexpr int SHAPE_DISC = 0;
        constexpr int SHAPE_CHEVRON = 1;
        constexpr int SHAPE_DIAMOND = 2;

        /// How far off the perimeter the arc / segment glyphs sit, in marker
        /// radii. Colour stops stay ON the line, arcs go outside and segments
        /// inside, so all three families stay legible when shown together.
        constexpr float MARKER_OFFSET_RADII = 2.2f;

        constexpr glm::vec4 ARC_START_COLOR(0.2f, 1.0f, 0.35f, 1.0f);
        constexpr glm::vec4 ARC_END_COLOR(1.0f, 0.25f, 0.2f, 1.0f);
        /// Segment with no stops of its own inherits the base gradient, so
        /// there is no "its" colour to draw - white reads as "inherited".
        constexpr glm::vec4 SEGMENT_DEFAULT_COLOR(1.0f, 1.0f, 1.0f, 1.0f);

        /// An arc this long covers the whole perimeter: its start and end land
        /// on the same point and there is no boundary worth marking twice.
        constexpr float FULL_ARC_LENGTH = 0.999f;

        /// Unit tangent (direction of travel under the config's winding) and
        /// outward normal at perimeter position @p t, in rect-local space.
        /// Central finite difference of the perimeter point - same estimate
        /// GeometryUtils::GetSunFragPosition uses, and exact enough for
        /// placing a glyph.
        inline void PerimeterFrame(float t, const RectGeometry &geom,
                                   glm::vec2 &outTangent, glm::vec2 &outNormal)
        {
            constexpr float EPS = 1e-3f;
            float tPrev = t - EPS;
            float tNext = t + EPS;
            tPrev -= std::floor(tPrev);
            tNext -= std::floor(tNext);

            glm::vec2 delta = GeometryUtils::GetPointOnRectangle(tNext, geom) -
                              GeometryUtils::GetPointOnRectangle(tPrev, geom);
            float len = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            outTangent = (len > 0.0f) ? (delta / len) : glm::vec2(1.0f, 0.0f);

            // The rect is convex about its centre, so the outward normal is
            // whichever perpendicular points away from the local origin.
            outNormal = glm::vec2(outTangent.y, -outTangent.x);
            if (glm::dot(outNormal, GeometryUtils::GetPointOnRectangle(t, geom)) < 0.0f)
            {
                outNormal = -outNormal;
            }
        }
    }
    bool DebugRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link DebugRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        mGradientLUT.Rebuild(mCurrentConfig.neon.colorStops,
                             mCurrentConfig.neon.blendSpace,
                             mCurrentConfig.neon.colorTransitionDuration);

        // Unit quad shared by every marker glyph ([-1,+1] on both axes). Each
        // marker is drawn by scaling (and for chevrons rotating) this quad via
        // uMVP so it lands where it belongs; the marker fragment shader treats
        // vPos in [-1,+1] as glyph space.
        // clang-format off
        float unitQuad[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mMarkerVertexArray.SetVertexData(unitQuad, sizeof(unitQuad));
        mMarkerVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        return true;
    }

    void DebugRenderer::Update(float deltaTime, float, const Config &)
    {
        // Drive the LUT strip's cross-fade off the raw frame delta so a colour
        // change fades in step with the neon renderers' own rings, animation
        // clock paused or not.
        mGradientLUT.Update(deltaTime);
    }

    void DebugRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.debug.enable)
        {
            return;
        }

        // Rect-local -> viewport transform, shared by every overlay below. The
        // y-flip lands the config's top-left origin in GL's bottom-left space.
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        if (config.debug.showWireframe)
        {
            renderWireframe(config, mvp);
        }

        if (config.debug.showGradientLUT)
        {
            renderGradientLUTStrip(config, time, mvp);
        }

        renderMarkers(config, proj, center);

        // Restore a known blend state - the wireframe and the LUT strip both
        // disable blending while they draw.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void DebugRenderer::OnConfigChanged(const Config &config)
    {
        const bool geometryDirty = config.geometry != mCurrentConfig.geometry;

        mCurrentConfig = config;
        if (!mWireframeShader.IsValid())
        {
            return;
        }

        if (geometryDirty)
        {
            setupGeometry(config);
        }

        // Cheap when nothing changed - GradientLUT::Rebuild compares the stops
        // it last baked and returns without touching GL.
        mGradientLUT.Rebuild(config.neon.colorStops, config.neon.blendSpace,
                             config.neon.colorTransitionDuration);
    }

    bool DebugRenderer::setupShaders()
    {
        mWireframeShader = ShaderProgram(ShaderSource::WIREFRAME_VERT_SRC,
                                         ShaderSource::WIREFRAME_FRAG_SRC,
                                         "DebugRenderer.Wireframe");
        // Both overlays below reuse the standard neon vertex shader (uMVP +
        // aPos -> vPos) so their quads land in the same rect-local space the
        // neon renderers draw in.
        mLUTStripShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                        "DebugRenderer.LUTStrip");
        mMarkerShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                      ShaderSource::DEBUG_MARKER_FRAG_SRC,
                                      "DebugRenderer.Marker");
        return mWireframeShader.IsValid() && mLUTStripShader.IsValid() &&
               mMarkerShader.IsValid();
    }

    void DebugRenderer::setupGeometry(const Config &config)
    {
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;

        // Bounding box: 4 corners walked as a GL_LINE_LOOP.
        // clang-format off
        float boxVerts[] = {
            -halfW, halfH, halfW, halfH,
            halfW, -halfH, -halfW, -halfH,
        };
        // clang-format on
        mWireframeVertexArray.SetVertexData(boxVerts, sizeof(boxVerts));
        mWireframeVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        // LUT strip: 60% of rect width x min(rect_height / 6, 40 px), centred
        // on the geometry origin so it sits inside the rounded box.
        float stripHalfW = halfW * 0.6f;
        float stripHalfH = std::min(halfH / 6.0f, 20.0f);
        mLUTStripHalfSize = glm::vec2(stripHalfW, stripHalfH);
        // clang-format off
        float stripVerts[] = {
            -stripHalfW,  stripHalfH,  -stripHalfW, -stripHalfH,   stripHalfW, -stripHalfH,
            -stripHalfW,  stripHalfH,   stripHalfW, -stripHalfH,   stripHalfW,  stripHalfH,
        };
        // clang-format on
        mLUTStripVertexArray.SetVertexData(stripVerts, sizeof(stripVerts));
        mLUTStripVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void DebugRenderer::renderWireframe(const Config &config, const glm::mat4 &mvp)
    {
        // A flat 1 px line loop - blending would only dilute it against the
        // glow it is meant to measure.
        glDisable(GL_BLEND);
        mWireframeShader.Use();
        mWireframeShader.SetUniform("uMVP", mvp);
        mWireframeShader.SetUniform("uColor", config.debug.wireframeColor);
        mWireframeVertexArray.DrawArrays(GL_LINE_LOOP, 4);
        mWireframeShader.Unuse();
        glEnable(GL_BLEND);
    }

    void DebugRenderer::renderGradientLUTStrip(const Config &config, float time, const glm::mat4 &mvp)
    {
        // Overwrites whatever is under the strip rect so the baked ring is
        // readable regardless of the glow's tone-mapped brightness.
        glDisable(GL_BLEND);
        mLUTStripShader.Use();
        mLUTStripShader.SetUniform("uMVP", mvp);
        mLUTStripShader.SetUniform("uStripHalfSize", mLUTStripHalfSize);
        mLUTStripShader.SetUniform("uTime", time);
        mLUTStripShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mGradientLUT.Bind(0);
        mLUTStripShader.SetUniform("uGradientLUT", 0);
        mLUTStripVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mLUTStripShader.Unuse();
    }

    void DebugRenderer::renderMarkers(const Config &config, const glm::mat4 &proj,
                                      const glm::vec2 &center)
    {
        const bool wantStops = config.debug.showColorStops && !config.neon.colorStops.empty();
        const bool wantArcs = config.debug.showArcMarkers && !config.neon.arcs.empty();
        const bool wantSegments = config.debug.showSegmentMarkers;
        if (!wantStops && !wantArcs && !wantSegments)
        {
            return;
        }

        // Straight alpha for the glyphs' anti-aliased edges and white rims.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Scale markers with the smaller half-extent so they stay inside the
        // rect on very tall/thin geometries; cap at 12 px so they are not huge
        // on large rects. One radius for all three families - the shapes, not
        // the sizes, are what tells them apart.
        float radius = std::min(std::min(config.geometry.width, config.geometry.height) * 0.5f * 0.06f, 12.0f);

        mMarkerShader.Use();
        if (wantStops)
        {
            drawColorStopMarkers(config, proj, center, radius);
        }
        if (wantArcs)
        {
            drawArcMarkers(config, proj, center, radius);
        }
        if (wantSegments)
        {
            drawSegmentMarkers(config, proj, center, radius);
        }
        mMarkerShader.Unuse();
    }

    void DebugRenderer::drawColorStopMarkers(const Config &config, const glm::mat4 &proj,
                                             const glm::vec2 &center, float radius)
    {
        // A disc in each stop's colour at its perimeter position, so the raw
        // (position, colour) inputs can be checked against the LUT strip and
        // the on-screen glow.
        for (const ColorStop &stop : config.neon.colorStops)
        {
            glm::vec2 localPt = GeometryUtils::GetPointOnRectangle(stop.position, config.geometry);
            drawMarker(proj, center, localPt, radius, 0.0f, SHAPE_DISC, stop.color);
        }
    }

    void DebugRenderer::drawArcMarkers(const Config &config, const glm::mat4 &proj,
                                       const glm::vec2 &center, float radius)
    {
        // Both chevrons of a pair point INTO the lit span: the start one along
        // the direction of travel, the end one against it. Together with the
        // green/red colouring that makes the arc's extent and direction
        // readable without counting perimeter fractions.
        for (const Arc &arc : config.neon.arcs)
        {
            float start = arc.start - std::floor(arc.start);
            drawArcBound(config, proj, center, radius, start, /*forward=*/true, ARC_START_COLOR);

            if (arc.length < FULL_ARC_LENGTH)
            {
                float end = arc.start + arc.length;
                end -= std::floor(end);
                drawArcBound(config, proj, center, radius, end, /*forward=*/false, ARC_END_COLOR);
            }
        }
    }

    void DebugRenderer::drawArcBound(const Config &config, const glm::mat4 &proj,
                                     const glm::vec2 &center, float radius, float t,
                                     bool forward, const glm::vec4 &color)
    {
        glm::vec2 tangent, normal;
        PerimeterFrame(t, config.geometry, tangent, normal);
        if (!forward)
        {
            tangent = -tangent;
        }

        glm::vec2 localPt = GeometryUtils::GetPointOnRectangle(t, config.geometry) +
                            normal * (radius * MARKER_OFFSET_RADII);
        // The chevron points at +x in glyph space; rotate it onto the tangent.
        float angle = std::atan2(tangent.y, tangent.x);
        drawMarker(proj, center, localPt, radius, angle, SHAPE_CHEVRON, color);
    }

    void DebugRenderer::drawSegmentMarkers(const Config &config, const glm::mat4 &proj,
                                           const glm::vec2 &center, float radius)
    {
        // The merged pool is what the neon renderers actually draw, so mark
        // that rather than either vector on its own.
        SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        for (const SegmentBoost &seg : mEffectiveSegments)
        {
            float t = seg.position - std::floor(seg.position);
            glm::vec2 tangent, normal;
            PerimeterFrame(t, config.geometry, tangent, normal);

            // Inside the perimeter, opposite the arc chevrons.
            glm::vec2 localPt = GeometryUtils::GetPointOnRectangle(t, config.geometry) -
                                normal * (radius * MARKER_OFFSET_RADII);
            glm::vec4 color = seg.colorStops.empty() ? SEGMENT_DEFAULT_COLOR
                                                     : seg.colorStops.front().color;
            drawMarker(proj, center, localPt, radius, 0.0f, SHAPE_DIAMOND, color);
        }
    }

    void DebugRenderer::drawMarker(const glm::mat4 &proj, const glm::vec2 &center,
                                   const glm::vec2 &localPt, float radius, float angle,
                                   int shape, const glm::vec4 &color)
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center + localPt, 0.0f));
        if (angle != 0.0f)
        {
            model = glm::rotate(model, angle, glm::vec3(0.0f, 0.0f, 1.0f));
        }
        model = glm::scale(model, glm::vec3(radius, radius, 1.0f));

        mMarkerShader.SetUniform("uMVP", proj * model);
        mMarkerShader.SetUniform("uMarkerColor", color);
        mMarkerShader.SetUniform("uMarkerShape", shape);
        mMarkerVertexArray.DrawArrays(GL_TRIANGLES, 6);
    }

} // namespace EdgeLighting
