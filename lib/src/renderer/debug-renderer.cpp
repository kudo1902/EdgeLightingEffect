#include "renderer/debug-renderer.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace EdgeLighting
{
    namespace
    {
        /// Strip size as a fraction of the rect: 60% of the width, and a sixth
        /// of the height capped at 20 px half-height, so it stays a readable
        /// band inside the rounded box on any geometry.
        constexpr float STRIP_HALF_WIDTH_FRAC = 0.6f;
        constexpr float STRIP_HALF_HEIGHT_DIV = 6.0f;
        constexpr float STRIP_HALF_HEIGHT_MAX = 20.0f;

        /// Marker radius as a fraction of the smaller half-extent, capped so it
        /// is not huge on a large rect. Scaling with the smaller side is what
        /// keeps the discs inside the rect on very tall or thin geometries.
        constexpr float MARKER_RADIUS_FRAC = 0.06f;
        constexpr float MARKER_RADIUS_MAX = 12.0f;
    }

    bool DebugRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link DebugRenderer shaders.");
            return false;
        }
        setupStopMarkerQuad();
        setupGeometry(mCurrentConfig);
        mGradientLUT.Bake(mCurrentConfig.neon.colorStops, mCurrentConfig.neon.blendSpace,
                          mCurrentConfig.neon.gradientLutSize,
                          mCurrentConfig.neon.colorTransitionDuration);
        return true;
    }

    void DebugRenderer::Update(float deltaTime, float, const Config &)
    {
        // Advances this renderer's own copy of the ring cross-fade. It is
        // handed the same deltaTime as the neon renderer's copy and was baked
        // from the same stops, so the two stay in step frame for frame - which
        // is what makes the strip a preview of the glow rather than of some
        // near-miss of it.
        mGradientLUT.Tick(deltaTime);
    }

    void DebugRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.debug.enable)
        {
            return;
        }

        // Nothing selected. Checked before the transform so an idle overlay
        // layer costs a few branches and no matrix work.
        //
        // The box is gated differently from the other two, on purpose. They
        // ANNOTATE the glow - the strip previews the ring it samples, the
        // markers show where its stops land - so with no glow they annotate
        // nothing and are suppressed: neon off, or the fill-only debug mode,
        // which deliberately leaves the opaque silhouette alone on screen so
        // its square corner can be compared against the emission's round one.
        //
        // The box annotates the GEOMETRY, which is there whether or not
        // anything is lit - and seeing the configured extent with the neon off
        // is a good part of what it is for, so it survives both.
        //
        // (`enable` above covers all three but NOT opaqueOnly, which is a mode
        // of the neon renderer that happens to live in the same struct.)
        const bool glowVisible = config.neon.enable && !config.debug.opaqueOnly;
        const bool wantStrip = config.debug.showGradientLUT && glowVisible;
        const bool wantMarkers = config.debug.showColorStops && glowVisible &&
                                 !config.neon.colorStops.empty();
        const bool wantBox = config.debug.showWireframe;
        if (!wantStrip && !wantMarkers && !wantBox)
        {
            return;
        }

        // Full-resolution throughout, whatever the neon layer's resolution
        // scale: these annotate the FINAL composited image, and a debug
        // annotation that inherited the effect's blurring would be a poor one.
        //
        // `center` reaches the screen by two routes - folded into `mvp` for the
        // strip and the box, and added to each stop's perimeter point for the
        // markers - so deriving it once here is what keeps the three overlays
        // aligned with each other and with the glow they annotate. Viewport y runs down in
        // Config but up in the projection, hence the mirror.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        const glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                          0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        const glm::vec2 center(config.geometry.position.x + halfRectW,
                               static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));

        // Box first, so the strip and the markers land on top of it rather
        // than being crossed out by a line drawn through them.
        if (wantBox)
        {
            // Unblended, as the old WireframeRenderer drew it: a 1 px line
            // wants no coverage blending.
            glDisable(GL_BLEND);
            renderWireframe(mvp, config);
        }
        if (wantStrip)
        {
            // Unblended: the strip overwrites the glow so it stays readable.
            glDisable(GL_BLEND);
            renderGradientLUTStrip(mvp, time, config);
        }
        if (wantMarkers)
        {
            // Straight alpha for the discs' anti-aliased edges.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            renderColorStopMarkers(proj, center, halfRectW, halfRectH, config);
        }

        // Restore a known blend state for following renderers (the box and
        // strip passes disable blending).
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void DebugRenderer::OnConfigChanged(const Config &config)
    {
        // Only the geometry sizes the strip; the markers read the config at
        // draw time and need no rebuild at all. Snapshot the dirtiness before
        // mCurrentConfig is overwritten.
        const bool geometryDirty = config.geometry != mCurrentConfig.geometry;

        mCurrentConfig = config;
        if (!mLUTDebugShader.IsValid())
        {
            return;
        }

        if (geometryDirty)
        {
            setupGeometry(config);
        }

        // Self-guarding (see GradientRingLUT::Bake), so it is called
        // unconditionally - which is also what keeps this copy of the ring in
        // step with the neon renderer's: both are offered every config change
        // and both re-bake on exactly the ones that move the gradient.
        mGradientLUT.Bake(config.neon.colorStops, config.neon.blendSpace,
                          config.neon.gradientLutSize, config.neon.colorTransitionDuration);
    }

    bool DebugRenderer::setupShaders()
    {
        // Both reuse the standard neon vertex shader (uMVP + aPos -> vPos), so
        // the strip quad respects the same rect-local transform as the glow and
        // each marker quad can be placed by its own model matrix.
        mLUTDebugShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                        "DebugRenderer.LUTDebug");
        mStopMarkerShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                          ShaderSource::NEON_STOP_MARKER_FRAG_SRC,
                                          "DebugRenderer.StopMarker");
        // The box keeps its own minimal vertex shader (uMVP + aPos, no vPos
        // varying) rather than borrowing the neon one - it is a line list, not
        // a quad, and has nothing to interpolate.
        mWireframeShader = ShaderProgram(ShaderSource::WIREFRAME_VERT_SRC,
                                         ShaderSource::WIREFRAME_FRAG_SRC,
                                         "DebugRenderer.Wireframe");
        return mLUTDebugShader.IsValid() && mStopMarkerShader.IsValid() &&
               mWireframeShader.IsValid();
    }

    void DebugRenderer::setupStopMarkerQuad()
    {
        // Unit quad for the per-stop markers ([-1,+1] on both axes). Each
        // marker is drawn by scaling + translating this quad via uMVP so it
        // lands at that stop's perimeter position; the marker fragment shader
        // treats vPos in [-1,+1] as disc space.
        //
        // Unlike the strip and the box in setupGeometry, this one never
        // changes: the geometry reaches it through the per-marker model
        // matrix, not through the vertices. Hence uploaded once from
        // Initialize and never revisited.
        // clang-format off
        float unitQuad[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mStopMarkerVertexArray.SetVertexData(unitQuad, sizeof(unitQuad));
        mStopMarkerVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void DebugRenderer::setupGeometry(const Config &config)
    {
        // Centred on the geometry origin so it sits inside the rounded box.
        // Full-res: the strip annotates the composited frame, so unlike the
        // neon quad it is never built in the scaled space.
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;
        float stripHalfW = halfW * STRIP_HALF_WIDTH_FRAC;
        float stripHalfH = std::min(halfH / STRIP_HALF_HEIGHT_DIV, STRIP_HALF_HEIGHT_MAX);
        mLUTStripHalfSize = glm::vec2(stripHalfW, stripHalfH);
        // clang-format off
        float stripVerts[] = {
            -stripHalfW,  stripHalfH,  -stripHalfW, -stripHalfH,   stripHalfW, -stripHalfH,
            -stripHalfW,  stripHalfH,   stripHalfW, -stripHalfH,   stripHalfW,  stripHalfH,
        };
        // clang-format on
        mLUTStripVertexArray.SetVertexData(stripVerts, sizeof(stripVerts));
        mLUTStripVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        // Bounding box: four corners, drawn as a GL_LINE_LOOP. Sharp on
        // purpose even when geometry.cornerRadius is set - it shows the extent
        // the config asked for rather than tracing the rounded outline the
        // neon actually draws, which is what makes the two comparable.
        // clang-format off
        float boxVerts[] = {
            -halfW,  halfH,   halfW,  halfH,
             halfW, -halfH,  -halfW, -halfH,
        };
        // clang-format on
        mWireframeVertexArray.SetVertexData(boxVerts, sizeof(boxVerts));
        mWireframeVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void DebugRenderer::renderGradientLUTStrip(const glm::mat4 &mvp, float time, const Config &config)
    {
        // Overwrites the neon output within the strip rect so the baked ring is
        // readable regardless of the glow's tone-mapped brightness, which is
        // why the caller draws it unblended.
        mLUTDebugShader.Use();
        mLUTDebugShader.SetUniform("uMVP", mvp);
        mLUTDebugShader.SetUniform("uStripHalfSize", mLUTStripHalfSize);
        mLUTDebugShader.SetUniform("uTime", time);
        mLUTDebugShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mGradientLUT.Bind(0);
        mLUTDebugShader.SetUniform("uGradientLUT", 0);
        mLUTStripVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mLUTDebugShader.Unuse();
    }

    void DebugRenderer::renderColorStopMarkers(const glm::mat4 &proj, const glm::vec2 &center,
                                               float halfWidth, float halfHeight, const Config &config)
    {
        // Draws a filled disc in each stop's colour at its perimeter position,
        // so the raw (position, colour) inputs can be checked against the LUT
        // strip and the on-screen glow. Uses standard alpha blending for the
        // ring / anti-aliased edge to composite cleanly - the caller sets that
        // blend mode before calling.
        float markerRadius = std::min(std::min(halfWidth, halfHeight) * MARKER_RADIUS_FRAC,
                                      MARKER_RADIUS_MAX);
        mStopMarkerShader.Use();
        for (const auto &stop : config.neon.colorStops)
        {
            glm::vec2 localPt = GeometryUtils::GetPointOnRectangle(stop.position, config.geometry);
            glm::mat4 markerModel =
                glm::translate(glm::mat4(1.0f), glm::vec3(center + localPt, 0.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(markerRadius, markerRadius, 1.0f));
            mStopMarkerShader.SetUniform("uMVP", proj * markerModel);
            mStopMarkerShader.SetUniform("uMarkerColor", stop.color);
            mStopMarkerVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }
        mStopMarkerShader.Unuse();
    }

    void DebugRenderer::renderWireframe(const glm::mat4 &mvp, const Config &config)
    {
        mWireframeShader.Use();
        mWireframeShader.SetUniform("uMVP", mvp);
        mWireframeShader.SetUniform("uColor", config.debug.wireframeColor);
        mWireframeVertexArray.DrawArrays(GL_LINE_LOOP, 4);
        mWireframeShader.Unuse();
    }

} // namespace EdgeLighting
