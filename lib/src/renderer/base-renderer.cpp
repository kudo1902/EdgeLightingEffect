#include "renderer/base-renderer.h"
#include "core/resource-context.h"
#include "gl/framebuffer.h"
#include "gl/vertex-array.h"
#include "util/log-util.h"
#include <string>
#include <utility>

namespace EdgeLighting
{

    BaseRenderer::~BaseRenderer()
    {
        // The derived class's Framebuffers are already gone, but nothing reads
        // a record between their destruction and this - one thread draws.
        unregisterOwned();
    }

    void BaseRenderer::Prepare(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        mPrepared = true;
        RenderOffscreen(viewportWidth, viewportHeight, time, config);
    }

    void BaseRenderer::SetResourceContext(std::shared_ptr<ResourceContext> context)
    {
        if (context == mResourceContext)
        {
            return;
        }
        unregisterOwned();
        mResourceContext = std::move(context);
        for (OwnedBuffer &owned : mOwnedBuffers)
        {
            registerOwned(owned);
        }
    }

    void BaseRenderer::EnsurePrepared(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!mPrepared)
        {
            RenderOffscreen(viewportWidth, viewportHeight, time, config);
        }
        mPrepared = false;
    }

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

    BufferHandle BaseRenderer::RegisterBuffer(const Framebuffer &buffer, int attachment, BufferVisibility visibility)
    {
        OwnedBuffer owned;
        owned.buffer = &buffer;
        owned.attachment = attachment;
        owned.visibility = visibility;
        if (!MakeBufferKey(buffer.GetName(), attachment, owned.key))
        {
            LOG_E("BaseRenderer: buffer '%s' attachment %d has no key that fits in %d chars - never listed.",
                  buffer.GetName(), attachment, BUFFER_KEY_CAPACITY);
        }
        registerOwned(owned);
        mOwnedBuffers.push_back(owned);
        return static_cast<BufferHandle>(mOwnedBuffers.size() - 1);
    }

    void BaseRenderer::MarkBufferCurrent(BufferHandle handle, bool changed)
    {
        if (mResourceContext && handle >= 0 && static_cast<size_t>(handle) < mOwnedBuffers.size())
        {
            mResourceContext->GetBufferRegistry().MarkCurrent(mOwnedBuffers[size_t(handle)].slot, changed);
        }
    }

    void BaseRenderer::MarkBufferCurrent(BufferHandle handle, const glm::vec2 &origin, const glm::vec2 &size,
                                         bool changed)
    {
        if (mResourceContext && handle >= 0 && static_cast<size_t>(handle) < mOwnedBuffers.size())
        {
            mResourceContext->GetBufferRegistry().MarkCurrent(mOwnedBuffers[size_t(handle)].slot, origin, size,
                                                              changed);
        }
    }

    const BufferRegistry *BaseRenderer::GetBufferRegistry() const
    {
        return mResourceContext ? &mResourceContext->GetBufferRegistry() : nullptr;
    }

    const VertexArray &BaseRenderer::GetFullscreenQuad()
    {
        if (mResourceContext)
        {
            return mResourceContext->GetFullscreenQuad();
        }
        if (!mOwnFullscreenQuad)
        {
            mOwnFullscreenQuad = std::make_unique<VertexArray>("BaseRenderer.FullscreenQuad");
            ResourceContext::BuildFullscreenQuad(*mOwnFullscreenQuad);
        }
        return *mOwnFullscreenQuad;
    }

    bool BaseRenderer::IsBufferWanted(BufferHandle handle) const
    {
        if (!mResourceContext || handle < 0 || static_cast<size_t>(handle) >= mOwnedBuffers.size())
        {
            return false;
        }
        const OwnedBuffer &owned = mOwnedBuffers[size_t(handle)];
        return owned.key[0] != '\0' && mResourceContext->GetReadList().IsWanted(owned.key);
    }

    void BaseRenderer::registerOwned(OwnedBuffer &owned)
    {
        owned.slot = BufferRegistry::INVALID_SLOT;
        if (mResourceContext && owned.key[0] != '\0')
        {
            owned.slot = mResourceContext->GetBufferRegistry().Register(this, owned.key, *owned.buffer,
                                                                        owned.attachment, owned.visibility);
        }
    }

    void BaseRenderer::unregisterOwned()
    {
        if (!mResourceContext)
        {
            return;
        }
        BufferRegistry &registry = mResourceContext->GetBufferRegistry();
        for (OwnedBuffer &owned : mOwnedBuffers)
        {
            registry.Unregister(owned.slot);
            owned.slot = BufferRegistry::INVALID_SLOT;
        }
    }

} // namespace EdgeLighting
