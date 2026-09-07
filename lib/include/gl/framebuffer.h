#ifndef _EDGE_LIGHTING_FRAMEBUFFER_H_
#define _EDGE_LIGHTING_FRAMEBUFFER_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <string>
#include <utility>

namespace EdgeLighting
{
    /// RAII wrapper around a GL framebuffer + a single colour attachment.
    ///
    /// The attachment defaults to RGBA8 / LINEAR; @ref Resize takes explicit
    /// format and filter parameters for callers that need otherwise (the
    /// emission pre-pass asks for RGBA16F / NEAREST).
    ///
    /// Typical use is "render to texture, then sample it in a later pass".
    /// Save the caller's target rather than assuming the default framebuffer -
    /// under an @c OffscreenCapture it is a real FBO:
    /// @code
    ///     const GLuint prev = Framebuffer::GetBoundId();
    ///     mBuffer.Resize(w, h);   // no-op when size unchanged
    ///     mBuffer.Bind();         // sets framebuffer AND viewport
    ///     // ... draw ...
    ///     Framebuffer::BindId(prev);
    ///     glViewport(0, 0, w, h); // Bind() changed it; put it back
    ///     mBuffer.BindTexture(0); // sample the result in the next pass
    /// @endcode
    ///
    /// Move-only ownership. Pass a @p name (e.g. via brace-init at declaration:
    /// @c Framebuffer mFoo{"MyPass.Foo"}; ) so log lines are attributable when
    /// several FBOs live in one process.
    class Framebuffer
    {
    public:
        Framebuffer() = default;

        explicit Framebuffer(const char *name)
            : mName(name ? name : "unnamed") {}

        ~Framebuffer() { destroy(); }

        Framebuffer(const Framebuffer &) = delete;
        Framebuffer &operator=(const Framebuffer &) = delete;

        Framebuffer(Framebuffer &&other) noexcept
            : mFbo(other.mFbo),
              mTexture(other.mTexture),
              mWidth(other.mWidth),
              mHeight(other.mHeight),
              mInternalFormat(other.mInternalFormat),
              mFilter(other.mFilter),
              mName(std::move(other.mName))
        {
            other.mFbo = 0;
            other.mTexture = 0;
            other.mWidth = 0;
            other.mHeight = 0;
        }

        Framebuffer &operator=(Framebuffer &&other) noexcept
        {
            if (this != &other)
            {
                destroy();
                mFbo = other.mFbo;
                mTexture = other.mTexture;
                mWidth = other.mWidth;
                mHeight = other.mHeight;
                mInternalFormat = other.mInternalFormat;
                mFilter = other.mFilter;
                mName = std::move(other.mName);
                other.mFbo = 0;
                other.mTexture = 0;
                other.mWidth = 0;
                other.mHeight = 0;
            }
            return *this;
        }

