#include "core/resource-context.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    ResourceContext::ResourceContext() = default;
    ResourceContext::~ResourceContext() = default;

    const VertexArray &ResourceContext::GetFullscreenQuad()
    {
        if (!mFullscreenQuad)
        {
            mFullscreenQuad = std::make_unique<VertexArray>("ResourceContext.FullscreenQuad");
            BuildFullscreenQuad(*mFullscreenQuad);
        }
        return *mFullscreenQuad;
    }

    void ResourceContext::BuildFullscreenQuad(const VertexArray &quad)
    {
        // Static, in clip space: independent of the geometry, the viewport
        // and any resolution scale, so uploaded once and never revisited.
        // clang-format off
        const float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        quad.SetVertexData(ndc, sizeof(ndc));
        quad.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

} // namespace EdgeLighting
