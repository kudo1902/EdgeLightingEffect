#ifndef _EDGE_LIGHTING_BASE_LUT_H_
#define _EDGE_LIGHTING_BASE_LUT_H_

#include "gl/texture-2d.h"

namespace EdgeLighting
{

    /// Common ground for the baked colour LUTs: the texture handle, the
    /// read-only surface their callers get, and the one place the upload
    /// format policy lives.
    ///
    /// Three invariants are the reason this class exists rather than each LUT
    /// holding its own @ref Texture2D:
    ///
    ///   - **RGBA8, never float.** Edge devices often lack float-texture
    ///     support, so every LUT here bakes on the CPU and quantises through
    ///     @c ColorUtils::ToByte. There is nothing to assert: @ref Upload is
    ///     the single call site and it passes the format literally, so a LUT
    ///     cannot ask for anything else.
    ///   - **A LUT's texture is a derived value.** @c mTexture is private, so
    ///     not even a subclass can reach @c Texture2D::SetData directly - the
    ///     only way to write the texture is @ref Upload, and the only thing a
    ///     caller can do with it is @ref Bind. Whatever the last bake produced
    ///     is what is on the GPU.
    ///   - **"Has a texture name" is not "has an image".** @c Texture's
    ///     constructor calls @c glGenTextures, so the name exists from the
    ///     moment a LUT is constructed, long before anything is baked into it.
    ///     @ref IsValid answers the question callers actually mean - is there
    ///     something here worth sampling - by tracking @ref Upload. See the
    ///     note on that method.
    ///
    /// Deliberately NOT polymorphic: nothing dispatches on a LUT and nothing
    /// stores one as a @c BaseLUT*. The destructor is protected rather than
    /// virtual, which turns deleting through a base pointer into a compile
    /// error instead of a lifetime bug.
    ///
    /// That buys less than it looks like, and the reason is worth knowing
    /// before anyone "optimises" further: @c Texture (the member's base) has a
    /// virtual destructor, so @c Texture2D carries a vptr and every LUT pays
    /// for one - @c sizeof(Texture2D) is 16 for a 4-byte handle. Nothing in
    /// the tree deletes through a @c Texture*, so that virtual is currently
    /// unearned, but removing it is a change to a shared GL wrapper used well
    /// outside the LUTs and is not this class's call to make.
    class BaseLUT
    {
    public:
        /// Bind for sampling. The only thing a caller may do with the texture.
        ///
        /// @note Binds nothing useful before the first @ref Upload - check
        ///       @ref IsValid if the caller cannot otherwise know a bake has
        ///       happened.
        void Bind(int unit = 0) const { mTexture.Bind(unit); }

        /// Whether this LUT holds an image worth sampling: a live texture name
        /// AND at least one completed @ref Upload.
        ///
        /// The second half is the point. A freshly constructed LUT already has
        /// a texture name (see the class note), so a plain
        /// @c Texture2D::IsValid would report true while the texture has no
        /// image at all - which in core profile samples as undefined data
        /// rather than failing loudly.
        bool IsValid() const { return mTexture.IsValid() && mUploaded; }

        /// The raw texture name, for reading the baked image back through
        /// @c CaptureUtil::ReadTexture2D - the LUT dumps that helper exists
        /// for. Not for binding (use @ref Bind) and not for writing: the
        /// texture is a derived value, see the class note.
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

        /// Whether @ref Upload has ever run. Subclasses gate their first-bake
        /// behaviour on this rather than each keeping a private copy: a
        /// GradientRingLUT with nothing on screen yet must snap instead of
        /// cross-fading, and a SpanAtlasLUT with no texture yet is dirty by
        /// definition. Both are the same event, so it lives in one place.
        bool HasUploaded() const { return mUploaded; }

        /// Upload one baked RGBA8 image and set the sampling policy.
        ///
        /// LINEAR both ways: every LUT is sampled at arbitrary positions
        /// between texels, and the interpolation IS the gradient. V is always
        /// CLAMP - a ring is a single row, and an atlas's rows must not bleed
        /// into each other.
        ///
        /// Leaves the texture-unit state exactly as it found it. Uploading
        /// means binding, and @c Texture2D::Bind activates unit 0 - so without
        /// the save/restore below a bake would silently steal unit 0's binding
        /// and leave unit 0 active. Today's bakes all run from
        /// @c OnConfigChanged or @c Update, never between a pass's texture
        /// binds and its draw, so nothing would observe it; but the whole point
        /// of hiding the upload behind this class is that a caller should not
        /// have to know that. Two state queries on a path that only runs when a
        /// bake is actually dirty is the same trade @c Framebuffer's clear-colour
        /// save/restore already makes on a per-frame path.
        ///
        /// @param data   @p width * @p height * 4 bytes, RGBA8.
        /// @param wrapS  the axis that actually differs between LUTs:
        ///               @c GL_REPEAT for a cyclic ring, @c GL_CLAMP_TO_EDGE
        ///               for spans laid head-to-tail.
        void Upload(const unsigned char *data, int width, int height, GLint wrapS)
        {
            GLint prevUnit = GL_TEXTURE0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
            // Read the binding on unit 0 specifically - that is the one
            // Texture2D::Bind() is about to overwrite.
            glActiveTexture(GL_TEXTURE0);
            GLint prevTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);

            // Allocate only when there is nothing to write into, or the shape
            // changed; otherwise write over what is there.
            //
            // Both do the same job at the same cost in bytes, but SetData
            // re-specifies - the driver frees and reallocates the texture's
            // storage every call. That is invisible on a bake, which happens
            // when the colours change, and not on a fade: GradientRingLUT::Tick
            // re-uploads a same-sized ring EVERY FRAME for the length of the
            // cross-fade, so this was a texture reallocation per frame per
            // ring. Sampler state is per-texture-object and survives a
            // sub-upload, so it only has to be re-sent when the object is
            // re-specified or the wrap mode actually moves.
            const bool respecify = !mUploaded || width != mTextureWidth || height != mTextureHeight;
            if (respecify)
            {
                mTexture.SetData(data, width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
                mTextureWidth = width;
                mTextureHeight = height;
            }
            else
            {
                mTexture.SetSubData(data, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
            }
            if (respecify || wrapS != mTextureWrapS)
            {
                mTexture.SetParams(GL_LINEAR, GL_LINEAR, wrapS, GL_CLAMP_TO_EDGE);
                mTextureWrapS = wrapS;
            }
            mUploaded = true;

            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
            glActiveTexture(static_cast<GLenum>(prevUnit));
        }

    private:
        Texture2D mTexture;
        /// False until the first @ref Upload puts an image in @c mTexture. The
        /// texture NAME exists from construction, so this is the only thing
        /// that distinguishes an empty LUT from a baked one.
        bool mUploaded = false;
        /// The shape and sampler state currently in @c mTexture, so @ref Upload
        /// can tell a same-sized rewrite from a reallocation. Only meaningful
        /// once @c mUploaded is set.
        int mTextureWidth = 0;
        int mTextureHeight = 0;
        GLint mTextureWrapS = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_LUT_H_
