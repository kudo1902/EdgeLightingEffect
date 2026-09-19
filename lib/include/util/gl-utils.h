#ifndef _EDGE_LIGHTING_GL_UTILS_H_
#define _EDGE_LIGHTING_GL_UTILS_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <string>
#include <vector>

namespace EdgeLighting
{
    namespace GLUtils
    {
        /// Return all available extensions as a vector of strings.
        inline std::vector<std::string> GetExtensions()
        {
            std::vector<std::string> result;
            GLint numExts = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &numExts);
            result.reserve(static_cast<size_t>(numExts));
            for (GLint i = 0; i < numExts; ++i)
            {
                const char *ext = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, i));
                if (ext)
                {
                    result.emplace_back(ext);
                }
            }
            return result;
        }

        /// Check whether a named GL extension is supported by the current context.
        inline bool CheckExtension(const char *name)
        {
            if (!name)
            {
                return false;
            }
            for (const auto &ext : GetExtensions())
            {
                if (ext == name)
                {
                    return true;
                }
            }
            return false;
        }

        /// Log all available extensions (optionally filtered by substring match).
        /// Pass an empty string to log every extension.
        inline void LogExtensions(const char *filter = "")
        {
            std::string f(filter ? filter : "");
            std::vector<std::string> all = GetExtensions();
            LOG_I("--- Extensions (%zu total) ---", all.size());
            for (const auto &ext : all)
            {
                if (f.empty() || ext.find(f) != std::string::npos)
                {
                    LOG_I("  %s", ext.c_str());
                }
            }
        }

        /// Print driver / renderer info strings to the log.
        inline void LogRendererInfo()
        {
            const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
            const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
            const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
            const char *glslVer = reinterpret_cast<const char *>(glGetString(GL_SHADING_LANGUAGE_VERSION));

            LOG_I("--- GL info ---");
            LOG_I("Vendor  : %s", vendor ? vendor : "?");
            LOG_I("Renderer: %s", renderer ? renderer : "?");
            LOG_I("Version : %s", version ? version : "?");
            LOG_I("GLSL    : %s", glslVer ? glslVer : "?");

            GLint numExts = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &numExts);
            LOG_I("Extensions: %d", numExts);
            LOG_I("---------------");
        }

        /// Check and log any pending GL error. Returns true if no error.
        /// @param context  Optional label for the log message (e.g. the calling function name).
        /// Prefer the GL_CHECK_ERROR macro which automatically includes file + line.
        inline bool CheckGLError(const char *context = nullptr)
        {
            GLenum err = glGetError();
            if (err == GL_NO_ERROR)
            {
                return true;
            }

            const char *label = context ? context : "?";
            const char *desc = "unknown";
            switch (err)
            {
            case GL_INVALID_ENUM:
                desc = "GL_INVALID_ENUM";
                break;
            case GL_INVALID_VALUE:
                desc = "GL_INVALID_VALUE";
                break;
            case GL_INVALID_OPERATION:
                desc = "GL_INVALID_OPERATION";
                break;
            case GL_OUT_OF_MEMORY:
                desc = "GL_OUT_OF_MEMORY";
                break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                desc = "GL_INVALID_FRAMEBUFFER_OPERATION";
                break;
            }

            // GLES 3.0 / desktop 3.3 may not define GL_STACK_* errors, so skip them.

            LOG_E("[%s] GL error: %s (0x%x)", label, desc, err);

            // Flush any additional queued errors.
            while ((err = glGetError()) != GL_NO_ERROR)
            {
                LOG_E("[%s] GL error (chained): 0x%x", label, err);
            }
            return false;
        }

        /// Macro wrapper that passes file:line as context automatically.
