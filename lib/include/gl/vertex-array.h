#ifndef _EDGE_LIGHTING_VERTEX_ARRAY_H_
#define _EDGE_LIGHTING_VERTEX_ARRAY_H_

#include "gl/gl-header.h"

namespace EdgeLighting
{
    class VertexArray
    {
    public:
        VertexArray()
        {
            glGenVertexArrays(1, &mVao);
            glGenBuffers(1, &mVbo);
        }

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
        }

        VertexArray(const VertexArray &) = delete;
        VertexArray &operator=(const VertexArray &) = delete;

        VertexArray(VertexArray &&other) noexcept
            : mVao(other.mVao), mVbo(other.mVbo)
        {
            other.mVao = 0;
            other.mVbo = 0;
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
                mVao = other.mVao;
                mVbo = other.mVbo;
                other.mVao = 0;
                other.mVbo = 0;
            }
            return *this;
        }

        void Bind() const
        {
            glBindVertexArray(mVao);
        }

        void Unbind() const
        {
            glBindVertexArray(0);
        }

        void BindBuffer() const
        {
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
        }

        void SetVertexData(const void *data, size_t size, GLenum usage = GL_STATIC_DRAW) const
        {
            Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferData(GL_ARRAY_BUFFER, size, data, usage);
        }

        void SetAttribPointer(GLuint location, GLint size, GLenum type, GLsizei stride, size_t offset) const
        {
            Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, size, type, GL_FALSE, stride, (void *)offset);
        }

        void DrawArrays(GLenum mode, GLint count, GLint first = 0) const
        {
            Bind();
            glDrawArrays(mode, first, count);
        }

    private:
        GLuint mVao = 0;
        GLuint mVbo = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_VERTEX_ARRAY_H_
