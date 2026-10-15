#ifndef _EDGE_LIGHTING_BUFFER_REGISTRY_H_
#define _EDGE_LIGHTING_BUFFER_REGISTRY_H_

#include <glm/glm.hpp>
#include <string_view>
#include <vector>

namespace EdgeLighting
{
    class Framebuffer;

    /// One offscreen buffer a renderer drew or read this frame, for another
    /// renderer to READ: which attachment of which of the producer's
    /// Framebuffers, and where on the caller's framebuffer it lies when it
    /// lies anywhere.
    ///
    /// It REFERS to the Framebuffer rather than copying from it: the name,
    /// texture, size and format are the Framebuffer's own, read through the
    /// accessors below, so nothing here can disagree with it - a buffer
    /// released since reads texture 0, one reallocated reads its new size.
    /// What the record adds is only what the Framebuffer cannot know: the
    /// attachment, and the box of the frame its producer reads it over.
    ///
    /// A record, not a share: the producer still owns the buffer, allocates
    /// and releases it, and is the only thing that writes it. Most of these
    /// buffers are caches a later frame reuses (the neon's skips on frames
    /// where nothing moved), which is why nothing is lent out for writing.
    ///
    /// @par Lifetime
    /// Until the next @ref BufferRegistry::Clear: the top of the next
    /// @c EdgeLightingEffect::Render, the effect's destructor, or an
    /// @c EdgeLightingEffect::Initialize that drops a renderer that failed.
    /// Those last two are the only places a producer can leave the effect, so
    /// a record the effect's frames publish never outlives the producer it
    /// points into. A renderer the host keeps and draws on its own after the
    /// effect is gone still publishes here, and nothing clears that. The
    /// placement describes the frame it was published in; the accessors read
    /// the Framebuffer as it is now.
    typedef struct PublishedBuffer
    {
        const Framebuffer *buffer = nullptr; ///< The producer's own.
        int attachment = 0;                  ///< Which colour attachment of it.
        /// Whether @c origin and @c size say where it lies on the caller's
        /// framebuffer: its texels span that box evenly, the way its
        /// producer reads them. False for a table (a LUT, an atlas) that
        /// maps onto no area of the frame.
        bool placed = false;
        glm::vec2 origin{0.0f}; ///< When @c placed: the box's lower-left corner, caller's framebuffer px, y up.
        glm::vec2 size{0.0f};   ///< When @c placed: the box's extent, px.

        /// The Framebuffer's name ("NeonRenderer.Scaled") - the key, with
        /// @c attachment, that consumers look it up by.
        const char *GetName() const;
        /// The attachment's GL texture name; 0 once the producer released it.
        unsigned int GetTexture() const;
        /// Its size in texels.
        glm::ivec2 GetTexels() const;
        /// The GL internal format it is allocated in (GL_RGBA16F, ...).
        int GetInternalFormat() const;
        /// 1, 2, 3 or 4, from the internal format; 0 if unknown.
        int GetChannels() const;
    } PublishedBuffer;

    /// The offscreen buffers the renderers published this frame, for a layer
    /// that reads another's output - today the debug layer's buffer view
    /// (@c DebugConfig::bufferView).
    ///
    /// Owned by @c EdgeLightingEffect, which hands it to every renderer it
    /// registers (@ref BaseRenderer::SetBufferRegistry) and clears it at the
    /// top of every @c Render: a buffer is listed only on a frame its producer
    /// drew, so a disabled layer, or one that drew nothing, lists nothing.
    /// Steady state allocates nothing: a record is a pointer and a few
    /// numbers, and the clear keeps the vector's capacity.
    class BufferRegistry
    {
    public:
        /// Forget every record. @c EdgeLightingEffect::Render, before any
        /// renderer draws.
        void Clear() { mBuffers.clear(); }

        /// Add @p record, or replace the one for the same attachment of the
        /// same Framebuffer. Ignores a record whose attachment has no texture,
        /// so a producer can publish a buffer it has not allocated without a
        /// check of its own.
        void Publish(const PublishedBuffer &record);

        /// The record for attachment @p attachment of the Framebuffer named
        /// @p name this frame, or nullptr.
        const PublishedBuffer *Find(std::string_view name, int attachment = 0) const;

        /// Every record this frame, in publish order: the layer order.
        const std::vector<PublishedBuffer> &GetBuffers() const { return mBuffers; }

    private:
        std::vector<PublishedBuffer> mBuffers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BUFFER_REGISTRY_H_
