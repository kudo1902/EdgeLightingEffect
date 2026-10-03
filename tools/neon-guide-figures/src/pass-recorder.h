#ifndef _NEON_GUIDE_FIGURES_PASS_RECORDER_H_
#define _NEON_GUIDE_FIGURES_PASS_RECORDER_H_

// Watches NeonRenderer draw one frame, from the outside.
//
// The guide's pass figures need what the renderer's private buffers hold
// between passes - the emission table, the gather buffer, the reduced buffer -
// and the triangles each pass draws. None of that is public, and it should not
// become public just so a document can show it. So this records it the way a
// GPU debugger would: GLAD reaches every GL entry point through a global
// function pointer, and this swaps glad_glDrawArrays (the only draw call the
// library makes) for a wrapper. Every draw still goes through untouched; after
// it, the wrapper names the pass from the program's uniforms and reads back
// whatever that draw wrote.
//
// That works only for a binary linked against the STATIC library with its
// own copy of glad (the C ABI dylib carries a private one), and it costs a
// readback per draw - a tool's trade, not a library's.

#include "canvas.h"
#include "gl/gl-header.h"

#include <glm/glm.hpp>
#include <vector>

namespace NeonGuideFigures
{
    /// Which neon pass a draw belongs to. The names are the guide's (Part 6).
    typedef enum class PassKind
    {
        OTHER,    ///< Anything else - the debug layer's overlays.
        P0,       ///< Emission table (neon-emission.frag).
        P1,       ///< Direct-path glow (neon.frag, plain).
        P1A,      ///< Gather pass (neon-gather.frag).
        P1B,      ///< Reduced-scale shading (neon.frag + NEON_READS_GATHER, offscreen).
        P2A,      ///< Opaque fill (black-rect.frag).
        P2B,      ///< Blit (neon-blit.frag).
        P2C       ///< Edge ring (neon.frag + NEON_READS_GATHER, onto the target).
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
        std::vector<Attachment> written;  ///< Every colour attachment of the target, after the draw.
    } DrawRecord;

    class PassRecorder
    {
    public:
        /// Swap the draw entry point. Call once, after gladLoadGLLoader.
        static void Install();

        /// Delete the GL objects this owns. Call while the context is alive.
        static void Release();

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
