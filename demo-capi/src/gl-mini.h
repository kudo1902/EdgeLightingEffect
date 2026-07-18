#ifndef _EDGE_LIGHTING_CAPI_DEMO_GL_MINI_H_
#define _EDGE_LIGHTING_CAPI_DEMO_GL_MINI_H_

#include <glad/glad.h>

#include <cstdio>
#include <cstring>

// Tiny raw-GL helpers used by the demo-only quads (background checker + image
// backdrop). The C API doesn't expose GL wrappers, so the demo owns its own -
// deliberately minimal, no move-only wrappers, no error tracking beyond
// glGetShaderInfoLog on failure. GLSL version is baked in for the platform.
#if defined(PLATFORM_MACOS)
#define EL_CAPI_GLSL_VERSION "#version 330 core\n"
#else
#define EL_CAPI_GLSL_VERSION "#version 300 es\n"
#endif

namespace EdgeLightingCapiDemo
{
    inline GLuint CompileShader(GLenum type, const char *src, const char *label)
    {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {0};
            glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
            std::fprintf(stderr, "shader compile failed (%s): %s\n", label, log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    inline GLuint LinkProgram(const char *vs, const char *fs, const char *label)
    {
        GLuint v = CompileShader(GL_VERTEX_SHADER, vs, label);
        if (!v)
        {
            return 0;
        }
        GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs, label);
        if (!f)
        {
            glDeleteShader(v);
            return 0;
        }
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {0};
            glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
            std::fprintf(stderr, "program link failed (%s): %s\n", label, log);
            glDeleteProgram(p);
            p = 0;
        }
        glDeleteShader(v);
        glDeleteShader(f);
        return p;
    }
} // namespace EdgeLightingCapiDemo

#endif // _EDGE_LIGHTING_CAPI_DEMO_GL_MINI_H_
