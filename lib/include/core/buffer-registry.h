#ifndef _EDGE_LIGHTING_BUFFER_REGISTRY_H_
#define _EDGE_LIGHTING_BUFFER_REGISTRY_H_

#include <glm/glm.hpp>
#include <cstdint>
#include <deque>
#include <string_view>

namespace EdgeLighting
{
    class BaseLUT;
    class BaseRenderer;
    class Framebuffer;

    /// The longest buffer key, NUL included: "NeonRenderer.Gather[1]" and the
    /// like. A key that does not fit is refused at registration, so every key
    /// the registry holds can be handed back through a buffer of this size.
    /// Mirrored by the C ABI's @c EL_BUFFER_NAME_CAPACITY (a static_assert in
    /// capi-internal.h holds the two together).
    constexpr int BUFFER_KEY_CAPACITY = 64;

    /// Who may bind a record's texture (docs/resource-sharing-plan.md 3.2).
    typedef enum class BufferVisibility
    {
        /// A product with a written contract - what its texels mean, which box
        /// of the frame they cover, which of the producer's optimisations it
        /// constrains - that any layer may bind in production.
        PUBLIC,
        /// An intermediate, for the debug layer's buffer view and a host UI
        /// only. Its contents may change with any optimisation of its producer.
        DEBUG
    } BufferVisibility;

    /// One offscreen buffer a renderer owns, as the registry lists it: which
    /// attachment of which of the producer's Framebuffers - or which of its
    /// LUTs - under which key, for whom, and - once its owner has said so this
    /// frame - where on the caller's framebuffer it lies.
    ///
    /// It REFERS to the Framebuffer or the LUT rather than copying from it:
    /// the texture, size and format are the owner's own, read through the
    /// accessors below, so nothing here can disagree with it - a buffer
    /// released since reads texture 0, one reallocated reads its new size, a
    /// LUT not yet baked reads texture 0.
    ///
    /// A record, not a share: the producer owns the buffer, allocates and
    /// releases it, and is the only thing that writes it. A consumer binds the
    /// texture for sampling and nothing else - no render, no resize, no
    /// @c glTexParameter, no mipmaps. Most of these buffers are caches a later
    /// frame reuses (the neon's skips on frames where nothing moved).
    ///
    /// @par Lifetime
    /// Registered once by its producer (@ref BaseRenderer::RegisterBuffer) and
    /// removed with it: @c ~BaseRenderer unregisters every record the renderer
    /// owns, and @c BaseRenderer::SetResourceContext moves them when it joins
    /// another context. So a record lives exactly as long as the Framebuffer
    /// it points into, whatever the effect does with the renderer.
    ///
    /// Trivially copyable: the key is stored in place, not as a string.
    typedef struct BufferRecord
    {
        /// The Framebuffer's name, with "[n]" after it for attachment n > 0:
        /// "NeonRenderer.Scaled", "NeonRenderer.Gather[1]". Unique within a
        /// registry. Empty on a free slot.
        char key[BUFFER_KEY_CAPACITY] = {};
        /// The renderer that owns it - identity only, never dereferenced.
        /// nullptr on a free slot.
        const BaseRenderer *producer = nullptr;
        /// The producer's own - a Framebuffer and one colour attachment of
        /// it, or a LUT (@c attachment then 0). Exactly one of the two is set
        /// on a registered record.
        const Framebuffer *buffer = nullptr;
        const BaseLUT *lut = nullptr;
        int attachment = 0; ///< Which colour attachment of @c buffer.
        BufferVisibility visibility = BufferVisibility::DEBUG;
        /// The registry frame (@ref BufferRegistry::BeginFrame) its producer
        /// last marked it current in; 0 for never. Read through
        /// @ref BufferRegistry::IsCurrent.
        uint64_t frame = 0;
        /// Moves whenever its bytes may have changed: on registration, and on
        /// every frame it is marked current with bytes that changed since the
        /// last - a consumer keys a cache of its own on it, as the neon's
        /// emission table keys on @c BaseLUT::GetUploadCount. Drawn from one
        /// counter for the whole registry, so it only grows and no value
        /// repeats: a key registered again - a renderer replaced, or one
        /// joining from another context - never hands back a version a
        /// consumer cached under that key. A producer may move it for a pass
        /// that happened to write the same bytes.
        uint64_t version = 0;
        /// Whether @c origin and @c size say where it lies on the caller's
        /// framebuffer: its texels span that box evenly, the way its producer
        /// reads them. False for a table (a LUT, an atlas) that maps onto no
        /// area of the frame. As of the frame it was last marked current.
        bool placed = false;
        glm::vec2 origin{0.0f}; ///< When @c placed: the box's lower-left corner, caller's framebuffer px, y up.
        glm::vec2 size{0.0f};   ///< When @c placed: the box's extent, px.

        /// Whether this slot holds a record (a free slot does not).
        bool IsRegistered() const { return producer != nullptr; }
        /// The attachment's GL texture name; 0 once the producer released it,
        /// and before it first allocated it.
        unsigned int GetTexture() const;
        /// Its size in texels.
        glm::ivec2 GetTexels() const;
        /// The GL internal format it is allocated in (GL_RGBA16F, ...).
        int GetInternalFormat() const;
        /// 1, 2, 3 or 4, from the internal format; 0 if unknown.
        int GetChannels() const;
    } BufferRecord;