#define GL_UTILS_STR(x) #x
#define GL_UTILS_XSTR(x) GL_UTILS_STR(x)
#define GL_CHECK_ERROR() ::EdgeLighting::GLUtils::CheckGLError(__FILE__ ":" GL_UTILS_XSTR(__LINE__))

        /// Return the value of a GL integer cap (e.g. GL_MAX_TEXTURE_SIZE).
        inline int GetCap(GLenum cap)
        {
            GLint v = 0;
            glGetIntegerv(cap, &v);
            return static_cast<int>(v);
        }

        /// Print common GL caps to the log.
        inline void LogCaps()
        {
            LOG_I("--- GL caps ---");
            LOG_I("Max texture size       : %d", GetCap(GL_MAX_TEXTURE_SIZE));
            LOG_I("Max vertex attribs     : %d", GetCap(GL_MAX_VERTEX_ATTRIBS));
            LOG_I("Max uniform components : %d", GetCap(GL_MAX_VERTEX_UNIFORM_COMPONENTS));
            LOG_I("Max vertex out comps   : %d", GetCap(GL_MAX_VERTEX_OUTPUT_COMPONENTS));
            LOG_I("Max fragment in comps  : %d", GetCap(GL_MAX_FRAGMENT_INPUT_COMPONENTS));
            LOG_I("Max combined tex units : %d", GetCap(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS));
            LOG_I("Max draw buffers       : %d", GetCap(GL_MAX_DRAW_BUFFERS));
            LOG_I("Max renderbuffer size  : %d", GetCap(GL_MAX_RENDERBUFFER_SIZE));
            GLint vpDims[2] = {0, 0};
            glGetIntegerv(GL_MAX_VIEWPORT_DIMS, vpDims);
            LOG_I("Max viewport dims      : %d x %d", vpDims[0], vpDims[1]);
#if defined(GL_MAX_SAMPLES)
            LOG_I("Max samples            : %d", GetCap(GL_MAX_SAMPLES));
#endif
            LOG_I("----------------");
        }

        /// Turns @c GL_SCISSOR_TEST off for the duration of a scope and puts
        /// the host's setting back afterwards.
        ///
        /// For one situation, and it is not "this pass would rather not be
        /// clipped": a pass rendering into an OFFSCREEN BUFFER OF ITS OWN.
        ///
        /// The scissor box is in the caller's window coordinates, and an
        /// internal buffer is not in that space - it is either a different
        /// size (a resolution-scaled copy of the viewport) or an entirely
        /// different quantity (@c NeonRenderer's N x 2 emission table, whose
        /// axes are sample index and row). Leaving the test on therefore does
        /// not clip such a pass, it CORRUPTS it: the box lands on unrelated
        /// texels, the clear and the draw skip everything outside it, and what
        /// survives is whatever the buffer held last frame. On the emission
        /// table, whose height is 2, any box with a y origin above 1 discards
        /// the entire bake.
        ///
        /// The host's clip is not lost by doing this. It still applies to the
        /// draw that composites the buffer back onto the caller's framebuffer,
        /// which is the one draw that IS in the caller's coordinate space and
        /// so the only place the box means what it says. Passes that draw
        /// straight onto the caller's framebuffer - the opaque fill, the
        /// unscaled gather, every debug overlay - must NOT use this; their
        /// clipping is exactly what the host asked for. (@c renderOpaqueFill
        /// goes further and intersects its clear box with the host's, because
        /// a clear is not clipped by the viewport the way its draw was.)
        ///
        /// Costs one @c glIsEnabled, a static-state query, and touches nothing
        /// when the host had no scissor. The box itself is never written, so
        /// there is none to put back.
        class NoScissorScope
        {
        public:
            /// @param active pass @c false to make the whole thing a no-op,
            ///        for a pass that only sometimes retargets. The query is
            ///        short-circuited too, so the non-retargeting path pays
            ///        nothing at all.
            explicit NoScissorScope(bool active = true)
                : mRestore(active && glIsEnabled(GL_SCISSOR_TEST))
            {
                if (mRestore)
                {
                    glDisable(GL_SCISSOR_TEST);
                }
            }

            ~NoScissorScope() { Restore(); }

            NoScissorScope(const NoScissorScope &) = delete;
            NoScissorScope &operator=(const NoScissorScope &) = delete;

            /// End the scope early. Idempotent, and the destructor calls it -
            /// this is for the case where the composite that has to see the
            /// host's clip lives in the same scope as the offscreen work, so
            /// the guard cannot simply be allowed to fall off the end.
            void Restore()
            {
                if (mRestore)
                {
                    glEnable(GL_SCISSOR_TEST);
                    mRestore = false;
                }
            }

        private:
            bool mRestore;
        };

        /// Turns @c GL_CULL_FACE off for the duration of a scope and puts the
        /// host's setting back afterwards.
        ///
        /// Taken ONCE per frame, by @c EdgeLightingEffect::Render, around the
        /// whole renderer fan-out. Not tidiness and not a preference: nothing
        /// this library draws is meant to be face-culled - every layer is a
        /// flat screen-space quad, ring or strip with no back side to hide - so
        /// a cull state can only ever delete pixels that were meant to be
        /// there.
        ///
        /// Two measurements, offscreen at 640x360, are what put it there:
        ///
        ///   - With GL_CULL_FACE on and the GL defaults (GL_BACK / GL_CCW), the
        ///     spotlight rendered 0 lit pixels while every other layer was
        ///     unchanged to the pixel. @c SpotlightRenderer draws through a
        ///     y-FLIPPED ortho so its VBO can hold app coordinates verbatim,
        ///     and a negative determinant reverses winding, which made it the
        ///     one back-facing layer here. (That is now also fixed at the
        ///     source, in @c SpotlightRenderer::buildStrips, so the strips are
        ///     correct on their own terms and not merely because of this
        ///     guard.)
        ///   - With culling on and the host's winding order reversed to GL_CW,
        ///     EVERY layer but the debug bounding box rendered 0 pixels. The
        ///     box survives only because it is a GL_LINE_LOOP, and culling does
        ///     not apply to lines. Winding the geometry correctly is therefore
        ///     not sufficient on its own: @c glCullFace and @c glFrontFace
        ///     belong to the host as much as @c GL_CULL_FACE does.
        ///
        /// Why a host would have culling on at all: the GL context is not
        /// always this library's alone. On an embedded surface view (Tizen
        /// Evas_GL, Android GLSurfaceView) a video pipeline or web engine
        /// shares it and leaves its own state behind, and none of this shows up
        /// on a desktop demo where GL_CULL_FACE is off by default and nothing
        /// ever enables it.
        ///
        /// Costs one @c glIsEnabled, a static-state query, and touches nothing
        /// when the host had no culling. The cull MODE and winding order are
        /// never written, so there is none to put back.
        class NoCullScope
        {
        public:
            /// @param active pass @c false to make the whole thing a no-op,
            ///        matching @ref NoScissorScope's signature.
            explicit NoCullScope(bool active = true)
                : mRestore(active && glIsEnabled(GL_CULL_FACE))
            {
                if (mRestore)
                {
                    glDisable(GL_CULL_FACE);
                }
            }

            ~NoCullScope() { Restore(); }

            NoCullScope(const NoCullScope &) = delete;
            NoCullScope &operator=(const NoCullScope &) = delete;

            /// End the scope early. Idempotent, and the destructor calls it.
            void Restore()
            {
                if (mRestore)
                {
                    glEnable(GL_CULL_FACE);
                    mRestore = false;
                }
            }

        private:
            bool mRestore;
        };

        /// Forces the three pieces of pipeline state every layer here ASSUMES
        /// but none of them ever sets, and puts the host's back afterwards:
        /// a full colour+alpha write mask, a @c GL_FUNC_ADD blend equation,
        /// and depth test and depth writes off.
        ///
        /// Taken ONCE per frame by @c EdgeLightingEffect::Render, alongside
        /// @ref NoCullScope and for the same reason: on a shared surface view
        /// (Tizen Evas_GL, Android GLSurfaceView) a video pipeline or web
        /// engine draws into the same context and leaves its own state behind,
        /// and none of it reproduces on a desktop demo where every one of
        /// these is still at its GL default.
        ///
        /// What each one costs if it is wrong:
        ///
        ///   - COLOUR MASK. Every layer writes a coverage alpha, and on an
        ///     embedded surface that alpha is what decides whether the pixel
        ///     is seen at all - the compositor or hardware video plane
        ///     finishes the frame with it. A host that left alpha writes
        ///     masked off turns all of it into a silent no-op: the colour is
        ///     there, the alpha is whatever was in the buffer, and the layer
        ///     is invisible over video with nothing in the log. This is the
        ///     specific failure that would defeat spotlight.frag's coverage
        ///     alpha, so it is the reason this scope exists.
        ///   - BLEND EQUATION. This library never calls @c glBlendEquation,
        ///     so every @c glBlendFunc in it is written expecting
        ///     @c GL_FUNC_ADD. A host that left @c GL_MAX or
        ///     @c GL_FUNC_REVERSE_SUBTRACT behind silently reinterprets every
        ///     composite in the pipeline.
        ///   - DEPTH. Every layer is a flat screen-space quad, ring or strip
        ///     at z = 0 with nothing to be in front of or behind, so a depth
        ///     test can only ever delete pixels that were meant to be there -
        ///     the same argument @ref NoCullScope makes for culling - and a
        ///     depth WRITE would corrupt a buffer that belongs to the host.
        ///
        /// Costs four static-state queries per frame, once for the whole
        /// fan-out rather than once per layer, and writes nothing back that it
        /// did not have to change.
        class CompositeStateScope
        {
        public:
            explicit CompositeStateScope(bool active = true)
                : mActive(active)
            {
                if (!mActive)
                {
                    return;
                }

                glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);
                if (!mColorMask[0] || !mColorMask[1] || !mColorMask[2] || !mColorMask[3])
                {
                    mMaskChanged = true;
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                }

                glGetIntegerv(GL_BLEND_EQUATION_RGB, &mEquationRGB);
                glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &mEquationAlpha);
                if (mEquationRGB != GL_FUNC_ADD || mEquationAlpha != GL_FUNC_ADD)
                {
                    mEquationChanged = true;
                    glBlendEquation(GL_FUNC_ADD);
                }

                mDepthTest = (glIsEnabled(GL_DEPTH_TEST) == GL_TRUE);
                if (mDepthTest)
                {
                    glDisable(GL_DEPTH_TEST);
                }

                GLboolean depthMask = GL_FALSE;
                glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
                mDepthWrite = (depthMask == GL_TRUE);
                if (mDepthWrite)
                {
                    glDepthMask(GL_FALSE);
                }
            }

            ~CompositeStateScope() { Restore(); }

            CompositeStateScope(const CompositeStateScope &) = delete;
            CompositeStateScope &operator=(const CompositeStateScope &) = delete;

            /// End the scope early. Idempotent, and the destructor calls it.
            void Restore()
            {
                if (!mActive)
                {
                    return;
                }
                mActive = false;

                if (mMaskChanged)
                {
                    glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
                }
                if (mEquationChanged)
                {
                    glBlendEquationSeparate(static_cast<GLenum>(mEquationRGB),
                                            static_cast<GLenum>(mEquationAlpha));
                }
                if (mDepthTest)
                {
                    glEnable(GL_DEPTH_TEST);
                }
                if (mDepthWrite)
                {
                    glDepthMask(GL_TRUE);
                }
            }

        private:
            bool mActive;
            bool mMaskChanged = false;
            bool mEquationChanged = false;
            bool mDepthTest = false;
            bool mDepthWrite = false;
            GLboolean mColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            GLint mEquationRGB = GL_FUNC_ADD;
            GLint mEquationAlpha = GL_FUNC_ADD;
        };

    } // namespace GLUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GL_UTILS_H_
