#ifndef _EDGE_LIGHTING_TEXTURE_2D_H_
#define _EDGE_LIGHTING_TEXTURE_2D_H_

#include "gl/texture.h"
#include "util/stb-image.h"

namespace EdgeLighting
{
    class Texture2D : public Texture
    {
    public:
        Texture2D() = default;

        void Bind(int unit = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, mId);
        }

        void Unbind(int unit = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        void SetData(const void *data, GLsizei width, GLsizei height,
                     GLint internalFormat = GL_RGBA8, GLenum format = GL_RGBA,
                     GLenum type = GL_UNSIGNED_BYTE) const
        {
            Bind();
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, data);
        }

        /// Replace the pixels of an ALREADY-ALLOCATED image of the same size.
        ///
        /// @ref SetData re-specifies the texture: the driver frees the old
        /// storage and allocates new storage on every call. That is the right
        /// call when the dimensions or the format move, and the wrong one for
        /// a stream of same-sized updates - a cross-fading @c GradientRingLUT
        /// re-uploads its ring every frame, and re-allocating a texture per
        /// frame to write the same number of bytes is pure overhead.
        ///
        /// @pre @ref SetData has run at these exact dimensions; there is no
        ///      allocation here, so a mismatched size is undefined behaviour
        ///      in GL rather than a resize. @c BaseLUT tracks the dimensions
        ///      and picks between the two.
        void SetSubData(const void *data, GLsizei width, GLsizei height,
                        GLenum format = GL_RGBA, GLenum type = GL_UNSIGNED_BYTE) const
        {
            Bind();
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
        }

        void SetParams(GLint minFilter = GL_LINEAR, GLint magFilter = GL_LINEAR,
                       GLint wrapS = GL_CLAMP_TO_EDGE, GLint wrapT = GL_CLAMP_TO_EDGE) const
        {
            Bind();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
        }

        bool SetDataFromFile(const char *path, GLint internalFormat = GL_RGBA8)
        {
            int w, h, n;
            unsigned char *pixels = stbi_load(path, &w, &h, &n, 4);
            if (!pixels)
            {
                return false;
            }

            SetData(pixels, w, h, internalFormat, GL_RGBA, GL_UNSIGNED_BYTE);
            stbi_image_free(pixels);
            return true;
        }
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_TEXTURE_2D_H_
