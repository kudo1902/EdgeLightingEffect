#include "core/shared-buffer.h"
#include <utility>

namespace EdgeLighting
{

    SharedBuffer::SharedBuffer(const char *name, std::vector<Format> formats, const char *fallbackCost)
        // GL_LINEAR is asked for through the walk, the only writer of the
        // tracked filter: setting it on the texture afterwards would leave the
        // two disagreeing and make the next walk reallocate, every frame.
        : mTarget(name, GL_LINEAR, fallbackCost), mFormats(std::move(formats))
    {
    }

    bool SharedBuffer::BeginWrite(const glm::ivec2 &texels, const glm::ivec4 &window, FormatAvailability &availability)
    {
        // The format walk starts at the best format this context may render
        // at this size - the probe results and every failed allocation are
        // kept in @p availability, so the buffer keeps none.
        // The target keeps the list's address for its Refresh: @c mFormats,
        // which this object - neither copied nor moved - keeps.
        if (mTarget.Ensure(availability, FormatList(mFormats.data(), mFormats.size()), texels.x, texels.y) ==
            AdaptiveFormatTarget::EnsureResult::FAILED)
        {
            // Resize destroys the attachment on its failure path, so the
            // buffer holds id 0, and Bind would bind the CALLER's framebuffer -
            // which a clear, unclipped by the viewport, would then erase. Bail
            // before either.
            mDirtyKnown = false;
            mReady = false;
            return false;
        }
        // A buffer allocated since the last write - by the walk above, or by
        // a Refresh earlier in the frame - holds undefined texels, so the
        // clear below has to be whole. Told by the generation, not by the
        // size and format: a retry of a better format that fails destroys the
        // buffer, and the walk then allocates the format it had at the size
        // it had - a fresh texture that looks like the old one, often under
        // the same name.
        if (mTarget.GetGeneration() != mWrittenGeneration)
        {
            mDirtyKnown = false;
        }

        // Bind, then clear: ClearBuffer acts on whatever is BOUND, so the bind
        // is its precondition. Only what can hold anything but 0 - this write's
        // window and the last write's; every texel outside both is still the 0
        // a clear left. A whole viewport-sized buffer cleared on every frame
        // that redraws it - every hue and intensity frame - cost a small rect up
        // to 1.65x a frame at 4K on an AMD Radeon Pro 5300M.
        mTarget.Bind();
        if (mDirtyKnown)
        {
            const glm::ivec2 lo = glm::min(glm::ivec2(mDirty.x, mDirty.y), glm::ivec2(window.x, window.y));
            const glm::ivec2 hi = glm::max(glm::ivec2(mDirty.z, mDirty.w), glm::ivec2(window.z, window.w));
            mTarget.ClearBufferRect(lo.x, lo.y, hi.x - lo.x, hi.y - lo.y);
        }
        else
        {
            mTarget.ClearBuffer();
        }
        mDirty = window;
        mDirtyKnown = true;
        mWrittenGeneration = mTarget.GetGeneration();
        ++mVersion;
        return true;
    }

    void SharedBuffer::MarkReady(const glm::ivec2 &viewport)
    {
        mViewport = viewport;
        mReady = mTarget.IsValid();
    }

    void SharedBuffer::Release()
    {
        mTarget.Release();
        mDirtyKnown = false;
        mReady = false;
    }

} // namespace EdgeLighting
