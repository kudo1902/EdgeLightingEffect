// neon-guide-figures - renders every image in docs/neon-onboarding-guide.md.
//
//   neon-guide-figures [OUTDIR]
//
// OUTDIR defaults to this checkout's docs/images/neon-onboarding. Every figure
// is rendered by the library itself, offscreen, from a fixed config with time
// frozen, so a rerun on the same GPU writes the same bytes. See README.md.

#include "figures.h"
#include "pass-recorder.h"

#include "gl/gl-header.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <streambuf>
#include <string>

using namespace NeonGuideFigures;
using NeonTools::PassRecorder;

namespace
{
    /// The library logs every line, INFO included, to std::cout. Pass only its
    /// WARN and ERROR lines on, to stderr: a shader that fails to compile must
    /// still be seen, and the list of files written must not be buried.
    class LogFilter : public std::streambuf
    {
    protected:
        int overflow(int c) override
        {
            if (c == traits_type::eof())
            {
                return traits_type::not_eof(c);
            }
            if (c != '\n')
            {
                mLine.push_back(char(c));
                return c;
            }
            if (mLine.find("][WARN]") != std::string::npos || mLine.find("][ERROR]") != std::string::npos)
            {
                std::fprintf(stderr, "%s\n", mLine.c_str());
            }
            mLine.clear();
            return c;
        }

    private:
        std::string mLine;
    };

    /// A hidden window's GL 3.3 core context for the life of a scope. Declared
    /// before anything that owns GL objects, so it outlives their destructors.
    class GLSession
    {
    public:
        GLSession()
        {
            if (!glfwInit())
            {
                std::fprintf(stderr, "neon-guide-figures: glfwInit failed\n");
                std::exit(2);
            }
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
            mWindow = glfwCreateWindow(64, 64, "neon-guide-figures", nullptr, nullptr);
            if (!mWindow)
            {
                std::fprintf(stderr, "neon-guide-figures: could not create a GL 3.3 core context\n");
                std::exit(2);
            }
            glfwMakeContextCurrent(mWindow);
            if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
            {
                std::fprintf(stderr, "neon-guide-figures: GLAD could not load GL\n");
                std::exit(2);
            }
            std::fprintf(stderr, "GL renderer: %s\n", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        }

        ~GLSession()
        {
            PassRecorder::Release();
            glfwTerminate();
        }

        GLSession(const GLSession &) = delete;
        GLSession &operator=(const GLSession &) = delete;

    private:
        GLFWwindow *mWindow = nullptr;
    };
}

int main(int argc, char **argv)
{
    static LogFilter logFilter;
    std::cout.rdbuf(&logFilter);

    const std::string dir = argc >= 2 ? std::string(argv[1]) : std::string(DOCS_DIR) + "/images/neon-onboarding";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error)
    {
        std::fprintf(stderr, "neon-guide-figures: cannot create %s: %s\n", dir.c_str(), error.message().c_str());
        return 1;
    }

    GLSession session;
    PassRecorder::Install();

    {
        EdgeLightingEffect effect;
        effect.Initialize();
        if (!effect.AddRenderer(RendererLayer::NEON) || !effect.AddRenderer(RendererLayer::DEBUG))
        {
            std::fprintf(stderr, "neon-guide-figures: a renderer failed to initialise\n");
            return 2;
        }
        WriteConceptFigures(dir);
        WriteModelFigures(effect, dir);
        WriteConfigFigures(effect, dir);
    }
    WritePassFigures(dir);
    return 0;
}
