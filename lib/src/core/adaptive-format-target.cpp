#include "core/adaptive-format-target.h"

namespace EdgeLighting
{

    AdaptiveFormatTarget::EnsureResult AdaptiveFormatTarget::Ensure(FormatAvailability &availability,
                                                                    const FormatList &formats, int width, int height,
                                                                    int attachments)
    {
        // Told by the count, not by what the buffer looks like after: a walk
        // whose better row failed again reallocates the row it had, at the
        // size it had.
        const uint32_t before = mBuffer.GetAllocationCount();
        const bool heldBuffer = mBuffer.IsValid();
        mFormats = formats;
        if (!availability.allocateInBestFormat(mBuffer, mFormats, mFilter, mFallbackCost.c_str(), width, height,
                                               attachments))
        {
            // A failed walk always leaves the buffer released. If it held
            // one, that is a loss the generation has to show: nothing was
            // allocated, so the count alone would not move.
            if (heldBuffer)
            {
                ++mLosses;
            }
            return EnsureResult::FAILED;
        }
        return mBuffer.GetAllocationCount() != before ? EnsureResult::ALLOCATED : EnsureResult::HELD;
    }

    AdaptiveFormatTarget::EnsureResult AdaptiveFormatTarget::Refresh(FormatAvailability &availability)
    {
        if (!availability.isBelowBestRow(mBuffer, mFormats))
        {
            return EnsureResult::HELD;
        }
        // A copy: Ensure overwrites mFormats with what it is handed.
        const FormatList formats = mFormats;
        return Ensure(availability, formats, mBuffer.GetWidth(), mBuffer.GetHeight(), mBuffer.GetAttachmentCount());
    }

} // namespace EdgeLighting
