#include "core/adaptive-format-target.h"

namespace EdgeLighting
{

    AdaptiveFormatTarget::EnsureResult AdaptiveFormatTarget::Ensure(FormatAvailability &availability,
                                                                    const FormatList &formats, int width, int height,
                                                                    int attachments)
    {
        // Told by the count, not by what the buffer looks like after: a walk
        // whose first row failed may reallocate the row it had, at the size
        // it had.
        const uint32_t before = mBuffer.GetAllocationCount();
        const bool heldBuffer = mBuffer.IsValid();
        if (!availability.allocateInBestFormat(mBuffer, formats, mFilter, mFallbackCost.c_str(), width, height,
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

} // namespace EdgeLighting
