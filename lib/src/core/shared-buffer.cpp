#include "core/shared-buffer.h"
#include "core/render-target-formats.h"
#include "util/log-util.h"
#include <utility>

namespace EdgeLighting
{

    SharedBuffer::SharedBuffer(const char *name, std::vector<Format> formats, const char *consequence)
        : mBuffer(name), mFormats(std::move(formats)), mConsequence(consequence ? consequence : "")
    {
    }

    bool SharedBuffer::BeginWrite(const glm::ivec2 &texels, const glm::ivec4 &window, RenderTargetFormats &targets)
    {
        // What the buffer is before the resize: a reallocation leaves its
        // texels undefined, so the clear below has to be whole.
        const bool wasValid = mBuffer.IsValid();
        const glm::ivec2 wasSize = GetTexels();
        const GLint wasFormat = mBuffer.GetInternalFormat();

        // The format walk (ResizeInBestFormat) starts at the best format this
        // context may render at this size - the probe's verdicts and every
        // failed allocation are kept in @p targets, so the buffer keeps none.
        // The filter is requested through Resize, the only writer of the
        // tracked value: setting it on the texture afterwards would leave the
        // two disagreeing and make the next Resize reallocate, every frame.
        if (!ResizeInBestFormat(mBuffer, targets, mFormats.data(), mFormats.size(), texels.x, texels.y, GL_LINEAR,
                                1, mBuffer.GetName(), "shared", mConsequence.c_str()))
        {
            // Resize destroys the attachment on its failure path, so the
            // buffer holds id 0, and Bind would bind the CALLER's framebuffer -
            // which a clear, unclipped by the viewport, would then erase. Bail
            // before either.
            mDirtyKnown = false;
            mReady = false;
            return false;
        }
        if (!wasValid || wasSize != GetTexels() || wasFormat != mBuffer.GetInternalFormat())
        {
            mDirtyKnown = false;
        }

        // Bind, then clear: ClearBuffer acts on whatever is BOUND, so the bind
        // is its precondition. Only what can hold anything but 0 - this write's
        // window and the last write's; every texel outside both is still the 0
        // a clear left. A whole viewport-sized buffer cleared on every frame
        // that redraws it - every hue and intensity frame - cost a small rect up
        // to 1.65x a frame at 4K on an AMD Radeon Pro 5300M.
        mBuffer.Bind();
        if (mDirtyKnown)
        {
            const glm::ivec2 lo = glm::min(glm::ivec2(mDirty.x, mDirty.y), glm::ivec2(window.x, window.y));
            const glm::ivec2 hi = glm::max(glm::ivec2(mDirty.z, mDirty.w), glm::ivec2(window.z, window.w));
            mBuffer.ClearBufferRect(lo.x, lo.y, hi.x - lo.x, hi.y - lo.y);
        }
        else
        {
            mBuffer.ClearBuffer();
        }
        mDirty = window;
        mDirtyKnown = true;
        ++mVersion;
        return true;
    }

    void SharedBuffer::MarkReady(const glm::ivec2 &viewport)
    {
        mViewport = viewport;
        mReady = mBuffer.IsValid();
    }

    void SharedBuffer::Release()
    {
        mBuffer.Release();
        mDirtyKnown = false;
        mReady = false;
    }

} // namespace EdgeLighting
