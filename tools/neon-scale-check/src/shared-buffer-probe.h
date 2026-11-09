#ifndef _NEON_SCALE_CHECK_SHARED_BUFFER_PROBE_H_
#define _NEON_SCALE_CHECK_SHARED_BUFFER_PROBE_H_

// The one reader of the neon's shared buffer the checks register: `shared-buffer`
// as it is, and `formats` (for the formats it is handed) and `sequence` with
// the texels read back on top (ReadSharedTexels).

#include "core/config.h"
#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "renderer/base-renderer.h"

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace NeonScaleCheck
{
    using namespace EdgeLighting;

    /// Of @p texels - RGBA floats, bottom row first, @p size across and down -
    /// how many lie outside @p window (x0, y0, x1, y1:
    /// SharedBuffer::GetLastWriteWindow), in @p outside, and how many of those
    /// are not 0, returned. The windowed clear rests on that being none; and
    /// it proves something only where @p outside is not 0.
    inline size_t CountNonZeroOutsideWindow(const std::vector<float> &texels, const glm::ivec2 &size,
                                            const glm::ivec4 &window, size_t &outside)
    {
        outside = 0;
        size_t nonZero = 0;
        for (int y = 0; y < size.y; ++y)
        {
            for (int x = 0; x < size.x; ++x)
            {
                const size_t at = (size_t(y) * size_t(size.x) + size_t(x)) * 4;
                if ((x >= window.x && x < window.z && y >= window.y && y < window.w) || at + 3 >= texels.size())
                {
                    continue;
                }
                ++outside;
                nonZero += texels[at] != 0.0f || texels[at + 1] != 0.0f || texels[at + 2] != 0.0f ||
                           texels[at + 3] != 0.0f;
            }
        }
        return nonZero;
    }

    /// Every texel of @p buffer as RGBA floats, bottom row first, read
    /// through a framebuffer of its own - so nothing the frame has bound is
    /// touched. Empty when the buffer holds nothing.
    inline std::vector<float> ReadSharedTexels(const SharedBuffer &buffer)
    {
        const glm::ivec2 texels = buffer.GetTexels();
        if (buffer.GetTextureId() == 0 || texels.x <= 0 || texels.y <= 0)
        {
            return {};
        }
        std::vector<float> data(size_t(texels.x) * size_t(texels.y) * 4, 0.0f);
        GLint previous = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.GetTextureId(), 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, texels.x, texels.y, GL_RGBA, GL_FLOAT, data.data());
        }
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
        glDeleteFramebuffers(1, &fbo);
        return data;
    }

    /// What a reader was handed of the shared buffer in one Render.
    typedef struct SharedBufferSeen
    {
        bool ready = false;
        uint32_t version = 0;
        glm::ivec2 viewport{0};
        glm::ivec2 texels{0};
        GLuint texture = 0;
        GLint internalFormat = 0;
        glm::ivec4 window{0}; ///< SharedBuffer::GetLastWriteWindow: every texel outside it should be 0.
    } SharedBufferSeen;

    /// An internal reader of the neon's shared buffer, as a library renderer
    /// would be one: it takes the buffer (Find(SharedBufferId::NEON)) in every
    /// Render and records what it was handed - the last frame's snapshot, and
    /// over its life how many frames it was ready and in which formats. Draws
    /// nothing. A check that needs more of a ready buffer while it is still
    /// this frame's - its texels - overrides OnReady.
    class SharedBufferProbe : public BaseRenderer
    {
    public:
        bool Initialize() override { return true; }
        void Update(float, double, const Config &) override {}
        void Render(int, int, double, const Config &) override
        {
            const std::shared_ptr<const SharedBuffer> buffer = GetSharedBufferRegistry()->Find(SharedBufferId::NEON);
            mSeen = SharedBufferSeen{};
            if (buffer)
            {
                mSeen = SharedBufferSeen{buffer->IsReady(),       buffer->GetVersion(),
                                         buffer->GetViewport(),   buffer->GetTexels(),
                                         buffer->GetTextureId(),  buffer->GetInternalFormat(),
                                         buffer->GetLastWriteWindow()};
            }
            if (mSeen.ready)
            {
                ++mReadyFrames;
                ++mFormats[mSeen.internalFormat];
                OnReady(*buffer);
            }
            else
            {
                OnNotReady();
            }
        }
        void OnConfigChanged(const Config &) override {}
        RendererLayer GetLayer() const override { return RendererLayer::CUSTOM; }

        /// What the last Render was handed.
        const SharedBufferSeen &Last() const { return mSeen; }
        /// Renders that were handed a ready buffer.
        int GetReadyFrames() const { return mReadyFrames; }
        /// The ready buffers' internal formats, with how many Renders saw each.
        const std::map<GLint, int> &GetFormats() const { return mFormats; }

    protected:
        /// In Render, after the snapshot, when the buffer was ready.
        virtual void OnReady(const SharedBuffer &) {}
        /// In Render, after the snapshot, when it was not.
        virtual void OnNotReady() {}

    private:
        SharedBufferSeen mSeen;
        int mReadyFrames = 0;
        std::map<GLint, int> mFormats;
    };
}

#endif
