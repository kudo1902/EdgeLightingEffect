#ifndef _NEON_SCALE_CHECK_SHARED_BUFFER_PROBE_H_
#define _NEON_SCALE_CHECK_SHARED_BUFFER_PROBE_H_

// The one reader of the neon's shared buffer the checks register: `shared-buffer`
// as it is, `formats` for the formats it is handed, and `sequence`'s BufferProbe,
// which reads the texels back, on top.

#include "core/config.h"
#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "renderer/base-renderer.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <map>
#include <memory>

namespace NeonScaleCheck
{
    using namespace EdgeLighting;

    /// What a reader was handed of the shared buffer in one Render.
    typedef struct SharedBufferSeen
    {
        bool ready = false;
        uint32_t version = 0;
        glm::ivec2 viewport{0};
        glm::ivec2 texels{0};
        GLuint texture = 0;
        GLint internalFormat = 0;
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
                mSeen = SharedBufferSeen{buffer->IsReady(),   buffer->GetVersion(),   buffer->GetViewport(),
                                         buffer->GetTexels(), buffer->GetTextureId(), buffer->GetInternalFormat()};
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
