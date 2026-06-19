#ifndef _EDGE_LIGHTING_SHADER_PROGRAM_H_
#define _EDGE_LIGHTING_SHADER_PROGRAM_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace EdgeLighting
{
    /// RAII wrapper around an OpenGL shader program object.
    ///
    /// Compiles a vertex + fragment shader pair and links them into a program
    /// on construction. Provides typed uniform setters and move-only ownership.
    class ShaderProgram
    {
    public:
        /// Default constructor — creates an invalid (null) program.
        ShaderProgram() = default;

        /// Compiles vertex and fragment shaders and links them into a program.
        /// @param vertSrc  Vertex shader source string.
        /// @param fragSrc  Fragment shader source string.
        /// Logs errors through @ref LOG_E on failure; resulting program is invalid.
        ShaderProgram(const char *vertSrc, const char *fragSrc)
        {
            GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
            if (!vs)
            {
                LOG_E("ShaderProgram: vertex shader compilation failed.");
                return;
            }

            GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
            if (!fs)
            {
                LOG_E("ShaderProgram: fragment shader compilation failed.");
                glDeleteShader(vs);
                return;
            }

            mId = glCreateProgram();
            glAttachShader(mId, vs);
            glAttachShader(mId, fs);
            glLinkProgram(mId);
            glDeleteShader(vs);
            glDeleteShader(fs);

            GLint ok;
            glGetProgramiv(mId, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[512];
                glGetProgramInfoLog(mId, 512, nullptr, log);
                LOG_E("Program link error:\n%s", log);
                glDeleteProgram(mId);
                mId = 0;
            }
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
            : mId(other.mId)
        {
            other.mId = 0;
        }

        ShaderProgram &operator=(ShaderProgram &&other) noexcept
        {
            if (this != &other)
            {
                if (mId != 0)
                    glDeleteProgram(mId);
                mId = other.mId;
                other.mId = 0;
            }
            return *this;
        }

        /// Returns true if the program compiled and linked successfully.
        bool IsValid() const { return mId != 0; }

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

        /// @name Typed uniform setters
        /// Each resolves the uniform location and uploads the value.
        ///@{
        void SetUniform(const char *name, int value) const
        {
            glUniform1i(glGetUniformLocation(mId, name), value);
        }

        void SetUniform(const char *name, float value) const
        {
            glUniform1f(glGetUniformLocation(mId, name), value);
        }

        void SetUniform(const char *name, const glm::vec2 &value) const
        {
            glUniform2f(glGetUniformLocation(mId, name), value.x, value.y);
        }

        void SetUniform(const char *name, const glm::vec4 &value) const
        {
            glUniform4f(glGetUniformLocation(mId, name), value.r, value.g, value.b, value.a);
        }

        void SetUniform(const char *name, const glm::mat4 &value) const
        {
            glUniformMatrix4fv(glGetUniformLocation(mId, name), 1, GL_FALSE, glm::value_ptr(value));
        }

        /// Uploads a tightly packed array of vec2 values to a uniform array.
        void SetUniform(const char *name, const glm::vec2 *values, int count) const
        {
            if (count <= 0) return;
            glUniform2fv(glGetUniformLocation(mId, name), count, glm::value_ptr(*values));
        }
        ///@}

    private:
        /// Compiles a single shader from source.
        /// @return The shader ID, or 0 on failure.
        static GLuint compileShader(GLenum type, const char *src)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &src, nullptr);
            glCompileShader(shader);
            GLint ok;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[512];
                glGetShaderInfoLog(shader, 512, nullptr, log);
                LOG_E("Shader compile error:\n%s", log);
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        GLuint mId = 0; ///< OpenGL program handle; 0 = invalid.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SHADER_PROGRAM_H_
