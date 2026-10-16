#include "core/buffer-registry.h"
#include "gl/framebuffer.h"
#include "util/log-util.h"
#include <cstdio>
#include <cstring>

namespace EdgeLighting
{
    namespace
    {
        /// How many channels a colour-renderable internal format carries -
        /// what a viewer needs to show a one- or two-channel buffer sensibly,
        /// since a texture fetch fills the missing ones with (0, 0, 1).
        ///
        /// GL ES 3.0 has no glGetTexLevelParameteriv to ask the driver, so the
        /// list is written out: every float or normalised format the library or
        /// a host renderer can render to (the 16-bit normalised ones only where
        /// the GL header has them - EXT_texture_norm16 on ES). The integer
        /// formats are left out on purpose: a float sampler cannot read them,
        /// so no channel count would make the view right.
        int ChannelCount(GLint internalFormat)
        {
            switch (internalFormat)
            {
            case GL_R8:
            case GL_R8_SNORM:
            case GL_R16F:
            case GL_R32F:
#ifdef GL_R16
            case GL_R16:
#endif
            {
                return 1;
            }
            case GL_RG8:
            case GL_RG8_SNORM:
            case GL_RG16F:
            case GL_RG32F:
#ifdef GL_RG16
            case GL_RG16:
#endif
            {
                return 2;
            }
            case GL_RGB8:
            case GL_RGB8_SNORM:
            case GL_RGB16F:
            case GL_RGB32F:
            case GL_RGB565:
            case GL_SRGB8:
            case GL_R11F_G11F_B10F:
            case GL_RGB9_E5:
#ifdef GL_RGB16
            case GL_RGB16:
#endif
            {
                return 3;
            }
            case GL_RGBA8:
            case GL_RGBA8_SNORM:
            case GL_RGBA16F:
            case GL_RGBA32F:
            case GL_RGBA4:
            case GL_RGB5_A1:
            case GL_RGB10_A2:
            case GL_SRGB8_ALPHA8:
#ifdef GL_RGBA16
            case GL_RGBA16:
#endif
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

    unsigned int BufferRecord::GetTexture() const { return buffer ? buffer->GetTextureId(attachment) : 0; }
    glm::ivec2 BufferRecord::GetTexels() const
    {
        return buffer ? glm::ivec2(buffer->GetWidth(), buffer->GetHeight()) : glm::ivec2(0);
    }
    int BufferRecord::GetInternalFormat() const { return buffer ? buffer->GetInternalFormat() : 0; }
    int BufferRecord::GetChannels() const { return ChannelCount(GetInternalFormat()); }

    bool MakeBufferKey(const char *name, int attachment, char (&out)[BUFFER_KEY_CAPACITY])
    {
        out[0] = '\0';
        if (!name || attachment < 0)
        {
            return false;
        }
        const int written = attachment > 0 ? std::snprintf(out, sizeof(out), "%s[%d]", name, attachment)
                                           : std::snprintf(out, sizeof(out), "%s", name);
        if (written < 0 || written >= BUFFER_KEY_CAPACITY)
        {
            out[0] = '\0';
            return false;
        }
        return true;
    }

    int GetBufferKeyAttachment(std::string_view key)
    {
        if (key.size() < 3 || key.back() != ']')
        {
            return 0;
        }
        const size_t open = key.rfind('[');
        if (open == std::string_view::npos || open + 2 > key.size() - 1)
        {
            return 0;
        }
        int attachment = 0;
        for (size_t i = open + 1; i + 1 < key.size(); ++i)
        {
            const char c = key[i];
            if (c < '0' || c > '9' || attachment > 1000)
            {
                return 0;
            }
            attachment = attachment * 10 + (c - '0');
        }
        return attachment;
    }

    int BufferRegistry::Register(const BaseRenderer *producer, const char *key, const Framebuffer &buffer,
                                 int attachment, BufferVisibility visibility)
    {
        const size_t length = key ? std::strlen(key) : 0;
        if (!producer || length == 0 || length >= static_cast<size_t>(BUFFER_KEY_CAPACITY))
        {
            LOG_E("BufferRegistry: key '%s' refused - %s.", key ? key : "",
                  length == 0 ? "empty" : "longer than BUFFER_KEY_CAPACITY allows");
            return INVALID_SLOT;
        }
        if (Find(key))
        {
            LOG_W("BufferRegistry: '%s' is registered already - the second buffer of that name is not listed.",
                  key);
            return INVALID_SLOT;
        }

        BufferRecord record;
        std::memcpy(record.key, key, length + 1);
        record.producer = producer;
        record.buffer = &buffer;
        record.attachment = attachment;
        record.visibility = visibility;

        ++mRecordCount;
        for (size_t i = 0; i < mRecords.size(); ++i)
        {
            if (!mRecords[i].IsRegistered())
            {
                mRecords[i] = record;
                return static_cast<int>(i);
            }
        }
        mRecords.push_back(record);
        return static_cast<int>(mRecords.size() - 1);
    }

    void BufferRegistry::Unregister(int slot)
    {
        if (slot < 0 || static_cast<size_t>(slot) >= mRecords.size() || !mRecords[size_t(slot)].IsRegistered())
        {
            return;
        }
        mRecords[size_t(slot)] = BufferRecord();
        --mRecordCount;
    }

    void BufferRegistry::MarkCurrent(int slot, bool changed)
    {
        if (slot < 0 || static_cast<size_t>(slot) >= mRecords.size() || !mRecords[size_t(slot)].IsRegistered())
        {
            return;
        }
        BufferRecord &record = mRecords[size_t(slot)];
        record.frame = mFrame;
        record.version += changed ? 1u : 0u;
        record.placed = false;
    }

    void BufferRegistry::MarkCurrent(int slot, const glm::vec2 &origin, const glm::vec2 &size, bool changed)
    {
        if (slot < 0 || static_cast<size_t>(slot) >= mRecords.size() || !mRecords[size_t(slot)].IsRegistered())
        {
            return;
        }
        BufferRecord &record = mRecords[size_t(slot)];
        record.frame = mFrame;
        record.version += changed ? 1u : 0u;
        record.placed = true;
        record.origin = origin;
        record.size = size;
    }

    const BufferRecord *BufferRegistry::Find(std::string_view key) const
    {
        if (key.empty())
        {
            return nullptr;
        }
        for (const BufferRecord &record : mRecords)
        {
            if (record.IsRegistered() && key == record.key)
            {
                return &record;
            }
        }
        return nullptr;
    }

} // namespace EdgeLighting
