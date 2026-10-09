#ifndef _NEON_TOOLS_PASS_RECORDER_H_
#define _NEON_TOOLS_PASS_RECORDER_H_

// Watches NeonRenderer draw one frame, from the outside. Shared by the tools:
// neon-guide-figures reads the renderer's buffers between passes for the
// guide's figures, and neon-scale-check `partition` replays the blit's and the
// ring's triangles to count how often each pixel is covered.
//
// Both need what the renderer keeps private - the emission table, the gather
// buffer, the reduced buffer, and the triangles each pass draws. None of that
// should become public for a tool's sake. So this records it the way a GPU
// debugger would: GLAD reaches every GL entry point through a global function
// pointer, and this swaps glad_glDrawArrays (the only draw call the library
// makes) for a wrapper. Every draw still goes through untouched; after it, the
// wrapper names the pass from the program's uniforms, records its vertex array
// and transform, and - unless told not to - reads back whatever it wrote.
//
// That works only for a binary linked against the STATIC library with its
// own copy of glad (the C ABI dylib carries a private one), and a readback per
// draw costs time - a tool's trade, not a library's.

#include "gl/gl-header.h"

#include <glm/glm.hpp>
#include <vector>

namespace NeonTools
{
    /// Which neon pass a draw belongs to. The names are the guide's (Part 6).
    typedef enum class PassKind
    {
        OTHER,    ///< Anything else - the debug layer's overlays.
        P0,       ///< Emission table (neon-emission.frag).
        P0B,      ///< Glow coverage table (neon-glow-cover.frag, uBakeTarget 0).
        P0S,      ///< Segment coverage table (neon-glow-cover.frag, uBakeTarget 1).
        P0F,      ///< The segment table's fill into the glow coverage table (neon-glow-cover-fill.frag).
        P1A,      ///< Gather pass (neon-gather.frag).
        P1B,      ///< Reduced-scale shading (neon.frag, offscreen, below scale 1.0).
        P1F,      ///< Field bake (neon.frag + NEON_FIELD_BAKE, offscreen): the glow's field, then the ring's (P1r).
        P1C,      ///< Field composite (neon-field.frag, into the reduced buffer in P1B's place).
        P2A,      ///< Opaque fill (black-rect.frag).
        P2B,      ///< Blit (neon-blit.frag).
        P2C       ///< Edge ring (neon.frag, onto the target), or its field's composite (P2r).
    } PassKind;

    /// What one colour attachment held right after a draw: RGBA floats, GL row
    /// order (bottom row first), whatever the attachment's format.
    typedef struct Attachment
    {
        int width = 0;
        int height = 0;
        GLint internalFormat = 0;
        std::vector<glm::vec4> texels;

        const glm::vec4 &At(int x, int y) const { return texels[size_t(y) * size_t(width) + size_t(x)]; }
    } Attachment;

    /// One recorded draw.
    typedef struct DrawRecord
    {
        PassKind kind = PassKind::OTHER;
        GLuint program = 0;
        GLuint vertexArray = 0;
        GLenum mode = GL_TRIANGLES;
        GLint first = 0;
        GLsizei count = 0;
        glm::mat4 mvp{1.0f};
        glm::vec2 uvScale{0.0f};          ///< The blit's uUVScale, when kind is P2B.
        glm::vec2 uvOffset{0.0f};         ///< The blit's uUVOffset, when kind is P2B.
        std::vector<Attachment> written;  ///< Every colour attachment of the target, after the draw (empty with readback off).
        GLuint timerQuery = 0;            ///< The draw's GL_TIME_ELAPSED query, with timing on, until ResolveTimes.
        double gpuMs = -1.0;              ///< The draw's GPU time, ms, once ResolveTimes has run; -1 without timing.
    } DrawRecord;

    /// @p kind's short name, as the tools print it ("P0b", "P2c", ...).
    const char *PassKindName(PassKind kind);

    class PassRecorder
    {
    public:
        /// Swap the draw entry point. Call once, after gladLoadGLLoader.
        static void Install();

        /// Delete the GL objects this owns. Call while the context is alive.
        static void Release();

        /// Whether a recorded draw reads its target back into
        /// @ref DrawRecord::written. On by default; turn it off when only the
        /// geometry matters - a float readback of a full frame per draw is
        /// most of what recording costs.
        static void SetReadback(bool enabled);

        /// Whether a recorded draw is wrapped in a GL_TIME_ELAPSED query, for
        /// a per-pass GPU time (@ref DrawRecord::gpuMs, filled by
        /// @ref ResolveTimes). Off by default. The queries serialise the
        /// draws, so a frame timed this way is slower than one that is not:
        /// read the passes' shares, and take the frame's own time from a
        /// query round the whole Render.
        static void SetTiming(bool enabled);

        /// Wait for every timed draw's query, store its time in
        /// @ref DrawRecord::gpuMs and delete the query.
        static void ResolveTimes();

        /// Record every draw until @ref End. The framebuffer bound for drawing
        /// at this moment is taken as the caller's, which is how a draw onto it
        /// is told apart from one into the renderer's own buffers.
        static void Begin();
        static void End();

        static const std::vector<DrawRecord> &GetDraws();

        /// The last recorded draw of @p kind, or nullptr.
        static const DrawRecord *Find(PassKind kind);

        /// Draw @p record's triangles again, with the same vertex array and
        /// transform, in one flat colour - as a filled area, or as the
        /// triangles' edges when @p wire is true. Draws into whatever is bound;
        /// the caller sets the viewport and the blending.
        static void Redraw(const DrawRecord &record, const glm::vec4 &colour, bool wire);

        /// Redraw @p record's vertex array through @p mvp instead of its own
        /// transform - for geometry recorded in another space.
        static void Redraw(const DrawRecord &record, const glm::mat4 &mvp, const glm::vec4 &colour, bool wire);
    };
}

#endif
