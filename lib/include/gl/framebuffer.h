#ifndef _EDGE_LIGHTING_FRAMEBUFFER_H_
#define _EDGE_LIGHTING_FRAMEBUFFER_H_

#include "gl/gl-header.h"
#include "util/log-util.h"
#include <algorithm>
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
              mCount(other.mCount),
              mWidth(other.mWidth),
              mHeight(other.mHeight),
              mName(std::move(other.mName))
        {
            for (int i = 0; i < MAX_ATTACHMENTS; ++i)
            {
                mTextures[i] = other.mTextures[i];
                mAttachments[i] = other.mAttachments[i];
                other.mTextures[i] = 0;
            }
            other.mFbo = 0;
            other.mCount = 0;
            other.mWidth = 0;
            other.mHeight = 0;
        }

        Framebuffer &operator=(Framebuffer &&other) noexcept
        {
            if (this != &other)
            {
                destroy();
                mFbo = other.mFbo;
                mCount = other.mCount;
                mWidth = other.mWidth;
                mHeight = other.mHeight;
                mName = std::move(other.mName);
                for (int i = 0; i < MAX_ATTACHMENTS; ++i)
                {
                    mTextures[i] = other.mTextures[i];
                    mAttachments[i] = other.mAttachments[i];
                    other.mTextures[i] = 0;
                }
                other.mFbo = 0;
                other.mCount = 0;
                other.mWidth = 0;
                other.mHeight = 0;
            }
            return *this;
        }

        /// Pixel format of one colour attachment. The default is the RGBA8 that
        /// every FBO here used before multi-attachment support existed.
        typedef struct Attachment
        {
            GLenum internalFormat = GL_RGBA8;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;

            bool operator==(const Attachment &o) const
            {
                return internalFormat == o.internalFormat &&
                       format == o.format &&
                       type == o.type;
            }
            bool operator!=(const Attachment &o) const { return !(*this == o); }
        } Attachment;

        static constexpr int MAX_ATTACHMENTS = 2;

        /// Allocates or resizes the RGBA8 colour attachment to @p width × @p height.
        /// No-op when the FBO already exists at the requested size - safe to call
        /// every frame from the render loop. Logs a warning with the FBO's name
        /// if the framebuffer ends up incomplete.
        /// @return @c true on success (or no-op); @c false on failure.
        bool Resize(int width, int height)
        {
            const Attachment rgba8;
            return Resize(width, height, &rgba8, 1);
        }

        /// Single attachment with an explicit pixel format.
        bool Resize(int width, int height, const Attachment &a0)
        {
            return Resize(width, height, &a0, 1);
        }

        /// Two colour attachments (MRT) - the fragment shader writes
        /// `layout(location = 0/1)` outputs and Bind() sets up the draw
        /// buffers. Formats may differ between attachments.
        bool Resize(int width, int height, const Attachment &a0, const Attachment &a1)
        {
            const Attachment atts[2] = {a0, a1};
            return Resize(width, height, atts, 2);
        }

        bool Resize(int width, int height, const Attachment *atts, int count)
        {
            if (width <= 0 || height <= 0)
            {
                LOG_E("Framebuffer[%s]: invalid size %dx%d requested.", mName.c_str(), width, height);
                return false;
            }
            if (atts == nullptr || count < 1 || count > MAX_ATTACHMENTS)
            {
                LOG_E("Framebuffer[%s]: invalid attachment count %d.", mName.c_str(), count);
                return false;
            }

            if (mFbo != 0 && width == mWidth && height == mHeight && count == mCount &&
                std::equal(atts, atts + count, mAttachments))
            {
                return true;
            }

            destroy();

            glGenFramebuffers(1, &mFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);

            for (int i = 0; i < count; ++i)
            {
                glGenTextures(1, &mTextures[i]);
                glBindTexture(GL_TEXTURE_2D, mTextures[i]);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(atts[i].internalFormat),
                             width, height, 0, atts[i].format, atts[i].type, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                       GL_TEXTURE_2D, mTextures[i], 0);
                mAttachments[i] = atts[i];
            }

            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                // The usual cause is a float internal format on a driver that
                // does not advertise it as colour-renderable (GLES 3.0 needs
                // EXT_color_buffer_half_float for RGBA16F).
                LOG_E("Framebuffer[%s] incomplete after resize to %dx%d (status=0x%x, %d attachment(s), fmt0=0x%x).",
                      mName.c_str(), width, height, status, count, atts[0].internalFormat);
                destroy();
                return false;
            }

            mWidth = width;
            mHeight = height;
            mCount = count;
            LOG_I("Framebuffer[%s] sized to %dx%d (id=%u, %d attachment(s), tex0=%u).",
                  mName.c_str(), mWidth, mHeight, mFbo, mCount, mTextures[0]);
            return true;
        }

        /// Activates this framebuffer for rendering and sets the GL viewport
        /// to match its dimensions. Caller is expected to clear if desired.
        void Bind() const
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
            glViewport(0, 0, mWidth, mHeight);
            if (mCount > 1)
            {
                const GLenum bufs[MAX_ATTACHMENTS] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                glDrawBuffers(mCount, bufs);
            }
        }

        /// Restores the default framebuffer. Does NOT touch the viewport - the
        /// caller is responsible for setting it back to the window size.
        static void BindDefault()
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        /// Binds colour attachment @p index to texture unit @p unit
        /// (defaults to GL_TEXTURE0) for sampling in a subsequent pass.
        void BindTexture(GLuint unit = 0, int index = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, GetTextureId(index));
        }

        bool IsValid() const { return mFbo != 0; }
        int GetWidth() const { return mWidth; }
        int GetHeight() const { return mHeight; }
        int GetAttachmentCount() const { return mCount; }
        GLuint GetId() const { return mFbo; }
        GLuint GetTextureId(int index = 0) const
        {
            return (index >= 0 && index < mCount) ? mTextures[index] : 0;
        }
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
            for (int i = 0; i < MAX_ATTACHMENTS; ++i)
            {
                if (mTextures[i] != 0)
                {
                    glDeleteTextures(1, &mTextures[i]);
                    mTextures[i] = 0;
                }
            }
            mWidth = 0;
            mHeight = 0;
            mCount = 0;
        }

    private:
        GLuint mFbo = 0;
        GLuint mTextures[MAX_ATTACHMENTS] = {0, 0};
        Attachment mAttachments[MAX_ATTACHMENTS];
        int mCount = 0;
        int mWidth = 0;
        int mHeight = 0;
        std::string mName = "unnamed";
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FRAMEBUFFER_H_
