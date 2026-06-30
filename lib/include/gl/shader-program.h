#ifndef _EDGE_LIGHTING_SHADER_PROGRAM_H_
#define _EDGE_LIGHTING_SHADER_PROGRAM_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/// Set to 0 to fall back to per-element indexed uniforms (e.g. "colors[0]",
/// "colors[1]", …) instead of the direct GL array upload (glUniform* v with
/// count > 1). Some restricted GL implementations or wrappers require this.
#ifndef UNIFORM_ARRAY_DIRECT
#define UNIFORM_ARRAY_DIRECT 1
#endif

namespace EdgeLighting
{
    /// RAII wrapper around a GL shader program with uniform caching.
    ///
    /// On top of the usual compile/link/use lifecycle, this class:
    /// - Logs compile/link errors with the shader's @p name for easy attribution.
    /// - Caches @c glGetUniformLocation results so each uniform name is resolved
    ///   exactly once per program lifetime.
    /// - Caches the last value uploaded for every uniform and skips the GL call
    ///   when the new value is identical — substantial savings when a renderer
    ///   uploads ~20 uniforms per frame but only a couple actually change.
    /// - Warns once if a uniform name doesn't exist in the program (then stays
    ///   silent on subsequent calls so render loops don't spam the log).
    ///
    /// Caller contract: call @ref Use before any @ref SetUniform.
    class ShaderProgram
    {
    public:
        ShaderProgram() = default;

        /// Compile @p vertSrc + @p fragSrc and link them. @p name (optional) is
        /// only used for log messages — pass the renderer / shader name so
        /// errors are attributable when several programs live in one process.
        ShaderProgram(const char *vertSrc, const char *fragSrc, const char *name = nullptr)
            : mName(name ? name : "unnamed")
        {
            GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc, "vertex", mName.c_str());
            if (!vs)
            {
                return;
            }

            GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc, "fragment", mName.c_str());
            if (!fs)
            {
                glDeleteShader(vs);
                return;
            }

            mId = glCreateProgram();
            glAttachShader(mId, vs);
            glAttachShader(mId, fs);
            glLinkProgram(mId);
            glDeleteShader(vs);
            glDeleteShader(fs);

            GLint ok = 0;
            glGetProgramiv(mId, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[1024];
                GLsizei len = 0;
                glGetProgramInfoLog(mId, sizeof(log), &len, log);
                LOG_E("ShaderProgram[%s] link error:\n%s", mName.c_str(), log);
                glDeleteProgram(mId);
                mId = 0;
                return;
            }

