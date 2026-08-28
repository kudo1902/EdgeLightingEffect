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
        ///       Renderers restore the viewport by reconstruction rather than
        ///       by querying GL_VIEWPORT; @ref BaseRenderer::Render documents
        ///       why that is sufficient.
        void Bind() const
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glViewport(0, 0, mWidth, mHeight);
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

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FRAMEBUFFER_H_
