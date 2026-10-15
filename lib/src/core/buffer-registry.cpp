#include "core/buffer-registry.h"
#include "gl/framebuffer.h"

namespace EdgeLighting
{
    namespace
    {
        /// How many channels a colour-renderable internal format carries -
        /// what a viewer needs to show a one- or two-channel buffer sensibly,
        /// since a texture fetch fills the missing ones with (0, 0, 1).
        int ChannelCount(GLint internalFormat)
        {
            switch (internalFormat)
            {
            case GL_R8:
            case GL_R16F:
            case GL_R32F:
            {
                return 1;
            }
            case GL_RG8:
            case GL_RG16F:
            case GL_RG32F:
            {
                return 2;
            }
            case GL_RGB8:
            case GL_RGB16F:
            case GL_RGB32F:
            {
                return 3;
            }
            case GL_RGBA8:
            case GL_RGBA16F:
            case GL_RGBA32F:
            {
                return 4;
            }
            default:
            {
                return 0;
            }
            }
        }
    }

    const char *PublishedBuffer::GetName() const { return buffer ? buffer->GetName() : ""; }
    unsigned int PublishedBuffer::GetTexture() const { return buffer ? buffer->GetTextureId(attachment) : 0; }
    glm::ivec2 PublishedBuffer::GetTexels() const
    {
        return buffer ? glm::ivec2(buffer->GetWidth(), buffer->GetHeight()) : glm::ivec2(0);
    }
    int PublishedBuffer::GetInternalFormat() const { return buffer ? buffer->GetInternalFormat() : 0; }
    int PublishedBuffer::GetChannels() const { return ChannelCount(GetInternalFormat()); }

    void BufferRegistry::Publish(const PublishedBuffer &record)
    {
        if (record.GetTexture() == 0)
        {
            return;
        }
        for (PublishedBuffer &existing : mBuffers)
        {
            if (existing.buffer == record.buffer && existing.attachment == record.attachment)
            {
                existing = record;
                return;
            }
        }
        mBuffers.push_back(record);
    }

    const PublishedBuffer *BufferRegistry::Find(std::string_view name, int attachment) const
    {
        for (const PublishedBuffer &record : mBuffers)
        {
            if (record.attachment == attachment && name == record.GetName())
            {
                return &record;
            }
        }
        return nullptr;
    }

} // namespace EdgeLighting
