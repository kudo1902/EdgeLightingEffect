#ifndef _EDGE_LIGHTING_CAPTURE_UTIL_H_
#define _EDGE_LIGHTING_CAPTURE_UTIL_H_

#include <string>
#include <vector>

#include "gl/gl-header.h"
#include "gl/framebuffer.h"

namespace EdgeLighting
{
    /// Deterministic pixel readback helpers.
    ///
    /// These deliberately never touch the window's default framebuffer. Reading
    /// the window back buffer is not portable: its contents are undefined after
    /// a buffer swap, its size follows the platform's HiDPI backing scale, it
    /// may be multisampled or sRGB-encoded, and it has already been composited
    /// by the window system. Capturing an application-owned RGBA8 framebuffer
    /// instead gives the same bytes on every platform.
    ///
    /// Typical use is @ref OffscreenCapture, which renders one frame at an
    /// explicit size and reads it back:
    /// @code
    ///     if (capture.Begin(1280, 720))
    ///     {
    ///         drawScene(1280, 720);
    ///         capture.Save("frame.png");
    ///         capture.End();
    ///     }
    /// @endcode
    namespace CaptureUtil
    {
        /// A CPU-side pixel buffer, tightly packed (no row padding), rows in
        /// top-left origin order - i.e. already flipped out of GL's bottom-left
        /// convention, so it can go straight to a PNG writer.
        typedef struct Image
        {
            int width = 0;
            int height = 0;
            int channels = 0; ///< Always 4 (RGBA8) for buffers produced here.
            std::vector<unsigned char> pixels;

            bool IsValid() const
            {
                return width > 0 && height > 0 && channels > 0 &&
                       pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
            }
        } Image;

        /// Reads an RGBA8 region out of the framebuffer currently bound to
        /// GL_READ_FRAMEBUFFER. Sets and restores GL_PACK_ALIGNMENT, and flips
        /// the rows so @p out is top-left origin.
        ///
        /// The read framebuffer must not be multisampled - resolve it with a
        /// @c glBlitFramebuffer into a single-sample target first.
        /// @return @c true on success; logs and returns @c false otherwise.
        bool ReadRegion(int x, int y, int width, int height, Image &out);

        /// Reads the whole colour attachment of @p fb. Binds it as the read
        /// framebuffer and restores the previous read binding afterwards.
        bool ReadFramebuffer(const Framebuffer &fb, Image &out);

        /// Reads a 2D texture back by attaching it to a scratch framebuffer.
        /// Works on both desktop GL and GLES (no @c glGetTexImage), which is
        /// why the LUT dumps go through here. @p texture must be colour
        /// renderable - the RGBA8 LUTs baked by the neon renderers are.
        bool ReadTexture2D(GLuint texture, int width, int height, Image &out);

        /// Writes @p image as a PNG. When @p dropAlpha is true the alpha
        /// channel is discarded and an RGB PNG is written instead.
        /// @return @c true when the file was written.
        bool WritePNG(const std::string &filepath, const Image &image, bool dropAlpha = false);

        /// Builds "@p dir /@p prefix YYYYMMDD_HHMMSS.@p ext" from local time.
        std::string TimestampedPath(const std::string &dir, const std::string &prefix, const std::string &ext);
    } // namespace CaptureUtil

    /// Owns an offscreen RGBA8 framebuffer sized independently of any window,
    /// so a captured frame is reproducible across platforms and DPI settings.
    ///
    /// @ref Begin saves the current draw framebuffer binding and viewport and
    /// switches to the capture target; @ref End restores both. Rendering
    /// between the two calls must use the size passed to @ref Begin.
    class OffscreenCapture
    {
    public:
        OffscreenCapture()
            : mCaptureBuffer("Capture.Offscreen")
        {
        }

        OffscreenCapture(const OffscreenCapture &) = delete;
        OffscreenCapture &operator=(const OffscreenCapture &) = delete;

        /// Allocates (or reuses) the capture target at @p width × @p height,
        /// binds it, and sets the viewport to match. Does NOT clear - the
        /// caller's scene draw is expected to.
        /// @return @c false if the size is invalid or the FBO is incomplete,
        ///         in which case no state was changed and @ref End must not be
        ///         called.
        bool Begin(int width, int height);

        /// Restores the framebuffer binding and viewport saved by @ref Begin.
        /// Safe to call when @ref Begin failed or was never called (no-op).
        void End();

        /// Reads the capture target back into @p out. Valid after the scene has
        /// been drawn, whether or not @ref End has run.
        bool Read(CaptureUtil::Image &out) const;

        /// @ref Read followed by @ref CaptureUtil::WritePNG.
        bool Save(const std::string &filepath, bool dropAlpha = false) const;

        const Framebuffer &GetBuffer() const { return mCaptureBuffer; }
        int GetWidth() const { return mCaptureBuffer.GetWidth(); }
        int GetHeight() const { return mCaptureBuffer.GetHeight(); }

    private:
        Framebuffer mCaptureBuffer;
        GLint mPrevFramebuffer = 0;
        GLint mPrevViewport[4] = {0, 0, 0, 0};
        bool mActive = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CAPTURE_UTIL_H_
