#ifndef _EDGE_LIGHTING_FRAMEBUFFER_H_
#define _EDGE_LIGHTING_FRAMEBUFFER_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <string>
#include <utility>

namespace EdgeLighting
{
    /// Maximum colour attachments a @ref Framebuffer can carry. 2 covers the
    /// emission pre-pass (hue + coverage split across two RGBA16F targets);
    /// raise it if a pass ever needs more.
    constexpr int FRAMEBUFFER_MAX_ATTACHMENTS = 2;

    /// RAII wrapper around a GL framebuffer + 1..FRAMEBUFFER_MAX_ATTACHMENTS
    /// colour attachments.
    ///
    /// Typical use is "render to texture, then sample it in a later pass":
    /// @code
    ///     mBuffer.Resize(w, h);   // no-op when size unchanged
    ///     mBuffer.Bind();         // sets framebuffer AND viewport
    ///     // ... draw ...
    ///     Framebuffer::BindDefault();
    ///     glViewport(0, 0, w, h);
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
              mWidth(other.mWidth),
              mHeight(other.mHeight),
              mAttachments(other.mAttachments),
              mInternalFormat(other.mInternalFormat),
              mName(std::move(other.mName))
        {
            for (int i = 0; i < FRAMEBUFFER_MAX_ATTACHMENTS; ++i)
            {
                mTextures[i] = other.mTextures[i];
                other.mTextures[i] = 0;
            }
            other.mFbo = 0;
            other.mWidth = 0;
            other.mHeight = 0;
        }

        Framebuffer &operator=(Framebuffer &&other) noexcept
        {
            if (this != &other)
            {
                destroy();
                mFbo = other.mFbo;
                mWidth = other.mWidth;
                mHeight = other.mHeight;
                mAttachments = other.mAttachments;
                mInternalFormat = other.mInternalFormat;
                mName = std::move(other.mName);
                for (int i = 0; i < FRAMEBUFFER_MAX_ATTACHMENTS; ++i)
                {
                    mTextures[i] = other.mTextures[i];
                    other.mTextures[i] = 0;
                }
                other.mFbo = 0;
                other.mWidth = 0;
                other.mHeight = 0;
            }
            return *this;
        }

        /// Allocates or resizes the colour attachments to @p width × @p height.
        /// No-op when the FBO already exists at the requested size, format and
        /// attachment count - safe to call every frame from the render loop.
        /// Logs a warning with the FBO's name if the framebuffer ends up
        /// incomplete.
        ///
        /// @p internalFormat selects the attachment format. GL_RGBA8 is the
        /// default; GL_RGBA16F is needed by the emission pre-pass, whose
        /// coverage channels carry per-arc intensity and so exceed 1.0.
        /// @p attachments > 1 sets up MRT and calls glDrawBuffers, which is
        /// framebuffer state, so it survives for the FBO's lifetime.
        /// @return @c true on success (or no-op); @c false on failure.
        bool Resize(int width, int height, GLenum internalFormat = GL_RGBA8, int attachments = 1)
        {
            if (width <= 0 || height <= 0)
            {
                LOG_E("Framebuffer[%s]: invalid size %dx%d requested.", mName.c_str(), width, height);
                return false;
            }

            if (attachments < 1 || attachments > FRAMEBUFFER_MAX_ATTACHMENTS)
            {
                LOG_E("Framebuffer[%s]: %d attachments requested, max is %d.",
                      mName.c_str(), attachments, FRAMEBUFFER_MAX_ATTACHMENTS);
                return false;
            }

            if (mFbo != 0 && width == mWidth && height == mHeight &&
                internalFormat == mInternalFormat && attachments == mAttachments)
            {
                return true;
            }

            destroy();

            // Pixel format/type that pairs with the requested internal format.
            // Only the two the renderers actually use are supported.
            const GLenum type = (internalFormat == GL_RGBA16F) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;

            glGenFramebuffers(1, &mFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);

            GLenum drawBuffers[FRAMEBUFFER_MAX_ATTACHMENTS];
            for (int i = 0; i < attachments; ++i)
            {
                glGenTextures(1, &mTextures[i]);
                glBindTexture(GL_TEXTURE_2D, mTextures[i]);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0,
                             GL_RGBA, type, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
                glFramebufferTexture2D(GL_FRAMEBUFFER, drawBuffers[i], GL_TEXTURE_2D, mTextures[i], 0);
            }
            glDrawBuffers(attachments, drawBuffers);

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                LOG_E("Framebuffer[%s] incomplete after resize to %dx%d (status=0x%x).",
                      mName.c_str(), width, height, status);
                destroy();
                return false;
            }

            mWidth = width;
            mHeight = height;
            mAttachments = attachments;
            mInternalFormat = internalFormat;
            LOG_I("Framebuffer[%s] sized to %dx%d (id=%u, tex=%u, attachments=%d).",
                  mName.c_str(), mWidth, mHeight, mFbo, mTextures[0], mAttachments);
            return true;
        }

        /// Activates this framebuffer for rendering and sets the GL viewport
        /// to match its dimensions. Caller is expected to clear if desired.
        void Bind() const
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glViewport(0, 0, mWidth, mHeight);
        }

        /// Restores the default framebuffer. Does NOT touch the viewport - the
        /// caller is responsible for setting it back to the window size.
        static void BindDefault()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        /// Binds colour attachment @p attachment to texture unit @p unit
        /// (both default to 0) for sampling in a subsequent pass.
        void BindTexture(GLuint unit = 0, int attachment = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, mTextures[attachment]);
        }

        bool IsValid() const { return mFbo != 0; }
        int GetWidth() const { return mWidth; }
        int GetHeight() const { return mHeight; }
        int GetAttachmentCount() const { return mAttachments; }
        GLuint GetId() const { return mFbo; }
        GLuint GetTextureId(int attachment = 0) const { return mTextures[attachment]; }
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
            for (int i = 0; i < FRAMEBUFFER_MAX_ATTACHMENTS; ++i)
            {
                if (mTextures[i] != 0)
                {
                    glDeleteTextures(1, &mTextures[i]);
                    mTextures[i] = 0;
                }
            }
            mWidth = 0;
            mHeight = 0;
            mAttachments = 1;
        }

    private:
        GLuint mFbo = 0;
        GLuint mTextures[FRAMEBUFFER_MAX_ATTACHMENTS] = {0, 0};
        int mWidth = 0;
        int mHeight = 0;
        int mAttachments = 1;
        GLenum mInternalFormat = GL_RGBA8;
        std::string mName = "unnamed";
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FRAMEBUFFER_H_
