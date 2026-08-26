#ifndef _EDGE_LIGHTING_BASE_LUT_H_
#define _EDGE_LIGHTING_BASE_LUT_H_

#include "gl/texture-2d.h"

namespace EdgeLighting
{

    /// Common ground for the baked colour LUTs: the texture handle, the
    /// read-only surface their callers get, and the one place the upload
    /// format policy lives.
    ///
    /// Two invariants are the reason this class exists rather than each LUT
    /// holding its own @ref Texture2D:
    ///
    ///   - **RGBA8, never float.** Edge devices often lack float-texture
    ///     support, so every LUT here bakes on the CPU and quantises through
    ///     @c ColorUtils::ToByte. Asserted once, in @ref Upload, instead of
    ///     once per LUT where the two copies could drift apart.
    ///   - **A LUT's texture is a derived value.** @c mTexture is private, so
    ///     not even a subclass can reach @c Texture2D::SetData directly - the
    ///     only way to write the texture is @ref Upload, and the only thing a
    ///     caller can do with it is @ref Bind. Whatever the last bake produced
    ///     is what is on the GPU.
    ///
    /// Deliberately NOT polymorphic. Nothing dispatches on a LUT and nothing
    /// stores one as a @c BaseLUT*, so there are no virtuals to pay for. The
    /// destructor is protected rather than virtual, which turns deleting
    /// through a base pointer into a compile error instead of a lifetime bug.
    class BaseLUT
    {
    public:
        /// Bind for sampling. The only thing a caller may do with the texture.
        void Bind(int unit = 0) const { mTexture.Bind(unit); }
        bool IsValid() const { return mTexture.IsValid(); }
        GLuint GetId() const { return mTexture.GetId(); }

    protected:
        BaseLUT() = default;
        /// Protected + non-virtual: subclasses destruct normally, polymorphic
        /// deletion does not compile.
        ~BaseLUT() = default;

        // Declaring the destructor above suppresses the implicit move
        // operations, and @c Texture2D is move-only - without these the LUTs
        // would silently become non-movable.
        BaseLUT(BaseLUT &&) noexcept = default;
        BaseLUT &operator=(BaseLUT &&) noexcept = default;

        /// Upload one baked RGBA8 image and set the sampling policy.
        ///
        /// LINEAR both ways: every LUT is sampled at arbitrary positions
        /// between texels, and the interpolation IS the gradient. V is always
        /// CLAMP - a ring is a single row, and an atlas's rows must not bleed
        /// into each other.
        ///
        /// @param data   @p width * @p height * 4 bytes, RGBA8.
        /// @param wrapS  the axis that actually differs between LUTs:
        ///               @c GL_REPEAT for a cyclic ring, @c GL_CLAMP_TO_EDGE
        ///               for spans laid head-to-tail.
        void Upload(const unsigned char *data, int width, int height, GLint wrapS)
        {
            mTexture.SetData(data, width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            mTexture.SetParams(GL_LINEAR, GL_LINEAR, wrapS, GL_CLAMP_TO_EDGE);
        }

    private:
        Texture2D mTexture;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_LUT_H_
