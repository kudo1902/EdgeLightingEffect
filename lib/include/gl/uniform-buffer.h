#ifndef _EDGE_LIGHTING_UNIFORM_BUFFER_H_
#define _EDGE_LIGHTING_UNIFORM_BUFFER_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace EdgeLighting
{
    /// RAII wrapper around an OpenGL uniform buffer object (UBO).
    ///
    /// Backs a std140 `uniform` block. @ref SetData caches the last-uploaded
    /// bytes and skips the GL call when the contents are unchanged (same idea
    /// as ShaderProgram's per-uniform caches). Move-only.
    class UniformBuffer
    {
    public:
        /// Generates a new UBO.
        /// @param name  Optional label used in log messages (default "unnamed").
        UniformBuffer(const char *name = nullptr)
            : mName(name ? name : "unnamed")
        {
            glGenBuffers(1, &mUbo);
            LOG_I("UniformBuffer[%s] created (ubo=%u).", mName.c_str(), mUbo);
        }

        ~UniformBuffer()
        {
            if (mUbo != 0)
            {
                glDeleteBuffers(1, &mUbo);
            }
            LOG_I("UniformBuffer[%s] destroyed.", mName.c_str());
        }

        UniformBuffer(const UniformBuffer &) = delete;
        UniformBuffer &operator=(const UniformBuffer &) = delete;

        UniformBuffer(UniformBuffer &&other) noexcept
            : mUbo(other.mUbo), mName(std::move(other.mName)), mCache(std::move(other.mCache))
        {
            other.mUbo = 0;
            LOG_I("UniformBuffer[%s] moved (ubo=%u).", mName.c_str(), mUbo);
        }

        UniformBuffer &operator=(UniformBuffer &&other) noexcept
        {
            if (this != &other)
            {
                if (mUbo != 0)
                {
                    glDeleteBuffers(1, &mUbo);
                }
                LOG_I("UniformBuffer[%s] move-assign replaced (ubo=%u->%u).",
                      mName.c_str(), mUbo, other.mUbo);
                mUbo = other.mUbo;
                mName = std::move(other.mName);
                mCache = std::move(other.mCache);
                other.mUbo = 0;
            }
            return *this;
        }

        /// Uploads @p size bytes of block data (std140-packed by the caller).
        /// Skips the GL call when identical to the previous upload.
        void SetData(const void *data, size_t size, GLenum usage = GL_DYNAMIC_DRAW)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            if (mCache.size() == size && std::memcmp(mCache.data(), bytes, size) == 0)
            {
                return;
            }

            mCache.assign(bytes, bytes + size);
            glBindBuffer(GL_UNIFORM_BUFFER, mUbo);
            glBufferData(GL_UNIFORM_BUFFER, size, data, usage);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }

        /// Attaches the buffer to @p bindingPoint. Pairs with
        /// ShaderProgram::SetUniformBlockBinding on the same point.
        void BindBase(GLuint bindingPoint) const
        {
            glBindBufferBase(GL_UNIFORM_BUFFER, bindingPoint, mUbo);
        }

    private:
        GLuint mUbo = 0; ///< Uniform Buffer Object handle.
        std::string mName = "unnamed";
        std::vector<uint8_t> mCache; ///< Last-uploaded bytes; skips redundant glBufferData.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_UNIFORM_BUFFER_H_
