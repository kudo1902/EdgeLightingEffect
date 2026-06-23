#ifndef _EDGE_LIGHTING_VERTEX_ARRAY_H_
#define _EDGE_LIGHTING_VERTEX_ARRAY_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <string>

namespace EdgeLighting
{
    /// RAII wrapper combining an OpenGL VAO and a single VBO.
    ///
    /// Provides convenience methods for uploading vertex data, setting
    /// attribute pointers, and issuing draw calls. Move-only.
    class VertexArray
    {
    public:
        /// Generates a new VAO and VBO.
        /// @param name  Optional label used in log messages (default "unnamed").
        VertexArray(const char *name = nullptr)
            : mName(name ? name : "unnamed")
        {
            glGenVertexArrays(1, &mVao);
            glGenBuffers(1, &mVbo);
            LOG_I("VertexArray[%s] created (vao=%u, vbo=%u).", mName.c_str(), mVao, mVbo);
        }

        /// Deletes the VBO and VAO.
        ~VertexArray()
        {
            if (mVbo != 0)
            {
                glDeleteBuffers(1, &mVbo);
            }

            if (mVao != 0)
            {
                glDeleteVertexArrays(1, &mVao);
            }
            LOG_I("VertexArray[%s] destroyed.", mName.c_str());
        }

        VertexArray(const VertexArray &) = delete;
        VertexArray &operator=(const VertexArray &) = delete;

        VertexArray(VertexArray &&other) noexcept
            : mVao(other.mVao), mVbo(other.mVbo), mName(std::move(other.mName))
        {
            other.mVao = 0;
            other.mVbo = 0;
            LOG_I("VertexArray[%s] moved (vao=%u, vbo=%u).", mName.c_str(), mVao, mVbo);
        }

        VertexArray &operator=(VertexArray &&other) noexcept
        {
            if (this != &other)
            {
                if (mVbo != 0)
                {
                    glDeleteBuffers(1, &mVbo);
                }

                if (mVao != 0)
                {
                    glDeleteVertexArrays(1, &mVao);
                }
                LOG_I("VertexArray[%s] move-assign replaced (vao=%u->%u, vbo=%u->%u).",
                      mName.c_str(), mVao, other.mVao, mVbo, other.mVbo);
                mVao = other.mVao;
                mVbo = other.mVbo;
                mName = std::move(other.mName);
                other.mVao = 0;
                other.mVbo = 0;
            }
            return *this;
        }

        /// Binds the VAO.
        void Bind() const
        {
            glBindVertexArray(mVao);
        }

        /// Unbinds the current VAO.
        void Unbind() const
        {
            glBindVertexArray(0);
        }

        /// Binds the internal VBO to @c GL_ARRAY_BUFFER.
        /// Required before calling @c glBufferSubData on this buffer.
        void BindBuffer() const
        {
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
        }

        /// Uploads vertex data to the VBO (binds VAO internally).
        /// @param data  Source data (nullptr to allocate without uploading).
        /// @param size  Size in bytes.
        /// @param usage  GL_STATIC_DRAW, GL_DYNAMIC_DRAW, etc.
        void SetVertexData(const void *data, size_t size, GLenum usage = GL_STATIC_DRAW) const
        {
            Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferData(GL_ARRAY_BUFFER, size, data, usage);
        }

        /// Configures a vertex attribute pointer (enables it automatically).
        /// @param location  Shader layout location index.
        /// @param size      Number of components (1-4).
        /// @param type      GL_FLOAT, etc.
        /// @param stride    Byte stride between consecutive vertices.
        /// @param offset    Byte offset to the first attribute value.
        void SetAttribPointer(GLuint location, GLint size, GLenum type, GLsizei stride, size_t offset) const
        {
            Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, size, type, GL_FALSE, stride, (void *)offset);
        }

        /// Binds the VAO and issues @c glDrawArrays.
        void DrawArrays(GLenum mode, GLint count, GLint first = 0) const
        {
            Bind();
            glDrawArrays(mode, first, count);
        }

    private:
        GLuint mVao = 0; ///< Vertex Array Object handle.
        GLuint mVbo = 0; ///< Vertex Buffer Object handle.
        std::string mName = "unnamed";
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_VERTEX_ARRAY_H_
