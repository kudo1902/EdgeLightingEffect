#ifndef _EDGE_LIGHTING_FRAMEBUFFER_H_
#define _EDGE_LIGHTING_FRAMEBUFFER_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <string>
#include <utility>

namespace EdgeLighting
{
    /// RAII wrapper around a GL framebuffer + single RGBA8 colour attachment.
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
              mTexture(other.mTexture),
              mWidth(other.mWidth),
              mHeight(other.mHeight),
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
                mName = std::move(other.mName);
                other.mFbo = 0;
                other.mTexture = 0;
                other.mWidth = 0;
                other.mHeight = 0;
            }
            return *this;
        }

        /// Allocates or resizes the RGBA8 colour attachment to @p width × @p height.
        /// No-op when the FBO already exists at the requested size — safe to call
        /// every frame from the render loop. Logs a warning with the FBO's name
        /// if the framebuffer ends up incomplete.
        /// @return @c true on success (or no-op); @c false on failure.
        bool Resize(int width, int height)
        {
            if (width <= 0 || height <= 0)
            {
                LOG_E("Framebuffer[%s]: invalid size %dx%d requested.", mName.c_str(), width, height);
                return false;
            }

            if (mFbo != 0 && width == mWidth && height == mHeight)
            {
                return true;
            }

            destroy();

            glGenTextures(1, &mTexture);
            glBindTexture(GL_TEXTURE_2D, mTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenFramebuffers(1, &mFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, 0);

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
            LOG_I("Framebuffer[%s] sized to %dx%d (id=%u, tex=%u).",
                  mName.c_str(), mWidth, mHeight, mFbo, mTexture);
            return true;
        }

        /// Activates this framebuffer for rendering and sets the GL viewport
        /// to match its dimensions. Caller is expected to clear if desired.
        void Bind() const
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glViewport(0, 0, mWidth, mHeight);
        }

        /// Restores the default framebuffer. Does NOT touch the viewport — the
        /// caller is responsible for setting it back to the window size.
        static void BindDefault()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
        }

    private:
        GLuint mFbo = 0;
        GLuint mTexture = 0;
        int mWidth = 0;
        int mHeight = 0;
        std::string mName = "unnamed";
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FRAMEBUFFER_H_
