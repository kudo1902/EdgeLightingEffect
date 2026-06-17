#ifndef _SCREENSHOT_UTIL_H_
#define _SCREENSHOT_UTIL_H_

#include <string>
#include <vector>
#include <ctime>

#include "gl/gl-header.h"
#include "stb/stb_image_write.h"

namespace EdgeLighting
{
    namespace ScreenshotUtil
    {
        inline void SaveScreenshot(const std::string &filepath, int w, int h)
        {
            std::vector<unsigned char> pixels(w * h * 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            std::vector<unsigned char> rgb(w * h * 3);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    int src = ((h - 1 - y) * w + x) * 4;
                    int dst = (y * w + x) * 3;
                    rgb[dst + 0] = pixels[src + 0];
                    rgb[dst + 1] = pixels[src + 1];
                    rgb[dst + 2] = pixels[src + 2];
                }
            }

            stbi_write_png(filepath.c_str(), w, h, 3, rgb.data(), w * 3);
        }

        inline std::string TimestampedPath(const std::string &dir, const std::string &prefix, const std::string &ext)
        {
            std::time_t t = std::time(nullptr);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
            return dir + "/" + prefix + buf + "." + ext;
        }
    }
}

#endif