            LOG_I("ShaderProgram[%s] linked (id=%u).", mName.c_str(), mId);
        }

        ~ShaderProgram()
        {
            if (mId != 0)
            {
                glDeleteProgram(mId);
            }
        }

        ShaderProgram(const ShaderProgram &) = delete;
        ShaderProgram &operator=(const ShaderProgram &) = delete;

        ShaderProgram(ShaderProgram &&other) noexcept
            : mId(other.mId),
              mName(std::move(other.mName)),
              mLocations(std::move(other.mLocations)),
              mCacheInt(std::move(other.mCacheInt)),
              mCacheFloat(std::move(other.mCacheFloat)),
              mCacheVec2(std::move(other.mCacheVec2)),
              mCacheVec3(std::move(other.mCacheVec3)),
              mCacheVec4(std::move(other.mCacheVec4)),
              mCacheMat4(std::move(other.mCacheMat4)),
              mCacheArray(std::move(other.mCacheArray))
        {
            other.mId = 0;
        }

        ShaderProgram &operator=(ShaderProgram &&other) noexcept
        {
            if (this != &other)
            {
                if (mId != 0)
                {
                    glDeleteProgram(mId);
                }

                mId = other.mId;
                mName = std::move(other.mName);
                mLocations = std::move(other.mLocations);
                mCacheInt = std::move(other.mCacheInt);
                mCacheFloat = std::move(other.mCacheFloat);
                mCacheVec2 = std::move(other.mCacheVec2);
                mCacheVec3 = std::move(other.mCacheVec3);
                mCacheVec4 = std::move(other.mCacheVec4);
                mCacheMat4 = std::move(other.mCacheMat4);
                mCacheArray = std::move(other.mCacheArray);

                other.mId = 0;
            }
            return *this;
        }

        /// Returns true if the program compiled and linked successfully.
        bool IsValid() const { return mId != 0; }

        const char *GetName() const { return mName.c_str(); }

        GLuint GetId() const { return mId; }

        /// Activates this shader program (@c glUseProgram).
        void Use() const
        {
            glUseProgram(mId);
        }

        /// Deactivates the current shader program (@c glUseProgram(0)).
        void Unuse() const
        {
            glUseProgram(0);
        }

        // -------------------------------------------------------------------
        // Scalar / vector / matrix uniform setters
        // Each caches the last value and skips the upload if unchanged.
        // -------------------------------------------------------------------

        void SetUniform(const char *name, int value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheInt.find(loc);
            if (it != mCacheInt.end() && it->second == value)
            {
                return;
            }

            mCacheInt[loc] = value;
            glUniform1i(loc, value);
        }

        void SetUniform(const char *name, bool value)
        {
            SetUniform(name, value ? 1 : 0);
        }

        void SetUniform(const char *name, float value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheFloat.find(loc);
            if (it != mCacheFloat.end() && it->second == value)
            {
                return;
            }

            mCacheFloat[loc] = value;
            glUniform1f(loc, value);
        }

        void SetUniform(const char *name, const glm::vec2 &value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheVec2.find(loc);
            if (it != mCacheVec2.end() && it->second == value)
            {
                return;
            }

            mCacheVec2[loc] = value;
            glUniform2f(loc, value.x, value.y);
        }

        void SetUniform(const char *name, const glm::vec3 &value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheVec3.find(loc);
            if (it != mCacheVec3.end() && it->second == value)
            {
                return;
            }

            mCacheVec3[loc] = value;
            glUniform3f(loc, value.x, value.y, value.z);
        }

        void SetUniform(const char *name, const glm::vec4 &value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheVec4.find(loc);
            if (it != mCacheVec4.end() && it->second == value)
            {
                return;
            }

            mCacheVec4[loc] = value;
            glUniform4f(loc, value.r, value.g, value.b, value.a);
        }

        void SetUniform(const char *name, const glm::mat4 &value)
        {
            GLint loc = getLocation(name);
            if (loc < 0)
            {
                return;
            }

            auto it = mCacheMat4.find(loc);
            if (it != mCacheMat4.end() && it->second == value)
            {
                return;
            }

            mCacheMat4[loc] = value;
            glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(value));
        }

        // -------------------------------------------------------------------
        // Array uniform setters
        // Cached by memcmp on the value buffer — collapses N per-element calls
        // into one and skips the upload when the array hasn't changed.
        // -------------------------------------------------------------------

        void SetUniform(const char *name, const float *values, int count)
        {
#if UNIFORM_ARRAY_DIRECT
            GLint loc = getLocation(name);
            if (loc < 0 || count <= 0)
            {
                return;
            }

            if (!isArrayChanged(loc, values, sizeof(float) * static_cast<size_t>(count)))
            {
                return;
            }

            glUniform1fv(loc, count, values);
#else
            if (count <= 0)
            {
                return;
            }

            for (int i = 0; i < count; ++i)
            {
                char elemName[128];
                snprintf(elemName, sizeof(elemName), "%s[%d]", name, i);
                SetUniform(elemName, values[i]);
            }
#endif
        }

        void SetUniform(const char *name, const glm::vec2 *values, int count)
        {
#if UNIFORM_ARRAY_DIRECT
            GLint loc = getLocation(name);
            if (loc < 0 || count <= 0)
            {
                return;
            }

            if (!isArrayChanged(loc, values, sizeof(glm::vec2) * static_cast<size_t>(count)))
            {
                return;
            }

            glUniform2fv(loc, count, glm::value_ptr(*values));
#else
            if (count <= 0)
            {
                return;
            }

            for (int i = 0; i < count; ++i)
            {
                char elemName[128];
                snprintf(elemName, sizeof(elemName), "%s[%d]", name, i);
                SetUniform(elemName, values[i]);
            }
#endif
        }

        void SetUniform(const char *name, const glm::vec4 *values, int count)
        {
#if UNIFORM_ARRAY_DIRECT
            GLint loc = getLocation(name);
            if (loc < 0 || count <= 0)
            {
                return;
            }

            if (!isArrayChanged(loc, values, sizeof(glm::vec4) * static_cast<size_t>(count)))
            {
                return;
            }

            glUniform4fv(loc, count, glm::value_ptr(*values));
#else
            if (count <= 0)
            {
                return;
            }

            for (int i = 0; i < count; ++i)
            {
                char elemName[128];
                snprintf(elemName, sizeof(elemName), "%s[%d]", name, i);
                SetUniform(elemName, values[i]);
            }
#endif
        }

    private:
        /// Compile @p src as a @p type shader. Logs the GL info log on failure
        /// with @p stage ("vertex" / "fragment") and @p programName for context.
        /// @return The shader ID, or 0 on failure.
        static GLuint compileShader(GLenum type, const char *src, const char *stage, const char *programName)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &src, nullptr);
            glCompileShader(shader);

            GLint ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024];
                GLsizei len = 0;
                glGetShaderInfoLog(shader, sizeof(log), &len, log);
                LOG_E("ShaderProgram[%s] %s compile error:\n%s", programName, stage, log);
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        /// Look up the location for @p name, caching the result (including -1
        /// for missing uniforms). The first miss logs a warning so renderer
        /// authors notice typos without flooding the log per frame.
        GLint getLocation(const char *name)
        {
            auto it = mLocations.find(name);
            if (it != mLocations.end())
            {
                return it->second;
            }
            GLint loc = glGetUniformLocation(mId, name);
            mLocations.emplace(name, loc);
            if (loc < 0)
            {
                LOG_E("ShaderProgram[%s]: uniform '%s' not found.",
                      mName.c_str(), name);
            }

            return loc;
        }

        /// Update the byte cache for @p location. Returns @c true if the
        /// caller should upload because the bytes differ from the cache.
        bool isArrayChanged(GLint location, const void *data, size_t bytes)
        {
            auto &cached = mCacheArray[location];
            if (cached.size() == bytes && std::memcmp(cached.data(), data, bytes) == 0)
            {
                return false;
            }

            cached.assign(static_cast<const uint8_t *>(data),
                          static_cast<const uint8_t *>(data) + bytes);
            return true;
        }

    private:
        GLuint mId = 0;
        std::string mName;

        // Uniform-location cache: resolves each name exactly once.
        std::unordered_map<std::string, GLint> mLocations;

        // Last-uploaded value caches, keyed by uniform location.
        std::unordered_map<GLint, int> mCacheInt;
        std::unordered_map<GLint, float> mCacheFloat;
        std::unordered_map<GLint, glm::vec2> mCacheVec2;
        std::unordered_map<GLint, glm::vec3> mCacheVec3;
        std::unordered_map<GLint, glm::vec4> mCacheVec4;
        std::unordered_map<GLint, glm::mat4> mCacheMat4;

        // Raw-byte cache for array uniforms.
        std::unordered_map<GLint, std::vector<uint8_t>> mCacheArray;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SHADER_PROGRAM_H_
