#include "util/capture-util.h"

#include "util/log-util.h"
#include "stb/stb_image_write.h"

#include <algorithm>
#include <cstddef>
#include <ctime>

namespace EdgeLighting
{
    namespace CaptureUtil
    {
        namespace
        {
            /// Copies @p src (bottom-left origin, tightly packed RGBA8) into
            /// @p dst flipped to top-left origin.
            void flipRows(const std::vector<unsigned char> &src,
                          std::vector<unsigned char> &dst,
                          int width, int height, int channels)
            {
                const size_t stride = static_cast<size_t>(width) * static_cast<size_t>(channels);
                for (int y = 0; y < height; ++y)
                {
                    const size_t srcOffset = static_cast<size_t>(height - 1 - y) * stride;
                    const size_t dstOffset = static_cast<size_t>(y) * stride;
                    std::copy(src.begin() + static_cast<std::ptrdiff_t>(srcOffset),
                              src.begin() + static_cast<std::ptrdiff_t>(srcOffset + stride),
                              dst.begin() + static_cast<std::ptrdiff_t>(dstOffset));
                }
            }
        } // namespace

        bool ReadRegion(int x, int y, int width, int height, Image &out)
        {
            if (width <= 0 || height <= 0)
            {
                LOG_E("CaptureUtil::ReadRegion: invalid size %dx%d.", width, height);
                return false;
            }

            // glReadPixels cannot read a multisampled attachment; catch that
            // here rather than letting it fail with an opaque GL error.
            GLint samples = 0;
            glGetIntegerv(GL_SAMPLES, &samples);
            if (samples > 1)
            {
                LOG_E("CaptureUtil::ReadRegion: read framebuffer is multisampled (%d samples); "
                      "blit it into a single-sample framebuffer first.",
                      samples);
                return false;
            }

            // glGetError is sticky: without draining first, an error raised
            // anywhere earlier in the frame would be blamed on glReadPixels
            // and abort a capture that actually succeeded.
            GLenum pending = GL_NO_ERROR;
            GLenum stale = GL_NO_ERROR;
            while ((pending = glGetError()) != GL_NO_ERROR)
            {
                stale = pending;
            }
            if (stale != GL_NO_ERROR)
            {
                LOG_W("CaptureUtil::ReadRegion: cleared a pending GL error (0x%x) raised before the readback.", stale);
            }

            const int channels = 4;
            std::vector<unsigned char> raw(static_cast<size_t>(width) *
                                           static_cast<size_t>(height) *
                                           static_cast<size_t>(channels));

            GLint prevAlignment = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);

            glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

            glPixelStorei(GL_PACK_ALIGNMENT, prevAlignment);

            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LOG_E("CaptureUtil::ReadRegion: glReadPixels failed (0x%x).", err);
                return false;
            }

            out.width = width;
            out.height = height;
            out.channels = channels;
            out.pixels.assign(raw.size(), 0);
            flipRows(raw, out.pixels, width, height, channels);
            return true;
        }

        bool ReadFramebuffer(const Framebuffer &fb, Image &out)
        {
            if (!fb.IsValid())
            {
                LOG_E("CaptureUtil::ReadFramebuffer: framebuffer [%s] is not allocated.", fb.GetName());
                return false;
            }

            GLint prevRead = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.GetId());
            bool ok = ReadRegion(0, 0, fb.GetWidth(), fb.GetHeight(), out);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));

            return ok;
        }

        bool ReadTexture2D(GLuint texture, int width, int height, Image &out)
        {
            if (texture == 0 || width <= 0 || height <= 0)
            {
                LOG_E("CaptureUtil::ReadTexture2D: invalid texture %u or size %dx%d.", texture, width, height);
                return false;
            }

            // The Framebuffer wrapper always allocates its own colour
            // attachment, so it cannot host a texture owned by someone else.
            // This scratch FBO is created and destroyed inline instead; it is
            // the one place in the library that needs raw framebuffer calls.
            GLint prevRead = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);

            GLuint scratch = 0;
            glGenFramebuffers(1, &scratch);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

            bool ok = false;
            GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                LOG_E("CaptureUtil::ReadTexture2D: scratch framebuffer incomplete for texture %u "
                      "(status=0x%x); is it colour renderable?",
                      texture, status);
            }
            else
            {
                ok = ReadRegion(0, 0, width, height, out);
            }

            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
            glDeleteFramebuffers(1, &scratch);

            return ok;
        }

        bool WritePNG(const std::string &filepath, const Image &image, bool dropAlpha)
        {
            if (!image.IsValid())
            {
                LOG_E("CaptureUtil::WritePNG: refusing to write malformed image to %s.", filepath.c_str());
                return false;
            }

            const int outChannels = (dropAlpha && image.channels == 4) ? 3 : image.channels;

            int written = 0;
            if (outChannels == image.channels)
            {
                written = stbi_write_png(filepath.c_str(), image.width, image.height,
                                         image.channels, image.pixels.data(),
                                         image.width * image.channels);
            }
            else
            {
                std::vector<unsigned char> rgb(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3);
                const size_t pixelCount = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    rgb[i * 3 + 0] = image.pixels[i * 4 + 0];
                    rgb[i * 3 + 1] = image.pixels[i * 4 + 1];
                    rgb[i * 3 + 2] = image.pixels[i * 4 + 2];
                }
                written = stbi_write_png(filepath.c_str(), image.width, image.height,
                                         3, rgb.data(), image.width * 3);
            }

            if (written == 0)
            {
                LOG_E("CaptureUtil::WritePNG: failed to write %s.", filepath.c_str());
                return false;
            }
            return true;
        }

        std::string TimestampedPath(const std::string &dir, const std::string &prefix, const std::string &ext)
        {
            std::time_t t = std::time(nullptr);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
            return dir + "/" + prefix + buf + "." + ext;
        }
    } // namespace CaptureUtil

    bool OffscreenCapture::Begin(int width, int height)
    {
        if (mActive)
        {
            LOG_W("OffscreenCapture::Begin called while already active; ending the previous capture.");
            End();
        }

        if (!mCaptureBuffer.Resize(width, height))
        {
            return false;
        }

        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &mPrevFramebuffer);
        glGetIntegerv(GL_VIEWPORT, mPrevViewport);

        mCaptureBuffer.Bind();
        mActive = true;
        return true;
    }

    void OffscreenCapture::End()
    {
        if (!mActive)
        {
            return;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(mPrevFramebuffer));
        glViewport(mPrevViewport[0], mPrevViewport[1], mPrevViewport[2], mPrevViewport[3]);
        mActive = false;
    }

    bool OffscreenCapture::Read(CaptureUtil::Image &out) const
    {
        return CaptureUtil::ReadFramebuffer(mCaptureBuffer, out);
    }

    bool OffscreenCapture::Save(const std::string &filepath, bool dropAlpha) const
    {
        CaptureUtil::Image image;
        if (!Read(image))
        {
            return false;
        }
        return CaptureUtil::WritePNG(filepath, image, dropAlpha);
    }

} // namespace EdgeLighting