        /// Allocates or resizes the colour attachment to @p width × @p height.
        /// No-op when the FBO already exists at the requested size **and** the
        /// same format / filter - safe to call every frame from the render loop.
        /// Logs a warning with the FBO's name if the framebuffer ends up
        /// incomplete.
        ///
        /// The format parameters default to the historical RGBA8 / LINEAR
        /// behaviour, so existing callers are unaffected. They exist for the
        /// emission pre-pass, which needs @c GL_RGBA16F (segment boosts stack
        /// above 1.0) and @c GL_NEAREST (its consumer uses @c texelFetch, and a
        /// filtered read across sample boundaries would blend neighbouring
        /// perimeter samples together).
        ///
        /// @note Format and filter are tracked alongside the size, so a caller
        ///       that changes format on an existing FBO forces a reallocation
        ///       instead of silently keeping the old one.
        /// @note Leaves the draw framebuffer binding exactly as it found it,
        ///       including on the failure path. It binds this FBO internally to
        ///       attach and validate, then puts the caller's target back - it
        ///       must NOT settle on 0, because "the target I was handed" is a
        ///       real FBO under an @c OffscreenCapture, and a caller that reads
        ///       @ref GetBoundId after calling this would otherwise capture 0
        ///       and redirect its later passes to the window.
        /// @return @c true on success (or no-op); @c false on failure.
        bool Resize(int width, int height,
                    GLint internalFormat = GL_RGBA8, GLenum format = GL_RGBA,
                    GLenum type = GL_UNSIGNED_BYTE, GLint filter = GL_LINEAR)
        {
            if (width <= 0 || height <= 0)
            {
                LOG_E("Framebuffer[%s]: invalid size %dx%d requested.", mName.c_str(), width, height);
                return false;
            }

            if (mFbo != 0 && width == mWidth && height == mHeight &&
                internalFormat == mInternalFormat && filter == mFilter)
            {
                return true;
            }

            // Saved before the first bind below and restored on every exit -
            // see the @note above. Read as the DRAW binding, which is what
            // glBindFramebuffer(GL_FRAMEBUFFER, ...) writes.
            GLint prevFbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
            // A caller resizing the FBO it currently has bound leaves us
            // holding a name destroy() is about to delete; restoring that would
            // bind a deleted object. GL drops the binding to 0 in that case, so
            // follow it there.
            if (static_cast<GLuint>(prevFbo) == mFbo)
            {
                prevFbo = 0;
            }

            destroy();

            glGenTextures(1, &mTexture);
            glBindTexture(GL_TEXTURE_2D, mTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &mFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, 0);

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));

            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                LOG_E("Framebuffer[%s] incomplete after resize to %dx%d (status=0x%x).",
                      mName.c_str(), width, height, status);
                destroy();
                return false;
            }

            mWidth = width;
            mHeight = height;
            mInternalFormat = internalFormat;
            mFilter = filter;
            LOG_I("Framebuffer[%s] sized to %dx%d (id=%u, tex=%u).",
                  mName.c_str(), mWidth, mHeight, mFbo, mTexture);
            return true;
        }

        /// Activates this framebuffer for rendering and sets the GL viewport
        /// to match its dimensions. Caller is expected to clear if desired.
        ///
        /// @note Viewport travels with the target on purpose - a bound target
        ///       without a matching viewport is a half-configured state. A pass
        ///       that calls this must therefore restore BOTH (see
        ///       @ref GetBoundId / @ref BindId for the framebuffer half).
        ///       @c BaseRenderer::Render's @pre permits either capture or
        ///       reconstruction for the viewport half; this library's passes
        ///       capture, which under that precondition is the same four
        ///       integers and one fewer assumption to carry.
        void Bind() const
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glViewport(0, 0, mWidth, mHeight);
        }

        /// Clear the colour attachment - transparent black by default, which
        /// is what a premultiplied-alpha layer wants under it.
        ///
        /// @c glClearBufferfv, not @c glClearColor + @c glClear: the colour is
        /// an ARGUMENT, so no global clear-colour state is saved, overwritten
        /// and put back. GL 3.0 / GLES 3.0 core.
        ///
        /// @pre @ref Bind has run. A clear acts on whatever is BOUND, not on
        ///      the object it is called through, so without the bind this
        ///      wipes the framebuffer the caller was drawing into - under an
        ///      @c OffscreenCapture, the capture. Keep the two calls adjacent;
        ///      detecting it here would cost a @c glGetIntegerv per clear.
        ///
        /// @note The early-out covers the other half of that: @ref Resize
        ///       destroys the attachment when it fails, and a @ref Bind on the
        ///       wreckage binds framebuffer 0. No attachment, nothing to clear.
        ///
        /// @note SCISSOR still applies, and on an offscreen target it is
        ///       almost never wanted - the host's box is in the CALLER's
        ///       coordinate space, which this attachment is not in. Wrap the
        ///       whole excursion in a @c GLUtils::NoScissorScope; this method
        ///       cannot, because the guard has to cover the draws too.
        void ClearBuffer(GLfloat r = 0.0f, GLfloat g = 0.0f,
                         GLfloat b = 0.0f, GLfloat a = 0.0f) const
        {
            if (!IsValid())
            {
                return;
            }

            const GLfloat rgba[4] = {r, g, b, a};
            glClearBufferfv(GL_COLOR, 0, rgba);
        }

        /// Restores the default framebuffer. Does NOT touch the viewport - the
        /// caller is responsible for setting it back to the window size.
        ///
        /// Prefer @ref GetBoundId + @ref BindId in a multi-pass renderer:
        /// "the target I started on" is not always the default framebuffer
        /// (an offscreen frame capture makes it a real FBO).
        static void BindDefault()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        /// The id currently bound for drawing; 0 means the window's default
        /// framebuffer. Note this is the DRAW binding
        /// (GL_FRAMEBUFFER_BINDING is GL_DRAW_FRAMEBUFFER_BINDING) - code that
        /// splits read from draw, as the capture readback does, must save the
        /// read binding separately.
        ///
        /// @note This is a GL state query, and a state query can force a driver
        ///       sync - on a tile-based GPU (the Mali / Tizen targets) that is
        ///       the kind of call that stalls a pipeline. It is called once per
        ///       frame per multi-pass renderer, which is cheap enough that no
        ///       measurement has justified removing it.
        ///
        ///       Do NOT "optimise" it back to @ref BindDefault. The reason this
        ///       exists is that "the target I was handed" is a real FBO under an
        ///       @c OffscreenCapture, and assuming 0 sends the renderer's output
        ///       to the window while the capture comes back empty.
        ///
        ///       The query-free alternative is to thread the caller's target
        ///       through @c BaseRenderer::Render instead of asking GL for it.
        ///       That is a breaking change to the renderer plugin API - six
        ///       renderers and both demos - and is deliberately not taken
        ///       without a profile showing this query on the critical path.
        ///       See docs/review-findings.md I5.
        static GLuint GetBoundId()
        {
            GLint id = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &id);
            return static_cast<GLuint>(id);
        }

        /// Binds a raw framebuffer id for both reading and drawing. Inverse of
        /// @ref GetBoundId: pair them to save and restore the caller's target
        /// around an offscreen pass. Does NOT touch the viewport.
        static void BindId(GLuint id)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, id);
        }

        /// Binds the colour attachment texture to texture unit @p unit
        /// (defaults to GL_TEXTURE0) for sampling in a subsequent pass.
        void BindTexture(GLuint unit = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, mTexture);
        }

        bool IsValid() const { return mFbo != 0; }
        int GetWidth() const { return mWidth; }
        int GetHeight() const { return mHeight; }
        GLuint GetId() const { return mFbo; }

        /// The internal format currently backing the attachment, valid only
        /// while @ref IsValid. Exposed so a caller that wants a preferred
        /// format with a fallback can ask what it actually GOT rather than
        /// tracking that itself: re-requesting the preferred format after the
        /// driver refused it would churn the texture + FBO once per frame,
        /// because @ref Resize treats a format change as a reallocation and
        /// @c destroy resets this to the default on the failure path.
        GLint GetInternalFormat() const { return mInternalFormat; }
        GLuint GetTextureId() const { return mTexture; }
        const char *GetName() const { return mName.c_str(); }
        void SetName(const char *name) { mName = name ? name : "unnamed"; }

    private:
        void destroy()
        {
            if (mFbo != 0)
            {
                glDeleteFramebuffers(1, &mFbo);
                mFbo = 0;
            }
            if (mTexture != 0)
            {
                glDeleteTextures(1, &mTexture);
                mTexture = 0;
            }
            mWidth = 0;
            mHeight = 0;
            mInternalFormat = GL_RGBA8;
            mFilter = GL_LINEAR;
        }

    private:
        GLuint mFbo = 0;
        GLuint mTexture = 0;
        int mWidth = 0;
        int mHeight = 0;
        GLint mInternalFormat = GL_RGBA8; ///< Tracked so a format change forces a realloc.
        GLint mFilter = GL_LINEAR;        ///< Tracked for the same reason.
        std::string mName = "unnamed";
    };

    /// The render target a pass was handed: framebuffer id AND viewport box.
    ///
    /// One type because the two travel together - @ref Framebuffer::Bind
    /// writes both, since a bound target without a matching viewport is a
    /// half-configured state, so anything that puts one back owes the other.
    /// Saving them as a pair is what stops a restore from being half done.
    ///
    /// Captured rather than reconstructed as @c (0, 0, width, height).
    /// @c BaseRenderer::Render's @pre fixes the viewport there and permits
    /// either, so under that precondition the two agree; capture is simply one
    /// fewer assumption to carry, and it earns most where a pass can be
    /// SKIPPED, since a run frame and a skipped frame have to leave the same
    /// state behind.
    ///
    /// Two queries, so capture only where something actually retargets:
    /// @code
    ///     RenderTargetState prev;              // captures nothing
    ///     if (scaled) { prev = RenderTargetState::Capture(); }
    ///     // ... offscreen work ...
    ///     prev.Restore();                      // no-op unless captured
    /// @endcode
    /// An uncaptured state restores nothing, so a path that never leaves the
    /// caller's target pays neither the queries nor a bogus restore to
    /// framebuffer 0 at a zero-size viewport.
    typedef struct RenderTargetState
    {
        /// Read the framebuffer and viewport currently bound for DRAWING.
        /// Call before anything binds a target of its own.
        static RenderTargetState Capture()
        {
            RenderTargetState state;
            state.fbo = Framebuffer::GetBoundId();
            glGetIntegerv(GL_VIEWPORT, state.viewport);
            state.captured = true;
            return state;
        }

        /// Put both back, exactly as found. A no-op on a state that was never
        /// @ref Capture d - see the class note for why that matters.
        void Restore() const
        {
            if (!captured)
            {
                return;
            }

            Framebuffer::BindId(fbo);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        }

        GLuint fbo = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        bool captured = false; ///< False on a default-constructed state; see @ref Restore.
    } RenderTargetState;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FRAMEBUFFER_H_
