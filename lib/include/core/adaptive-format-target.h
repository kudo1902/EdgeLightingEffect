#ifndef _EDGE_LIGHTING_ADAPTIVE_FORMAT_TARGET_H_
#define _EDGE_LIGHTING_ADAPTIVE_FORMAT_TARGET_H_

#include "core/format-availability.h"
#include "gl/framebuffer.h"
#include "gl/gl-header.h"
#include <cstdint>
#include <string>

namespace EdgeLighting
{
    /// A render target allocated from a list of formats, best first: a
    /// Framebuffer that is only ever sized through the format walk - which
    /// lives in FormatAvailability, private, with this class its only way in,
    /// so nothing else can size a buffer from a list - and says what each
    /// call did to it. Every library buffer with a format list is one - the
    /// neon's emission table, coverage tables, gather buffer and two fields,
    /// the shared buffer, the flare's and the spotlight's scaled buffers.
    ///
    /// It exists for the one thing a buffer's owner has to get right that the
    /// walk cannot do for it: **texels undefined.** Any allocation leaves them
    /// so, whatever the size and format say. @ref Ensure returns ALLOCATED for
    /// it, and @ref GetGeneration moves. It moves, too, when the target loses
    /// its buffer (a walk that ends with none - FAILED - or @ref Release), so
    /// a cache keyed on it never takes a released buffer for the one it drew.
    /// Whoever keeps contents across frames keys its cache on the generation,
    /// never on the size or format.
    ///
    /// What the buffer IS - its name, its filter, what falling back costs it -
    /// is fixed at construction; what it walks is handed to each @ref Ensure,
    /// since a buffer's list may follow the config (the neon's coverage
    /// table, its fields). Non-copyable and non-movable: an owner keeps it as
    /// a member. GL thread only.
    /// See docs/design/render-target-format-check-plan.md.
    class AdaptiveFormatTarget
    {
    public:
        // --- Types -----------------------------------------------------------

        /// What a call left the target holding.
        typedef enum class EnsureResult
        {
            HELD,      ///< What it held before: nothing allocated, the texels as they were.
            ALLOCATED, ///< A new allocation - every texel undefined, whatever the size and format say.
            FAILED,    ///< No row allocated: the target holds no buffer.
        } EnsureResult;

        // --- Construction ----------------------------------------------------

        /// @p name labels the buffer in every log line about it, Framebuffer's
        /// and the walk's ("<name>: target unavailable in <format> ...").
        /// @p filter is the texture filter it is allocated with, for its life
        /// - GL_LINEAR or GL_NEAREST. @p fallbackCost ends the line a fallback
        /// writes: what the next format costs (copied; may be empty).
        /// Allocates nothing.
        AdaptiveFormatTarget(const char *name, GLint filter, const char *fallbackCost)
            : mBuffer(name), mFilter(filter), mFallbackCost(fallbackCost ? fallbackCost : "")
        {
        }

        AdaptiveFormatTarget(const AdaptiveFormatTarget &) = delete;
        AdaptiveFormatTarget &operator=(const AdaptiveFormatTarget &) = delete;

        // --- Sizing: the only way ---------------------------------------------

        /// Hold @p width x @p height x @p attachments in the best of
        /// @p formats this context may render at that size - THE format walk,
        /// the only way a buffer with a format list is sized, so the protocol
        /// lives in one place:
        ///   0. refuse a buffer wider or taller than the texture size limit
        ///      (FormatAvailability::GetMaxTextureSize) before trying any
        ///      row, keeping nothing: the limit is the context's, not a
        ///      format's;
        ///   1. ask @p availability for the first row to try (one lookup a
        ///      frame once probed): not refused, and no failure kept that
        ///      this allocation would repeat;
        ///   2. resize to it; if that fails, keep the failure in
        ///      @p availability and go on to the next row to try, logging "<name>: target unavailable in <row>
        ///      at W x H x A (<cause>), falling back to <next>.
        ///      <fallbackCost>" - a format the probe passed or could not judge
        ///      whose full-size allocation failed (memory, an attachment count
        ///      the driver will not take);
        ///   3. when no row may be tried or none allocates, FAILED - the
        ///      buffer released, not left holding an allocation sized for
        ///      another frame.
        ///
        /// STATELESS: everything a walk needs to remember is in
        /// @p availability - the probe results and the failures - so a buffer
        /// released between uses keeps nothing, and every walk on a format
        /// learns from any walk's failure. A buffer already holding the row
        /// the walk lands on costs Resize's early-out: HELD.
        ///
        /// A list of the host's own: ask its formats from the renderer's
        /// Initialize (FormatAvailability::Ask), or the first walk probes them
        /// mid-frame, clearing the frame's pending GL errors - the startup
        /// report asks only the library's lists (LIBRARY_FORMAT_LISTS).
        EnsureResult Ensure(FormatAvailability &availability, const FormatList &formats, int width, int height,
                            int attachments = 1);

        /// Moves with every allocation (Framebuffer::GetAllocationCount) and
        /// every loss of a buffer the target held - a walk that ends with none,
        /// or @ref Release - and never otherwise: a cache over the buffer's
        /// texels keys on it (V37 was a released buffer a cache still took
        /// for the one it drew). Compared only for equality.
        uint32_t GetGeneration() const { return mBuffer.GetAllocationCount() + mLosses; }

        // --- What it holds, and drawing it ------------------------------------

        bool IsValid() const { return mBuffer.IsValid(); }
        int GetWidth() const { return mBuffer.GetWidth(); }
        int GetHeight() const { return mBuffer.GetHeight(); }
        /// The sized internal format held, valid only while @ref IsValid.
        GLint GetInternalFormat() const { return mBuffer.GetInternalFormat(); }
        int GetAttachmentCount() const { return mBuffer.GetAttachmentCount(); }
        GLuint GetTextureId(int attachment = 0) const { return mBuffer.GetTextureId(attachment); }
        const char *GetName() const { return mBuffer.GetName(); }

        /// Framebuffer::Bind: the buffer as the draw target, and its viewport.
        void Bind() const { mBuffer.Bind(); }
        /// Framebuffer::BindTexture.
        void BindTexture(GLuint unit = 0, int attachment = 0) const { mBuffer.BindTexture(unit, attachment); }
        /// Framebuffer::ClearBuffer - of whatever is bound: Bind first.
        void ClearBuffer() const { mBuffer.ClearBuffer(); }
        /// Framebuffer::ClearBufferRect - of whatever is bound: Bind first.
        void ClearBufferRect(GLint x, GLint y, GLsizei width, GLsizei height) const
        {
            mBuffer.ClearBufferRect(x, y, width, height);
        }

        /// Give the buffer back. The next @ref Ensure allocates afresh. Moves
        /// the generation if the target held a buffer: its texels are gone.
        void Release()
        {
            if (mBuffer.IsValid())
            {
                ++mLosses;
            }
            mBuffer.Release();
        }

    private:
        Framebuffer mBuffer;       ///< The buffer; its label is the target's name.
        GLint mFilter;             ///< What every allocation is made with.
        std::string mFallbackCost; ///< What the next format costs, for a fallback's log line.
        uint32_t mLosses = 0;      ///< Buffers this target held and lost - its share of @ref GetGeneration.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ADAPTIVE_FORMAT_TARGET_H_
