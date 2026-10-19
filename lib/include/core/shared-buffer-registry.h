#ifndef _EDGE_LIGHTING_SHARED_BUFFER_REGISTRY_H_
#define _EDGE_LIGHTING_SHARED_BUFFER_REGISTRY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace EdgeLighting
{
    // Declared, not included: this header reaches every host through
    // base-renderer.h and edge-lighting.h, and shared-buffer.h brings the GL
    // loader with it - which collides with a host's own GL headers. A producer
    // or a reader, which calls into the buffer, includes core/shared-buffer.h
    // itself.
    class SharedBuffer;

    /// Which layer's offscreen image a @ref SharedBuffer is: one value per
    /// layer that shares a buffer it already draws with the other renderers.
    typedef enum class SharedBufferId : uint32_t
    {
        /// @ref NeonRenderer's reduced buffer: the neon - filament, halo and
        /// bloom - at @c NeonConfig::resolutionScale over the whole viewport,
        /// with no opaque fill and before the one-sided cut and the cutoffs.
        /// See the shared buffer section of @ref NeonRenderer.
        NEON = 0,
        COUNT ///< Not a buffer: how many there are.
    } SharedBufferId;

    /// The directory of the offscreen images the renderers of one
    /// @ref EdgeLightingEffect share: one slot per @ref SharedBufferId, holding
    /// its producer's @ref SharedBuffer. It allocates, draws and schedules
    /// nothing - the buffer does all of that; it only says which buffer is
    /// whose, lets one producer claim each slot and hands readers a read-only
    /// handle. Owned by the effect and handed to every renderer it registers
    /// (@ref BaseRenderer::GetSharedBufferRegistry), never to the host.
    ///
    /// A producer claims its slot once (@ref Register, from its Initialize) and
    /// gives it back when it goes (@ref Unregister); one producer per slot. A
    /// reader takes the buffer with @ref Find - in its Render, where the
    /// effect guarantees every layer's offscreen phase, and so this frame's
    /// image, has already run - and samples it while @c IsReady. There is no
    /// per-frame publish: the buffer itself says whether it holds a frame.
    ///
    /// Everything here runs on the GL thread.
    class SharedBufferRegistry
    {
    public:
        SharedBufferRegistry();
        ~SharedBufferRegistry();

        /// Producer: @p buffer is @p id's from now on. Refused, and logged, if
        /// another buffer holds the slot, or for a value that names no slot.
        bool Register(SharedBufferId id, const std::shared_ptr<SharedBuffer> &buffer);

        /// Producer: give @p id back, if @p buffer is the one holding it.
        void Unregister(SharedBufferId id, const SharedBuffer *buffer);

        /// Reader: @p id's buffer, held const, or nullptr while no producer
        /// holds the slot. Shared, so a reader may keep it across frames: it
        /// stays valid, reporting not ready once its producer is gone.
        std::shared_ptr<const SharedBuffer> Find(SharedBufferId id) const;

    private:
        /// Whether @p id names a slot; logs one that does not.
        static bool isValidId(SharedBufferId id);

    private:
        std::array<std::shared_ptr<SharedBuffer>, static_cast<size_t>(SharedBufferId::COUNT)> mBuffers; ///< One per slot; empty while no producer holds it.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SHARED_BUFFER_REGISTRY_H_
