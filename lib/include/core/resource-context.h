#ifndef _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
#define _EDGE_LIGHTING_RESOURCE_CONTEXT_H_

#include "gl/shader-cache.h"

namespace EdgeLighting
{
    /// What the renderers of one effect share. Today that is the compiled
    /// shader stages (@ref ShaderCache). docs/resource-sharing-plan.md adds
    /// the buffer registry (its S2), the shared quad (S5) and a render-target
    /// pool (S7), each under the plan's rule: one owner writes a resource,
    /// everyone else only reads it.
    ///
    /// Created by @c EdgeLightingEffect and handed to every renderer it
    /// registers (@c BaseRenderer::SetResourceContext). Both hold it by
    /// @c shared_ptr, so it lives while either does. A renderer drawn without
    /// an effect has none and builds everything for itself, as it did before
    /// the context existed.
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

    private:
        ShaderCache mShaderCache;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
