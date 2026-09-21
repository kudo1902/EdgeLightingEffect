#ifndef _EDGE_LIGHTING_VERTEX_ARRAY_H_
#define _EDGE_LIGHTING_VERTEX_ARRAY_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace EdgeLighting
{
    /// RAII wrapper combining an OpenGL VAO and a single VBO.
    ///
    /// Provides convenience methods for uploading vertex data, setting
    /// attribute pointers, and issuing draw calls. Move-only.
    ///
    /// @ref SetVertexData caches the last-uploaded bytes and skips the GL call
    /// when they have not moved - the same thing @c UniformBuffer::SetData
    /// does, and for the same reason. See the note there.
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
            : mVao(other.mVao), mVbo(other.mVbo), mName(std::move(other.mName)),
              mCache(std::move(other.mCache)), mUsage(other.mUsage)
        {
            other.mVao = 0;
            other.mVbo = 0;
            other.mUsage = 0;
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
                mCache = std::move(other.mCache);
                mUsage = other.mUsage;
                other.mVao = 0;
                other.mVbo = 0;
                other.mUsage = 0;
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
        ///
        /// Skips the upload entirely - bind included - when @p data, @p size
        /// and @p usage all match the previous call, because re-specifying a
        /// buffer store with the bytes already in it is pure driver work.
        ///
        /// The gate earns its keep on geometry that is rebuilt from an
        /// ANIMATED gate but does not always move. @c NeonRenderer's glow quad
        /// is the case that prompted it: its dirty set includes @c intensity,
        /// @c lineWidth, @c glowRadius, @c bloomStrength and
        /// @c filamentFalloff, every one of them an @c AnimatableField, so an
        /// @c IntensityPulse rebuilds it 60 times a second - and under
        /// @c GlowSide::INSIDE the outer margin is capped to a CONSTANT
        /// (@c sideCullPx + @c GLOW_EDGE_SAFETY), so all five of those inputs
        /// can sweep their whole range while the six vertices never change by
        /// a bit. Same shape of thing for the fill ring, the droplet band and
        /// the debug box, which are rebuilt from config changes that often
        /// leave their bounds where they were.
        ///
        /// Compared by MEMCMP on the produced bytes rather than by a snapshot
        /// of the values the caller derived them from. A snapshot would be
        /// faster and would rot: it is the same hazard as a @c Config
        /// sub-struct whose @c operator== misses a new field (see AGENTS.md),
        /// except the symptom is stale geometry rather than a missed rebuild.
        /// The bytes cannot drift from themselves.
        ///
        /// @note A skipped call leaves the VAO and @c GL_ARRAY_BUFFER bindings
        ///       ALONE, where an upload leaves both pointing at this object.
        ///       Nothing may rely on that side effect - every call site here
        ///       either follows with @ref SetAttribPointer (which binds both
        ///       itself) or nothing at all, and @ref DrawArrays binds too.
        ///
        /// @param data  Source data. @c nullptr allocates without uploading;
        ///              that always reaches the driver and drops the cache,
        ///              since the resulting store's contents are undefined and
        ///              there is nothing to remember about them.
        /// @param size  Size in bytes.
        /// @param usage  GL_STATIC_DRAW, GL_DYNAMIC_DRAW, etc. Tracked with
        ///               the bytes: a caller that re-uploads identical data
        ///               under a new hint means to re-specify the store.
        void SetVertexData(const void *data, size_t size, GLenum usage = GL_STATIC_DRAW)
        {
            if (data != nullptr && usage == mUsage &&
                mCache.size() == size && std::memcmp(mCache.data(), data, size) == 0)
            {
                return;
            }

            Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferData(GL_ARRAY_BUFFER, size, data, usage);
            mUsage = usage;

            if (data == nullptr)
            {
                mCache.clear();
                return;
            }
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            mCache.assign(bytes, bytes + size);
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
        std::vector<uint8_t> mCache; ///< Last-uploaded bytes; skips redundant glBufferData.
        /// Usage hint behind @c mCache. 0 is not a valid GL usage enum, so it
        /// can never collide with a real one before the first upload.
        GLenum mUsage = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_VERTEX_ARRAY_H_
