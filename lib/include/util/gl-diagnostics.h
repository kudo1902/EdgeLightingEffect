#ifndef _EDGE_LIGHTING_GL_DIAGNOSTICS_H_
#define _EDGE_LIGHTING_GL_DIAGNOSTICS_H_

#include "gl/gl-header.h"
#include "util/log-util.h"

namespace EdgeLighting
{
    /// One-shot instrumentation for "the layer is simply not there" bugs on a
    /// device you cannot attach a GL debugger to.
    ///
    /// WHAT THIS IS FOR. A renderer that draws nothing has a small number of
    /// possible causes, and they fall into three groups that need completely
    /// different fixes. The whole point of this header is to tell them apart in
    /// one frame of logs:
    ///
    ///   1. IT NEVER RASTERISED. Culled, scissored out, depth- or
    ///      stencil-rejected, discarded before rasterisation, drawn with no
    ///      vertices, or drawn off-screen. @ref SampleCounter answers this
    ///      directly, and @ref DumpPipelineState says which of them it was.
    ///   2. IT RASTERISED BUT WROTE NOTHING VISIBLE. A colour mask, a blend
    ///      equation the host changed, or a target that is not the one being
    ///      presented. @ref DumpDrawTarget and the blend/mask half of
    ///      @ref DumpPipelineState cover this.
    ///   3. IT WROTE, AND SOMETHING DOWNSTREAM DROPPED IT. The pixels are in
    ///      the framebuffer but the window system composite discards them -
    ///      classically an additive pass whose alpha is 0 over a surface whose
    ///      alpha IS the coverage. @ref ProbePixel is the one that catches
    ///      this, because it reads back what actually landed.
    ///
    /// Nothing here is on the per-frame path. Every entry point is called only
    /// while a dump is armed (see @c EdgeLightingEffect::Diagnose), because
    /// all of them are pipeline stalls: @ref SampleCounter blocks on a query
    /// result and @ref ProbePixel blocks on a readback. Arming this every frame
    /// will halve the frame rate and tell you nothing extra.
    namespace GLDiagnostics
    {
        /// Occlusion-query target. Desktop GL gives a fragment COUNT; GLES 3.0
        /// only offers the boolean "did anything pass", which is all this
        /// header needs anyway. @ref SampleCounter::IsCount says which one the
        /// number came from so a log reader is never left guessing whether "1"
        /// means one fragment or "yes".
#if defined(GL_SAMPLES_PASSED) && !defined(PLATFORM_LINUX) && !defined(PLATFORM_WINDOWS)
        constexpr GLenum QUERY_TARGET = GL_SAMPLES_PASSED;
        constexpr bool QUERY_IS_COUNT = true;
#else
        constexpr GLenum QUERY_TARGET = GL_ANY_SAMPLES_PASSED;
        constexpr bool QUERY_IS_COUNT = false;
#endif

        inline const char *BoolToString(bool v) { return v ? "ON" : "off"; }

        /// Swallow and report whatever errors are already queued.
        ///
        /// Called before the instrumented work as well as after it: GL's error
        /// flag is sticky and shared, so an error the HOST left behind would
        /// otherwise be read back as this library's, and the first dump would
        /// blame the wrong code.
        inline void DrainErrors(const char *label)
        {
            GLenum err = glGetError();
            if (err == GL_NO_ERROR)
            {
                return;
            }

            while (err != GL_NO_ERROR)
            {
                LOG_E("[diag] %s: GL error 0x%x", label, err);
                err = glGetError();
            }
        }

