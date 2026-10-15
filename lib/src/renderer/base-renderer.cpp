#include "renderer/base-renderer.h"
#include "core/resource-context.h"
#include <string>

namespace EdgeLighting
{

    ShaderProgram BaseRenderer::BuildProgram(const char *vertSrc, const char *fragSrc, const char *name,
                                             const char *fragDefines) const
    {
        // The vertex stage first, and the fragment stage only if it compiled:
        // the order ShaderProgram's own two-source constructor keeps, so a
        // failed vertex stage costs no fragment compile on either path.
        if (mResourceContext)
        {
            ShaderCache &cache = mResourceContext->GetShaderCache();
            const ShaderObject &vertex = cache.Get(GL_VERTEX_SHADER, vertSrc, nullptr, name);
            if (!vertex.IsValid())
            {
                return ShaderProgram();
            }
            const ShaderObject &fragment = cache.Get(GL_FRAGMENT_SHADER, fragSrc, fragDefines, name);
            return ShaderProgram(vertex, fragment, name);
        }

        const ShaderObject vertex(GL_VERTEX_SHADER, vertSrc, "vertex", name);
        if (!vertex.IsValid())
        {
            return ShaderProgram();
        }
        const std::string source = fragDefines ? ShaderCache::WithDefines(fragSrc, fragDefines) : std::string(fragSrc);
        const ShaderObject fragment(GL_FRAGMENT_SHADER, source.c_str(), "fragment", name);
        return ShaderProgram(vertex, fragment, name);
    }

} // namespace EdgeLighting
