#include "renderer/base-renderer.h"
#include "core/render-target-formats.h"

namespace EdgeLighting
{

    RenderTargetFormats &BaseRenderer::GetRenderTargetFormats()
    {
        // A renderer the effect registered holds the effect's, whose startup
        // report probed every format the library uses. One no effect holds
        // gets its own, made here because the header only declares the type,
        // and asks those formats at once, as the report would - so its walks
        // find their answers kept rather than probing mid-frame, where a
        // probe clears the frame's pending GL errors. Renderers that use it
        // call this from their Initialize, so this lands there.
        if (!mRenderTargetFormats)
        {
            mRenderTargetFormats = std::make_shared<RenderTargetFormats>();
            mRenderTargetFormats->AskReportFormats();
        }
        return *mRenderTargetFormats;
    }

} // namespace EdgeLighting