        /// Every piece of host-owned state that can stop a draw from producing
        /// pixels, in one line per group.
        ///
        /// Read it top to bottom against the three groups in the namespace
        /// comment. The first two lines are the ones that erase a WHOLE layer
        /// and so are what to look at first for a missing renderer; the blend
        /// and mask line erases colour rather than geometry.
        inline void DumpPipelineState(const char *label)
        {
            GLint viewport[4] = {0, 0, 0, 0};
            GLint scissorBox[4] = {0, 0, 0, 0};
            GLint cullMode = 0;
            GLint frontFace = 0;
            GLint depthFunc = 0;
            GLboolean depthMask = GL_TRUE;
            GLint stencilFunc = 0;
            GLint stencilRef = 0;
            GLint stencilMask = 0;
            GLint blendSrcRgb = 0;
            GLint blendDstRgb = 0;
            GLint blendSrcAlpha = 0;
            GLint blendDstAlpha = 0;
            GLint blendEqRgb = 0;
            GLint blendEqAlpha = 0;
            GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            GLint program = 0;

            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
            glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
            glGetIntegerv(GL_FRONT_FACE, &frontFace);
            glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
            glGetIntegerv(GL_STENCIL_REF, &stencilRef);
            glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilMask);
            glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
            glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);

            // GROUP 1 - can delete an entire layer's geometry.
            LOG_E("[diag] %s: cull=%s mode=0x%x front=0x%x (0x%x=CCW) | "
                  "rasterizerDiscard=%s",
                  label,
                  BoolToString(glIsEnabled(GL_CULL_FACE) == GL_TRUE), cullMode, frontFace, GL_CCW,
                  BoolToString(glIsEnabled(GL_RASTERIZER_DISCARD) == GL_TRUE));
            LOG_E("[diag] %s: depth=%s func=0x%x mask=%d | stencil=%s func=0x%x ref=%d mask=0x%x | "
                  "scissor=%s box=(%d,%d %dx%d)",
                  label,
                  BoolToString(glIsEnabled(GL_DEPTH_TEST) == GL_TRUE), depthFunc, (int)depthMask,
                  BoolToString(glIsEnabled(GL_STENCIL_TEST) == GL_TRUE), stencilFunc, stencilRef, stencilMask,
                  BoolToString(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE),
                  scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);