    /// Writes the key of attachment @p attachment of the Framebuffer named
    /// @p name into @p out: the name, then "[n]" for an attachment past the
    /// first. False, with @p out empty, when it does not fit in
    /// @ref BUFFER_KEY_CAPACITY or @p attachment is negative.
    bool MakeBufferKey(const char *name, int attachment, char (&out)[BUFFER_KEY_CAPACITY]);

    /// The attachment a key names: n for a key ending in "[n]", else 0.
    int GetBufferKeyAttachment(std::string_view key);

    /// The offscreen buffers the renderers of one effect own, for a layer that
    /// reads another's output - today the debug layer's buffer view
    /// (@c DebugConfig::bufferView) - and a host UI that lists them.
    ///
    /// Held by the effect's @ref ResourceContext. A renderer registers each
    /// buffer once and marks it current on every frame its content describes:
    /// written this frame, or a cache it checked and found unchanged. A
    /// consumer finds a record by key and asks @ref IsCurrent - a record its
    /// producer did not mark this frame (a layer that is off, a buffer the
    /// config does not use) is listed but not current.
    ///
    /// Steady state allocates nothing and searches nothing: registration
    /// happens once, and marking a record is one store through its slot.
    class BufferRegistry
    {
    public:
        /// No slot.
        static constexpr int INVALID_SLOT = -1;

        /// Starts a frame: every record stops being current until its producer
        /// marks it again. @c EdgeLightingEffect::Render, before any renderer
        /// draws.
        void BeginFrame() { ++mFrame; }

        /// Ends the frames for good: the effect that began them is gone. No
        /// record is current from here on, and marks are ignored. The records
        /// stay listed until their producers go - a renderer the host kept
        /// still owns its buffers - but without an effect there is no frame
        /// for "current" to describe, and a kept renderer drawn on its own
        /// would otherwise leave every record it last marked current for good.
        /// @c ~EdgeLightingEffect.
        void Close() { mClosed = true; }

        /// Lists attachment @p attachment of @p buffer under @p key, owned by
        /// @p producer. Refused - logged, and @ref INVALID_SLOT returned - for
        /// an empty key, one that does not fit in @ref BUFFER_KEY_CAPACITY, or
        /// one another record already holds: one key, one buffer (two neons in
        /// one effect list the first one's buffers only).
        /// @returns the record's slot, stable until @ref Unregister.
        int Register(const BaseRenderer *producer, const char *key, const Framebuffer &buffer, int attachment,
                     BufferVisibility visibility);

        /// The same, for a LUT: a texture its owner bakes on the CPU and
        /// uploads, a table by nature.
        int Register(const BaseRenderer *producer, const char *key, const BaseLUT &lut, BufferVisibility visibility);

        /// Removes the record in @p slot; its slot is reused by a later
        /// registration. Ignores @ref INVALID_SLOT.
        void Unregister(int slot);

        /// Marks the record in @p slot current this frame as a TABLE - a buffer
        /// that maps onto no area of the frame. @p changed when its bytes
        /// changed since the last frame it was marked (bumps
        /// @c BufferRecord::version).
        void MarkCurrent(int slot, bool changed);

        /// The same, for a buffer whose texels span the box @p origin,
        /// @p size of the caller's framebuffer (px, from its lower-left
        /// corner, y up) - the box its producer reads it over.
        void MarkCurrent(int slot, const glm::vec2 &origin, const glm::vec2 &size, bool changed);

        /// Whether @p record describes the frame being drawn (or, between
        /// frames, the last one): marked by its producer since the last
        /// @ref BeginFrame, and allocated - and never once the registry is
        /// closed (@ref Close).
        bool IsCurrent(const BufferRecord &record) const
        {
            return !mClosed && record.IsRegistered() && record.frame == mFrame && record.GetTexture() != 0;
        }

        /// The record registered under @p key, current or not, or nullptr.
        ///
        /// The pointer stays valid for the registry's life: a registration
        /// adds a slot without moving the others. It points at a SLOT,
        /// though, not at the buffer: once that record is removed the slot
        /// reads @c IsRegistered false, and a later registration may reuse it
        /// for another key. A reader that keeps the pointer across frames
        /// checks @c IsRegistered and the key, or simply calls Find each frame.
        const BufferRecord *Find(std::string_view key) const;

        /// Every slot, in registration order. A free slot is in the list with
        /// @c BufferRecord::IsRegistered false - skip it. A deque, so a slot
        /// never moves (see @ref Find).
        const std::deque<BufferRecord> &GetRecords() const { return mRecords; }

        /// How many slots hold a record.
        int GetRecordCount() const { return mRecordCount; }

        /// Moves on every registration and every removal, so two equal values
        /// mean the same records - unlike the count, which a removal and a
        /// registration leave where it was.
        uint64_t GetGeneration() const { return mGeneration; }

    private:
        /// Stores @p record under @p key, past the checks both
        /// @ref Register overloads share.
        int insert(const char *key, BufferRecord record);

    private:
        /// The slots. A deque rather than a vector: a registration grows it
        /// without moving a slot, so no record pointer a reader holds
        /// (@ref Find) dangles because another renderer joined.
        std::deque<BufferRecord> mRecords;
        int mRecordCount = 0;
        /// The last @c BufferRecord::version handed out.
        uint64_t mLastVersion = 0;
        /// Moves on every registration and every removal: what a listing, or
        /// the read diagnostics, compare to see whether the records changed.
        uint64_t mGeneration = 0;
        /// Frames begun so far; starts at 1 so a record never marked (frame 0)
        /// is never current.
        uint64_t mFrame = 1;
        bool mClosed = false; ///< Set by @ref Close: no frame will begin again.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BUFFER_REGISTRY_H_
