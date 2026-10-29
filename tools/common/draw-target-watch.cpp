#include "draw-target-watch.h"

#include <algorithm>
#include <vector>

namespace NeonTools
{
    namespace
    {
        /// Colour attachments looked at per draw: the library's most is two
        /// (the gather buffer with segments); Framebuffer allows three.
        const int WATCHED_ATTACHMENTS = 4;

        /// Every byte of a texture allocated with no data: 60/255 in an 8-bit
        /// channel, 1.06 in a half-float one, 0.0115 in a float one - finite,
        /// and nothing a cleared or drawn texel holds by accident.
        const unsigned char FRESH_TEXEL_BYTE = 0x3C;

        PFNGLDRAWARRAYSPROC gNextDrawArrays = nullptr;
        PFNGLGENTEXTURESPROC gNextGenTextures = nullptr;
        PFNGLTEXIMAGE2DPROC gNextTexImage2D = nullptr;
        GLuint gHighestTexture = 0;
        std::vector<unsigned char> gFreshTexels;
        bool gWatching = false;
        GLint gCallerFramebuffer = 0;
        std::map<GLint, long> gFormats;

        /// The sized internal format of colour attachment @p attachment of the
        /// bound draw framebuffer, read from the attachment's own parameters -
        /// component type, bits per channel, colour encoding - so nothing is
        /// bound and any attachment answers: a texture of any target, a layer,
        /// a renderbuffer. 0 for a combination not listed here.
        GLint AttachmentFormat(GLenum attachment)
        {
            GLint componentType = GL_NONE;
            GLint encoding = GL_LINEAR;
            GLint r = 0;
            GLint g = 0;
            GLint b = 0;
            GLint a = 0;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
                                                  &r);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,
                                                  &b);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &a);
            // Channels present, each the same width as red.
            const int channels = 1 + (g == r) + (b == r) + (a == r);
            const bool contiguous = (g == r || g == 0) && (b == r || b == 0) && (a == r || a == 0) &&
                                    !(g == 0 && (b != 0 || a != 0)) && !(b == 0 && a != 0);
            if (!contiguous)
            {
                return 0;
            }
            if (componentType == GL_FLOAT && r == 16)
            {
                const GLint formats[] = {GL_R16F, GL_RG16F, GL_RGB16F, GL_RGBA16F};
                return formats[channels - 1];
            }
            if (componentType == GL_FLOAT && r == 32)
            {
                const GLint formats[] = {GL_R32F, GL_RG32F, GL_RGB32F, GL_RGBA32F};
                return formats[channels - 1];
            }
            if (componentType == GL_UNSIGNED_NORMALIZED && r == 8)
            {
                if (encoding == GL_SRGB)
                {
                    return channels == 4 ? GL_SRGB8_ALPHA8 : 0;
                }
                const GLint formats[] = {GL_R8, GL_RG8, GL_RGB8, GL_RGBA8};
                return formats[channels - 1];
            }
            return 0;
        }

        /// Record what this draw is about to write, then make it. Only
        /// queries: the draw sees exactly the state it was given.
        void APIENTRY HookDrawArrays(GLenum mode, GLint first, GLsizei count)
        {
            if (gWatching)
            {
                GLint framebuffer = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
                if (framebuffer != 0 && framebuffer != gCallerFramebuffer)
                {
                    for (int i = 0; i < WATCHED_ATTACHMENTS; ++i)
                    {
                        const GLenum attachment = GLenum(GL_COLOR_ATTACHMENT0 + i);
                        GLint type = GL_NONE;
                        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
                        if (type == GL_NONE)
                        {
                            continue;
                        }
                        ++gFormats[AttachmentFormat(attachment)];
                    }
                }
            }
            gNextDrawArrays(mode, first, count);
        }

        /// Make the names, then remember the highest.
        void APIENTRY HookGenTextures(GLsizei n, GLuint *textures)
        {
            gNextGenTextures(n, textures);
            for (GLsizei i = 0; i < n; ++i)
            {
                gHighestTexture = std::max(gHighestTexture, textures[i]);
            }
        }

        /// Bytes per texel of a client upload in @p format and @p type; 0 for
        /// a pair not listed - the library uploads no other.
        size_t UploadTexelBytes(GLenum format, GLenum type)
        {
            size_t channels = 0;
            switch (format)
            {
            case GL_RED:
            {
                channels = 1;
                break;
            }
            case GL_RG:
            {
                channels = 2;
                break;
            }
            case GL_RGB:
            {
                channels = 3;
                break;
            }
            case GL_RGBA:
            {
                channels = 4;
                break;
            }
            default:
            {
                return 0;
            }
            }
            switch (type)
            {
            case GL_UNSIGNED_BYTE:
            {
                return channels;
            }
            case GL_HALF_FLOAT:
            {
                return channels * 2;
            }
            case GL_FLOAT:
            {
                return channels * 4;
            }
            default:
            {
                return 0;
            }
            }
        }

        /// An allocation with no data - every Framebuffer::Resize, every
        /// format probe - is uploaded FRESH_TEXEL_BYTE throughout instead; any
        /// other upload goes through untouched. Only reads state, so the
        /// upload raises exactly what it would have.
        void APIENTRY HookTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                                     GLint border, GLenum format, GLenum type, const void *pixels)
        {
            const size_t texelBytes = UploadTexelBytes(format, type);
            GLint unpackBuffer = 0;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
            if (pixels != nullptr || unpackBuffer != 0 || texelBytes == 0 || width <= 0 || height <= 0)
            {
                gNextTexImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
                return;
            }
            GLint alignment = 4;
            GLint rowLength = 0;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            const size_t rowTexels = size_t(rowLength > 0 ? rowLength : width);
            const size_t step = size_t(std::max(alignment, 1));
            const size_t rowBytes = (rowTexels * texelBytes + step - 1) / step * step;
            const size_t bytes = rowBytes * size_t(height);
            if (gFreshTexels.size() < bytes)
            {
                gFreshTexels.assign(bytes, FRESH_TEXEL_BYTE);
            }
            gNextTexImage2D(target, level, internalFormat, width, height, border, format, type, gFreshTexels.data());
        }
    }

    void DrawTargetWatch::Install()
    {
        if (gNextDrawArrays == nullptr)
        {
            gNextDrawArrays = glad_glDrawArrays;
            glad_glDrawArrays = HookDrawArrays;
        }
        if (gNextGenTextures == nullptr)
        {
            gNextGenTextures = glad_glGenTextures;
            glad_glGenTextures = HookGenTextures;
        }
        InstallFreshTextureFill();
    }

    void DrawTargetWatch::InstallFreshTextureFill()
    {
        if (gNextTexImage2D == nullptr)
        {
            gNextTexImage2D = glad_glTexImage2D;
            glad_glTexImage2D = HookTexImage2D;
        }
    }

    GLuint DrawTargetWatch::GetHighestTextureName()
    {
        return gHighestTexture;
    }

    void DrawTargetWatch::Begin()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &gCallerFramebuffer);
        gWatching = true;
    }

    void DrawTargetWatch::End()
    {
        gWatching = false;
    }

    void DrawTargetWatch::Reset()
    {
        gFormats.clear();
    }

    const std::map<GLint, long> &DrawTargetWatch::GetFormats()
    {
        return gFormats;
    }
}
