#ifndef _EDGE_LIGHTING_TEXTURE_1D_H_
#define _EDGE_LIGHTING_TEXTURE_1D_H_

#include "gl/texture.h"

namespace EdgeLighting
{
    class Texture1D : public Texture
    {
    public:
        Texture1D() = default;

        void Bind(int unit = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_1D, mId);
        }

        void Unbind(int unit = 0) const
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_1D, 0);
        }

        void SetData(const float *data, GLsizei count, GLint internalFormat = GL_RG32F,
                     GLenum format = GL_RG) const
        {
            Bind();
            glTexImage1D(GL_TEXTURE_1D, 0, internalFormat, count, 0, format, GL_FLOAT, data);
        }

        void SetParams(GLint minFilter = GL_NEAREST, GLint magFilter = GL_NEAREST,
                       GLint wrapS = GL_CLAMP_TO_EDGE) const
        {
            Bind();
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, minFilter);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, magFilter);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, wrapS);
        }
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_TEXTURE_1D_H_
