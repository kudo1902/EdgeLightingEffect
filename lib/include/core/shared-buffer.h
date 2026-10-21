#ifndef _EDGE_LIGHTING_SHARED_BUFFER_H_
#define _EDGE_LIGHTING_SHARED_BUFFER_H_

#include "gl/framebuffer.h"
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace EdgeLighting
{
    /// One layer's offscreen image, shared with the other renderers: the GL
    /// buffer itself and the state that says what it holds. The producer draws
    /// into it as part of its own frame; readers sample it. Nothing is drawn
    /// for a reader and nothing is copied.
    ///
    /// Owned by shared pointer between the producer and the effect's
    /// @ref SharedBufferRegistry, so a reader never holds a dangling one: a producer
    /// that goes away releases the GL buffer and leaves the object not ready.
    ///
    /// @par The contract a reader relies on
    ///   - It covers the WHOLE viewport it was drawn for (@ref GetViewport): a
    ///     fragment of a pass drawn at that viewport finds its own pixel at
    ///     <tt>uv = gl_FragCoord.xy / viewport</tt>.
    ///   - Colour attachment 0, filtered GL_LINEAR, clamped to its edge.
    ///   - @ref IsReady holds this frame's image from the end of the
    ///     producer's offscreen phase (BaseRenderer::RenderOffscreen), which
    ///     the effect runs for every layer before any layer composites - so a
    ///     reader's Render sees this frame's image wherever it sits in the
    ///     layer order.
    ///   - @ref GetVersion moves whenever the producer redraws it, so an
    ///     unchanged version means unchanged texels.
    /// A reader binds it and samples it, and nothing else: the write side of
    /// this class is the producer's alone, and a reader holds it const.
    ///
    /// @par Writes
    /// The producer allocates and clears it through @ref BeginWrite, which
    /// keeps the format tier, the version and which texels may hold anything
    /// but 0 - so a write clears only the window it draws into and the one the
    /// last write drew, rather than the whole viewport-sized buffer.
    class SharedBuffer
    {
    public:
        /// A format the buffer may be allocated in.
        typedef struct Format
        {
            GLint internalFormat; ///< The sized internal format, e.g. GL_RGBA16F.
            GLenum format;        ///< The pixel format of a transfer, e.g. GL_RGBA.
            GLenum type;          ///< Its component type, e.g. GL_HALF_FLOAT.
            const char *name;     ///< For the fallback log line.
        } Format;

        /// @p name labels the buffer in log messages. @p formats in preference
        /// order, best first: each allocation walks them from the best one the
        /// driver has not yet refused. @p consequence says, in the log line a
        /// fallback writes, what the next format costs. Creates no GL object.
        SharedBuffer(const char *name, std::vector<Format> formats, const char *consequence);

        SharedBuffer(const SharedBuffer &) = delete;
        SharedBuffer &operator=(const SharedBuffer &) = delete;

        // --- Read: any renderer ----------------------------------------------

        /// Whether the buffer holds this frame's image.
        bool IsReady() const { return mReady && mBuffer.IsValid(); }

        /// Bind colour attachment 0 to texture unit @p unit.
        void BindTexture(GLuint unit) const { mBuffer.BindTexture(unit); }

        /// The GL texture name of attachment 0; 0 while unallocated. For a
        /// reader that has to attach it elsewhere (a read-back); bind it with
        /// @ref BindTexture to sample it.
        GLuint GetTextureId() const { return mBuffer.GetTextureId(); }

        /// The buffer's size, texels.
        glm::ivec2 GetTexels() const { return glm::ivec2(mBuffer.GetWidth(), mBuffer.GetHeight()); }

        /// The viewport it was drawn for, px: uv (0, 0) to (1, 1) spans it.
        glm::ivec2 GetViewport() const { return mViewport; }

        /// Writes so far. Compared only for equality.
        uint32_t GetVersion() const { return mVersion; }

        // --- Write: the producer alone ---------------------------------------

        /// Size the buffer to @p texels in the best format left, bind it, and
        /// clear what may hold anything but 0: @p window - [x0, x1) x [y0, y1)
        /// texels, as x0, y0, x1, y1, where this write draws - and the last
        /// write's window, or the whole buffer when its texels are undefined
        /// (fresh, reallocated or released). Moves the version.
        ///
        /// The caller lifts the host's scissor first and keeps it lifted
        /// through its draw (GLUtils::NoScissorScope): the buffer is the
        /// caller's viewport at a reduced scale, so a host scissor in window
        /// px lands on the wrong texels. The clear's own scissor is put back
        /// as it was found.
        /// @return false if no format allocates, in which case nothing is bound
        ///         - binding then would bind the CALLER's framebuffer - and the
        ///         caller must draw nothing.
        bool BeginWrite(const glm::ivec2 &texels, const glm::ivec4 &window);

        /// Treat the texels as undefined: the next @ref BeginWrite clears the
        /// whole buffer rather than two windows. Moves nothing else. For
        /// verification (NeonBypass::CLEAR) - a producer never needs it.
        void ForgetContents() { mDirtyKnown = false; }

        /// This frame's image is in the buffer, drawn or kept from an earlier
        /// frame, for a viewport of @p viewport px.
        void MarkReady(const glm::ivec2 &viewport);

        /// The buffer holds nothing to read this frame: the layer drew no
        /// image, or a pass failed.
        void MarkNotReady() { mReady = false; }

        /// Delete the GL buffer. Not ready, and the next write clears it whole.
        void Release();

    private:
        Framebuffer mBuffer;          ///< The image: one attachment.
        std::vector<Format> mFormats; ///< Candidate formats, best first.
        size_t mTier = 0;             ///< Index of the best format the driver has not refused. Only ever advances.
        std::string mConsequence;     ///< What a fallback costs, for its log line.
        glm::ivec4 mDirty{0};         ///< The last write's window: every texel outside it is 0 while @c mDirtyKnown.
        bool mDirtyKnown = false;     ///< False while the texels are undefined.
        glm::ivec2 mViewport{0};      ///< See @ref GetViewport.
        uint32_t mVersion = 0;        ///< See @ref GetVersion.
        bool mReady = false;          ///< See @ref IsReady.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SHARED_BUFFER_H_
