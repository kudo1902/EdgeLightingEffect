#include "renderer/debug-renderer.h"
#include "util/geometry-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

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

        /// The buffer view's outline: opaque magenta, so it reads against the
        /// glow and is not mistaken for the green bounding box.
        const glm::vec4 BUFFER_VIEW_BORDER_COLOR(1.0f, 0.0f, 1.0f, 1.0f);

        /// A table's panel (a buffer with no box of the frame): its distance
        /// from the viewport's lower-left corner - whole px, so texel edges
        /// land on pixel edges - the share of the viewport it may fill each
        /// way, and the least height it is stretched to.
        constexpr float PANEL_MARGIN = 16.0f;
        constexpr float PANEL_MAX_FRAC = 0.5f;
        constexpr float PANEL_MIN_HEIGHT = 64.0f;

        /// Whether the LUT strip will be drawn this frame.
        ///
        /// @ref DebugRenderer::renderGradientLUTStrip is the ONLY thing that
        /// samples @c mGradientLUT, so this is also the condition under which
        /// the ring is worth maintaining at all - the bake and the cross-fade
        /// tick both gate on it. This renderer keeps its own ring rather than
        /// borrowing the neon one (that separation is the point - see the
        /// class note), which means every frame of a colour cross-fade blended
        /// and re-uploaded a SECOND ring for an overlay that is off by
        /// default and invisible even when the whole debug layer is on.
        ///
        /// One predicate, shared by the gate and the draw, so the two cannot
        /// drift: a gate that stopped matching would show a stale ring the
        /// moment the strip was switched on, which is exactly the sort of bug
        /// a debug overlay must not have.
        inline bool IsStripVisible(const Config &config)
        {
            return config.debug.enable && config.debug.showGradientLUT &&
                   config.neon.enable && !config.debug.opaqueOnly;
        }
    }

    bool DebugRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link DebugRenderer shaders.");
            return false;
        }
        setupUnitQuad();
        setupGeometry(mCurrentConfig);
        // Unconditional, unlike the bake in OnConfigChanged, and it is the
        // safety net that lets that one be gated: a host whose config never
        // changes after registration gets no OnConfigChanged with a valid
        // shader, so this is the only bake the strip would ever have. Seeding
        // it here costs one ring at startup and makes "the strip is visible"
        // imply "the ring is uploaded" for the life of the renderer.
        mGradientLUT.Bake(mCurrentConfig.neon.colorStops, mCurrentConfig.neon.blendSpace,
                          mCurrentConfig.neon.gradientLutSize,
                          mCurrentConfig.neon.colorTransitionDuration);
        // What that bake was for, so a later reveal is judged against it
        // rather than against the flag's initial value.
        mStripVisible = IsStripVisible(mCurrentConfig);
        return true;
    }

    void DebugRenderer::Update(float deltaTime, float, const Config &config)
    {
        // Advances this renderer's own copy of the ring cross-fade. It is
        // handed the same deltaTime as the neon renderer's copy and was baked
        // from the same stops, so the two stay in step frame for frame - which
        // is what makes the strip a preview of the glow rather than of some
        // near-miss of it.
        //
        // Only while the strip is actually on screen, though. A tick during a
        // fade is a full ring blend plus a texture upload, and with the strip
        // hidden nothing samples the result.
        //
        // Hiding it does not strand the ring, because the bake is gated on the
        // same predicate. A fade in flight when the strip is hidden simply
        // PAUSES - mDisplay holds, and the tick resumes it on re-show. Stops
        // that change while it is hidden are not baked at all, so the re-show
        // is itself a config change that bakes them - and bakes them with no
        // fade, because a cross-fade from a ring nobody has seen for the last
        // however-many seconds is not a preview of anything. See
        // @c mStripVisible. Either way the strip is showing the ring for the
        // current stops from the frame it reappears.
        if (IsStripVisible(config))
        {
            mGradientLUT.Tick(deltaTime);
        }
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
        // (`enable` above covers every overlay but NOT opaqueOnly, which is a mode
        // of the neon renderer that happens to live in the same struct.)
        const bool glowVisible = config.neon.enable && !config.debug.opaqueOnly;
        // The same predicate the ring's bake and tick gate on, so the strip can
        // never be drawn from a ring those two decided not to maintain.
        // (Re-tests debug.enable, which the early return above already
        // guarantees - the helper is self-contained because its other two
        // callers have no such early return.)
        const bool wantStrip = IsStripVisible(config);
        const bool wantMarkers = config.debug.showColorStops && glowVisible &&
                                 !config.neon.colorStops.empty();
        const bool wantBox = config.debug.showWireframe;
        // A buffer some layer drawn before this one marked current this frame.
        // No gate of its own beyond that: a layer that is off, or drew nothing
        // - the neon under opaqueOnly - marks nothing.
        const BufferRegistry *registry = GetBufferRegistry();
        const BufferRecord *buffer =
            registry && !config.debug.bufferView.empty() ? registry->Find(config.debug.bufferView) : nullptr;
        if (buffer && !registry->IsCurrent(*buffer))
        {
            buffer = nullptr;
        }
        if (!wantStrip && !wantMarkers && !wantBox && !buffer)
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

        // The buffer first: in place it replaces the frame inside its box,
        // and the other overlays annotate whatever is under them.
        if (buffer && ensureBufferViewShader())
        {
            glDisable(GL_BLEND);
            renderBufferView(proj, viewportWidth, viewportHeight, *buffer, config);
        }
        // Box next, so the strip and the markers land on top of it rather
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

        // Restore a known blend state for following renderers (the view, box
        // and strip passes disable blending).
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

        // Self-guarding (see GradientRingLUT::Bake), so within a visible strip
        // it is called unconditionally - which is what keeps this copy of the
        // ring in step with the neon renderer's: both are offered every config
        // change and both re-bake on exactly the ones that move the gradient.
        //
        // Gated on the strip being drawn at all, though, because unlike the
        // neon renderer's ring this one has exactly one consumer and it is an
        // overlay that is off by default. A bake is a full ring plus a texture
        // upload; spending that on a hidden strip buys nothing. Becoming
        // visible is itself a config change, so the bake it needs arrives with
        // it - see the note in @ref Update for what a fade does across the gap.
        const bool stripVisible = IsStripVisible(config);
        if (stripVisible)
        {
            // Zero duration on the RISING EDGE, so a re-show lands on the
            // current stops immediately. The gate above means stop changes
            // made while the strip was hidden were never baked, so the
            // catch-up bake arriving here would otherwise cross-fade from
            // whatever was last on screen - a ring that can be arbitrarily
            // stale - and the strip would spend colorTransitionDuration
            // previewing colours the glow settled away from long ago. The
            // strip's whole claim is that it is a preview of the glow.
            //
            // Only ever a snap where there is something new to snap TO: Bake
            // returns early when the gradient inputs have not moved, so a
            // hide/show over an unchanged ring never reaches the duration at
            // all and a fade paused by the hide resumes as before.
            const float fadeDuration = mStripVisible ? config.neon.colorTransitionDuration : 0.0f;
            mGradientLUT.Bake(config.neon.colorStops, config.neon.blendSpace,
                              config.neon.gradientLutSize, fadeDuration);
        }
        mStripVisible = stripVisible;
    }

    bool DebugRenderer::setupShaders()
    {
        // Both reuse the standard neon vertex shader (uMVP + aPos -> vPos), so
        // the strip quad respects the same rect-local transform as the glow and
        // each marker quad can be placed by its own model matrix.
        mLUTDebugShader = BuildProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_LUT_DEBUG_FRAG_SRC,
                                       "DebugRenderer.LUTDebug");
        mStopMarkerShader = BuildProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::NEON_STOP_MARKER_FRAG_SRC,
                                         "DebugRenderer.StopMarker");
        // The box keeps its own minimal vertex shader (uMVP + aPos, no vPos
        // varying) rather than borrowing the neon one - it is a line list, not
        // a quad, and has nothing to interpolate.
        mWireframeShader = BuildProgram(ShaderSource::WIREFRAME_VERT_SRC,
                                        ShaderSource::WIREFRAME_FRAG_SRC,
                                        "DebugRenderer.Wireframe");
        return mLUTDebugShader.IsValid() && mStopMarkerShader.IsValid() &&
               mWireframeShader.IsValid();
    }

    bool DebugRenderer::ensureBufferViewShader()
    {
        if (!mBufferViewShader.IsValid() && !mBufferViewFailed)
        {
            // The neon vertex shader, like the markers: the unit quad scaled
            // onto the box, vPos across it in [-1, +1].
            mBufferViewShader = BuildProgram(ShaderSource::NEON_VERT_SRC,
                                             ShaderSource::DEBUG_BUFFER_VIEW_FRAG_SRC,
                                             "DebugRenderer.BufferView");
            mBufferViewFailed = !mBufferViewShader.IsValid();
            if (mBufferViewFailed)
            {
                LOG_E("Failed to compile/link the DebugRenderer buffer view - it stays off.");
            }
        }
        return mBufferViewShader.IsValid();
    }

    void DebugRenderer::setupUnitQuad()
    {
        // Unit quad ([-1,+1] on both axes) for the per-stop markers and the
        // buffer view. Each marker is drawn by scaling + translating
        // this quad via uMVP so it lands at that stop's perimeter position;
        // the marker fragment shader treats vPos in [-1,+1] as disc space. The
        // view scales it onto its box the same way.
        //
        // Unlike the strip and the box in setupGeometry, this one never
        // changes: the geometry reaches it through each draw's model matrix,
        // not through the vertices. Hence uploaded once from Initialize and
        // never revisited.
        // clang-format off
        float unitQuad[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mUnitQuadVertexArray.SetVertexData(unitQuad, sizeof(unitQuad));
        mUnitQuadVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
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

    void DebugRenderer::renderBufferView(const glm::mat4 &proj, int viewportWidth, int viewportHeight,
                                         const BufferRecord &buffer, const Config &config)
    {
        // In place: the box the producer reads it over, in the caller's
        // framebuffer px, y up - the space @p proj maps. A table maps onto no
        // box, so it gets a panel in the lower-left corner: as large as fits
        // in PANEL_MAX_FRAC of the viewport each way at one scale for both
        // axes, then stretched to PANEL_MIN_HEIGHT so a table a few rows tall
        // (the emission table's 2) still reads.
        //
        // The panel's scale is WHOLE (I72): an integer number of px per texel,
        // or for a table too large for that, an odd integer number of texels
        // per px. A fractional scale gave texel columns of two widths and put
        // pixel centres exactly on texel edges, where the texel shown flipped
        // with the interpolated vPos (947 px, up to 5 levels, on a 128-wide
        // table at 2.8125x). Whole px per texel puts every centre half a px
        // inside a texel; k texels per px puts it at (i + 0.5) k, inside a
        // texel only when k is odd - an even k lands on an edge again.
        glm::vec2 origin = buffer.origin;
        glm::vec2 size = buffer.size;
        if (!buffer.placed)
        {
            const glm::vec2 viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
            const glm::vec2 room = glm::max(viewport * PANEL_MAX_FRAC, glm::vec2(1.0f));
            const glm::vec2 texels = glm::max(glm::vec2(buffer.GetTexels()), glm::vec2(1.0f));
            const float fit = std::min(room.x / texels.x, room.y / texels.y);
            float scale = std::floor(fit);
            if (fit < 1.0f)
            {
                float texelsPerPx = std::ceil(1.0f / fit);
                if (std::fmod(texelsPerPx, 2.0f) == 0.0f)
                {
                    texelsPerPx += 1.0f;
                }
                scale = 1.0f / texelsPerPx;
            }
            size = texels * scale;
            // A few rows stretched on their own, still whole px per row.
            const float rowPx = std::floor(std::min(PANEL_MIN_HEIGHT, room.y) / texels.y);
            if (rowPx > scale)
            {
                size.y = texels.y * rowPx;
            }
            origin = glm::vec2(PANEL_MARGIN);
        }
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(origin + size * 0.5f, 0.0f)) *
                                glm::scale(glm::mat4(1.0f), glm::vec3(size * 0.5f, 1.0f));
        mBufferViewShader.Use();
        mBufferViewShader.SetUniform("uMVP", proj * model);
        mBufferViewShader.SetUniform("uDestSize", size);
        mBufferViewShader.SetUniform("uChannels", buffer.GetChannels());
        mBufferViewShader.SetUniform("uChannel", static_cast<int>(config.debug.bufferViewChannel));
        mBufferViewShader.SetUniform("uGain", config.debug.bufferViewGain);
        mBufferViewShader.SetUniform("uBorderColor", BUFFER_VIEW_BORDER_COLOR);
        // Bound by name: the texture is the producer's, and this layer only
        // samples it.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, buffer.GetTexture());
        mBufferViewShader.SetUniform("uSource", 0);
        mUnitQuadVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBufferViewShader.Unuse();
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
            mUnitQuadVertexArray.DrawArrays(GL_TRIANGLES, 6);
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
