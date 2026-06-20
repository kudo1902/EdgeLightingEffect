#ifndef _EDGE_LIGHTING_TEXTURE_H_
#define _EDGE_LIGHTING_TEXTURE_H_

#include "gl/gl-header.h"

namespace EdgeLighting
{
    class Texture
    {
    public:
        Texture() { glGenTextures(1, &mId); }

        virtual ~Texture()
        {
            if (mId != 0)
            {
                glDeleteTextures(1, &mId);
            }
        }

        Texture(const Texture &) = delete;
        Texture &operator=(const Texture &) = delete;

        Texture(Texture &&other) noexcept
            : mId(other.mId)
        {
            other.mId = 0;
        }

        Texture &operator=(Texture &&other) noexcept
        {
            if (this != &other)
            {
                if (mId != 0)
                {
                    glDeleteTextures(1, &mId);
                }
                mId = other.mId;
                other.mId = 0;
            }
            return *this;
        }

        GLuint GetId() const { return mId; }
        bool IsValid() const { return mId != 0; }

    protected:
        GLuint mId = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_TEXTURE_H_
