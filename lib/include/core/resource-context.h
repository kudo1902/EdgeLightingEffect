#ifndef _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
#define _EDGE_LIGHTING_RESOURCE_CONTEXT_H_

#include "core/buffer-registry.h"
#include "core/read-list.h"
#include "gl/shader-cache.h"
#include <memory>

namespace EdgeLighting
{
    class VertexArray;

    /// What the renderers of one effect share: the compiled shader stages
    /// (@ref ShaderCache), the records of their offscreen buffers
    /// (@ref BufferRegistry), this frame's declared reads of them
    /// (@ref ReadList) and one fullscreen quad (@ref GetFullscreenQuad).
    /// docs/resource-sharing-plan.md adds a render-target pool (its S7),
    /// under the plan's rule: one owner writes a resource, everyone else only
    /// reads it.
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
        ResourceContext();
        ~ResourceContext();
        ResourceContext(const ResourceContext &) = delete;
        ResourceContext &operator=(const ResourceContext &) = delete;

        ShaderCache &GetShaderCache() { return mShaderCache; }
        const ShaderCache &GetShaderCache() const { return mShaderCache; }

        BufferRegistry &GetBufferRegistry() { return mBufferRegistry; }
        const BufferRegistry &GetBufferRegistry() const { return mBufferRegistry; }

        ReadList &GetReadList() { return mReadList; }
        const ReadList &GetReadList() const { return mReadList; }

        /// The NDC quad every renderer of the effect draws a whole-target pass
        /// with (an identity MVP): the neon's emission bake and its ALL-mode
        /// fill, the lens flare and its blit, the spotlight's blit. Built on
        /// the first call, so a GL context must be current then. Renderers take
        /// it through @c BaseRenderer::GetFullscreenQuad, which falls back to
        /// a quad of their own without a context.
        const VertexArray &GetFullscreenQuad();

        /// Fills @p quad with the fullscreen quad: two triangles, six vertices
        /// of @c vec2 NDC position at attribute 0. The one definition, for
        /// the context's quad and a renderer's own alike - the passes drawn
        /// with it are byte-identical either way.
        static void BuildFullscreenQuad(const VertexArray &quad);

    private:
        ShaderCache mShaderCache;
        BufferRegistry mBufferRegistry;
        ReadList mReadList;
        std::unique_ptr<VertexArray> mFullscreenQuad;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RESOURCE_CONTEXT_H_
