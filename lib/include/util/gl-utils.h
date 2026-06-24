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
#define GL_CHECK_ERROR() ::EdgeLighting::GLUtil::CheckGLError(__FILE__ ":" GL_UTILS_XSTR(__LINE__))

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
            LOG_I("Max varyings           : %d", GetCap(GL_MAX_VARYING_COMPONENTS));
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

    } // namespace GLUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GL_UTILS_H_
