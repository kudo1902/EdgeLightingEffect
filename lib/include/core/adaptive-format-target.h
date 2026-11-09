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
    /// It exists for the two things a buffer's owner has to get right that
    /// the walk cannot do for it:
    ///   - **Texels undefined.** Any allocation leaves them so, whatever the
    ///     size and format say: a retry of a better format that runs out of
    ///     memory destroys the buffer, and the walk then allocates the format
    ///     it had at the size it had - a fresh texture under, often, the same
    ///     name. @ref Ensure and @ref Refresh return ALLOCATED for it, and
    ///     @ref GetGeneration moves. It moves, too, when the target loses
    ///     its buffer (a walk that ends with none - FAILED - or @ref Release),
    ///     so a cache keyed on it never takes a released buffer for the one
    ///     it drew. Whoever keeps contents across frames either reacts
    ///     where it calls them, or keys its cache on the generation - never
    ///     on the size or format.
    ///   - **Moving back up.** A buffer that fell back past a format whose
    ///     out-of-memory failure has since expired is moved back up by its
    ///     next walk - but a frame that reuses the buffer runs no walk.
    ///     @ref Refresh is that walk, at the size the buffer holds, and does
    ///     nothing unless a better row may be allocated now: a renderer calls
    ///     it for every target a frame can skip, in one place, before any
    ///     skip is decided. A move it makes is an allocation like any other,
    ///     so the generation keys above see it.
    ///
    /// What precision a buffer holds is a separate question - a reallocation
    /// in the same format recomputes the same texels, so a consumer that only
    /// cares about precision (the neon's fields, FieldInputFormats) keys on
    /// the format instead.
    ///
    /// What the buffer IS - its name, its filter, what falling back costs it -
    /// is fixed at construction; what it walks is handed to each @ref Ensure,
    /// since a buffer's list may follow the config (the neon's coverage
    /// table, its fields). Non-copyable and non-movable: an owner keeps it as
    /// a member. It keeps the last list it walked, for @ref Refresh, so the
    /// list has to outlive the target - one of the library's lists
    /// (LIBRARY_FORMAT_LISTS), or a vector its owner keeps. GL thread only.
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
        ///      @p availability by what Resize said failed it and go on to the
        ///      next row to try, logging "<name>: target unavailable in <row>
        ///      at W x H x A (<cause>), falling back to <next>.
        ///      <fallbackCost>" - a format the probe passed or could not judge
        ///      whose full-size allocation failed (memory, an attachment count
        ///      the driver will not take);
        ///   3. when no row may be tried or none allocates, FAILED - the
        ///      buffer released, not left holding an allocation sized for
        ///      another frame.
        ///
        /// STATELESS: everything a walk needs to remember is in
        /// @p availability - the probe results and the failures, each by its
        /// cause - so a buffer released between uses keeps nothing, every
        /// walk on a format learns from any walk's failure that says
        /// something about it, and a format that failed at one size is tried
        /// again at a smaller one - and, out of memory, at any size once the
        /// failure expires. A buffer already holding the row the walk lands
        /// on costs Resize's early-out: HELD. Keeps @p formats - the pointer,
        /// not the list - for @ref Refresh.
        ///
        /// A list of the host's own: ask its formats from the renderer's
        /// Initialize (FormatAvailability::Ask), or the first walk probes them
        /// mid-frame, clearing the frame's pending GL errors - the startup
        /// report asks only the library's lists (LIBRARY_FORMAT_LISTS).
        EnsureResult Ensure(FormatAvailability &availability, const FormatList &formats, int width, int height,
                            int attachments = 1);

        /// Move the buffer back up its list, at the size and attachment count
        /// it holds, when a row above the one it holds may be allocated now -
        /// an out-of-memory failure it fell back past has expired. HELD
        /// otherwise, or when it holds nothing. A free lookup while the buffer
        /// holds its first row.
        EnsureResult Refresh(FormatAvailability &availability);

        /// Moves with every allocation (Framebuffer::GetAllocationCount) and
        /// every loss of a buffer the target held - a walk that ends with none,
        /// or @ref Release - and never otherwise: a cache over the buffer's
        /// texels keys on it. Without the losses, a retry and its fallback
        /// that both ran out of memory left the generation where it was, and
        /// the neon's still frames went on blitting the released shared
        /// buffer - opaque black over 94% of a 1280 x 720 frame. Compared
        /// only for equality.
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
        FormatList mFormats;       ///< The last list walked, what Refresh walks again; empty before the first.
        uint32_t mLosses = 0;      ///< Buffers this target held and lost - its share of @ref GetGeneration.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_ADAPTIVE_FORMAT_TARGET_H_
