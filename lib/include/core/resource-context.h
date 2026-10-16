#ifndef _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
#define _EDGE_LIGHTING_RESOURCE_CONTEXT_H_

#include "core/buffer-registry.h"
#include "core/read-list.h"
#include "gl/shader-cache.h"

namespace EdgeLighting
{
    /// What the renderers of one effect share: the compiled shader stages
    /// (@ref ShaderCache), the records of their offscreen buffers
    /// (@ref BufferRegistry) and this frame's declared reads of them
    /// (@ref ReadList). docs/resource-sharing-plan.md adds the shared quad
    /// (its S5) and a render-target pool (S7), each under the plan's rule:
    /// one owner writes a resource, everyone else only reads it.
    ///
    /// Created by @c EdgeLightingEffect and handed to every renderer it
    /// registers (@c BaseRenderer::SetResourceContext). Both hold it by
    /// @c shared_ptr, so it lives while either does. A renderer drawn without
    /// an effect has none: it builds everything for itself, as it did before
    /// the context existed, and lists its buffers nowhere.
    ///
    /// It holds GL objects: like the renderers, destroy it with the GL
    /// context current.
    class ResourceContext
    {
    public:
        ResourceContext() = default;
        ResourceContext(const ResourceContext &) = delete;
        ResourceContext &operator=(const ResourceContext &) = delete;

        ShaderCache &GetShaderCache() { return mShaderCache; }
        const ShaderCache &GetShaderCache() const { return mShaderCache; }

        BufferRegistry &GetBufferRegistry() { return mBufferRegistry; }
        const BufferRegistry &GetBufferRegistry() const { return mBufferRegistry; }

        ReadList &GetReadList() { return mReadList; }
        const ReadList &GetReadList() const { return mReadList; }

    private:
        ShaderCache mShaderCache;
        BufferRegistry mBufferRegistry;
        ReadList mReadList;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