            // GROUP 2 - geometry survives, colour does not.
            LOG_E("[diag] %s: blend=%s rgb(0x%x,0x%x) alpha(0x%x,0x%x) eq(0x%x,0x%x) | "
                  "colorMask=%d%d%d%d | viewport=(%d,%d %dx%d) | program=%d",
                  label,
                  BoolToString(glIsEnabled(GL_BLEND) == GL_TRUE),
                  blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha, blendEqRgb, blendEqAlpha,
                  (int)colorMask[0], (int)colorMask[1], (int)colorMask[2], (int)colorMask[3],
                  viewport[0], viewport[1], viewport[2], viewport[3], program);
        }

        /// What is bound for drawing, and whether it has an alpha channel.
        ///
        /// The alpha size is the interesting number and it is easy to skip
        /// past. A surface with 0 alpha bits cannot carry coverage at all, so a
        /// premultiplied additive pass is safe on it; a surface WITH alpha,
        /// composited by a window system that treats that alpha as coverage,
        /// silently deletes any pass that writes colour at alpha 0. That is the
        /// group-3 failure in the namespace comment, and this line plus
        /// @ref ProbePixel is how to confirm it.
        inline void DumpDrawTarget(const char *label)
        {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

            // The attachment enum differs between the default framebuffer and
            // an FBO, and between desktop GL and GLES for the default one.
            GLenum attachment = GL_COLOR_ATTACHMENT0;
            if (fbo == 0)
            {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
                attachment = GL_BACK_LEFT;
#else
                attachment = GL_BACK;
#endif
            }

            GLint redBits = -1;
            GLint alphaBits = -1;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alphaBits);
            // Those queries are allowed to fail on some drivers for the default
            // framebuffer. A failure leaves the -1 sentinels above, which is
            // information too, so clear the flag rather than reporting it.
            while (glGetError() != GL_NO_ERROR)
            {
            }

            const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            LOG_E("[diag] %s: drawFbo=%d status=0x%x (0x%x=COMPLETE) redBits=%d alphaBits=%d "
                  "(-1 = driver would not say)",
                  label, fbo, status, GL_FRAMEBUFFER_COMPLETE, redBits, alphaBits);
        }

        /// Read one pixel back out of the bound draw framebuffer.
        ///
        /// THE DECISIVE MEASUREMENT for a layer that is missing on screen but
        /// passed its sample count. If the colour is here, the library did its
        /// job and whatever loses the light is downstream of this library
        /// entirely - the window system composite, a hardware overlay plane, a
        /// surface whose alpha is being used as coverage. If it is zero, the
        /// loss is upstream and the state dump above says where.
        ///
        /// @param x,y In APP coordinates (origin top-left, +y down), the space
        ///            @c SpotLight::position and @c RectGeometry::position use.
        ///            Flipped here into GL's bottom-left window convention, so
        ///            a caller can pass a lamp position straight in.
        /// @param viewportHeight The height the frame was rendered at, which is
        ///            what the flip needs.
        inline void ProbePixel(const char *label, int x, int y, int viewportHeight)
        {
            const int glY = viewportHeight - 1 - y;
            if (x < 0 || glY < 0 || y < 0 || y >= viewportHeight)
            {
                LOG_E("[diag] %s: probe (%d,%d) is outside the %d px tall viewport - not read",
                      label, x, y, viewportHeight);
                return;
            }

            unsigned char px[4] = {0, 0, 0, 0};
            // The scissor does not clip glReadPixels, and neither does the
            // viewport, so nothing has to be turned off around this.
            glReadPixels(x, glY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            DrainErrors("ProbePixel");

            LOG_E("[diag] %s: pixel at app(%d,%d) = rgba(%u,%u,%u,%u)%s",
                  label, x, y, px[0], px[1], px[2], px[3],
                  (px[0] + px[1] + px[2] > 0 && px[3] == 0)
                      ? "  <-- COLOUR PRESENT AT ALPHA 0: a compositor using alpha as "
                        "coverage will drop this"
                      : "");
        }

        /// Wraps a draw in an occlusion query: did this produce any fragments?
        ///
        /// Answers the single most useful question about a missing layer, and
        /// it is a question no amount of state dumping settles on its own -
        /// state tells you what COULD have gone wrong, this tells you whether
        /// anything actually reached the framebuffer.
        ///
        /// Counts every fragment the scope emits, INCLUDING any the renderer
        /// wrote into an offscreen buffer of its own (the neon's emission bake,
        /// a resolution-scaled buffer). So a non-zero result means "this
        /// renderer rasterised something somewhere", not "this renderer put
        /// something on the target". Pair it with @ref ProbePixel, which does
        /// answer the second question.
        ///
        /// Blocks on the result in @ref End. One-shot use only.
        class SampleCounter
        {
        public:
            explicit SampleCounter(bool active)
                : mActive(active)
            {
                if (!mActive)
                {
                    return;
                }
                glGenQueries(1, &mQuery);
                glBeginQuery(QUERY_TARGET, mQuery);
            }

            ~SampleCounter()
            {
                if (mQuery != 0)
                {
                    glDeleteQueries(1, &mQuery);
                }
            }

            SampleCounter(const SampleCounter &) = delete;
            SampleCounter &operator=(const SampleCounter &) = delete;

            /// End the query and log the result. Idempotent.
            void End(const char *label)
            {
                if (!mActive)
                {
                    return;
                }
                mActive = false;

                glEndQuery(QUERY_TARGET);

                GLuint result = 0;
                glGetQueryObjectuiv(mQuery, GL_QUERY_RESULT, &result);

                if (QUERY_IS_COUNT)
                {
                    LOG_E("[diag] %s: fragments rasterised = %u%s", label, result,
                          result == 0 ? "  <-- NOTHING REACHED THE RASTERISER" : "");
                }
                else
                {
                    LOG_E("[diag] %s: any samples passed = %s%s", label, result ? "YES" : "NO",
                          result == 0 ? "  <-- NOTHING REACHED THE RASTERISER" : "");
                }
            }

        private:
            bool mActive = false;
            GLuint mQuery = 0;
        };

    } // namespace GLDiagnostics
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GL_DIAGNOSTICS_H_
