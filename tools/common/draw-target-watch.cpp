#include "draw-target-watch.h"

#include <algorithm>

namespace NeonTools
{
    namespace
    {
        /// Colour attachments looked at per draw: the library's most is two
        /// (the gather buffer with segments); Framebuffer allows three.
        const int WATCHED_ATTACHMENTS = 4;

        PFNGLDRAWARRAYSPROC gNextDrawArrays = nullptr;
        PFNGLGENTEXTURESPROC gNextGenTextures = nullptr;
        GLuint gHighestTexture = 0;
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
