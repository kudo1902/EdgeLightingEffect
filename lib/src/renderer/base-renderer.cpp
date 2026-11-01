#include "renderer/base-renderer.h"
#include "core/format-availability.h"

namespace EdgeLighting
{

    FormatAvailability &BaseRenderer::GetFormatAvailability()
    {
        // A renderer the effect registered holds the effect's, whose startup
        // report probed every format the library uses. One no effect holds
        // gets its own, made here because the header only declares the type,
        // and asks those formats at once, as the report would - so its walks
        // find their answers kept rather than probing mid-frame, where a
        // probe clears the frame's pending GL errors. Renderers that use it
        // call this from their Initialize, so this lands there.
        if (!mFormatAvailability)
        {
            mFormatAvailability = std::make_shared<FormatAvailability>();
            mOwnsFormatAvailability = true;
            mFormatAvailability->AskReportFormats();
        }
        return *mFormatAvailability;
    }

    void BaseRenderer::AdvanceOwnFormatAvailability(float deltaTime)
    {
        // Only an object this renderer made: the effect's is advanced by the
        // effect, once a frame - advancing it here too would age its failures
        // once per renderer.
        if (mOwnsFormatAvailability)
        {
            mFormatAvailability->Advance(deltaTime);
        }
    }

} // namespace EdgeLighting
