#include "gl/gl-header.h"
#include <GLFW/glfw3.h>
#include "core/edge-lighting.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/neon-renderer.h"
#include "renderer/neon-multi-pass-renderer.h"
#include "renderer/neon-optimized-renderer.h"
#include "animation/neon-animations.h"
#include "debug-ui.h"
#include "background-quad.h"
#include "ui-controls.h"
#include "util/log-util.h"
#include <memory>

std::unique_ptr<EdgeLighting::EdgeLightingEffect> gEffect;

void OnResize(GLFWwindow *window, int width, int height);
void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods);

int main()
{
    if (!glfwInit())
    {
        LOG_E("Failed to initialize GLFW");
        return -1;
    }

    // Context hints — Core 3.3 on macOS, OpenGL ES 3.0 on Windows / Linux.
    // The PLATFORM_* defines come from lib/CMakeLists.txt.
#if defined(PLATFORM_MACOS)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else // Windows or Linux → OpenGL ES 3.0
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    int mainW = 900, mainH = 700;
    GLFWwindow *window = glfwCreateWindow(mainW, mainH, "Edge Lighting Effect", nullptr, nullptr);
    if (!window)
    {
        LOG_E("Failed to create main window");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // GL function loader — only desktop GL and ANGLE-on-Windows need GLAD.
    // Linux uses native <GLES3/gl3.h> whose symbols are already linked.
#if defined(PLATFORM_MACOS)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        LOG_E("Failed to initialize GLAD (desktop GL)");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
#elif defined(PLATFORM_WINDOWS)
    if (!gladLoadGLES2Loader((GLADloadproc)glfwGetProcAddress))
    {
        LOG_E("Failed to initialize GLAD (GLES 3.0)");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
#endif

    int displayW, displayH;
    glfwGetFramebufferSize(window, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glfwSetFramebufferSizeCallback(window, OnResize);
    glfwSetKeyCallback(window, OnKey);

    // --- Debug UI (separate window, shared GL context) ---
    DebugUI debugUI;
    if (!debugUI.Init(window, mainW, mainH))
    {
        glfwSetKeyCallback(window, nullptr);
        glfwSetFramebufferSizeCallback(window, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // --- Effect setup ---
    gEffect = std::make_unique<EdgeLighting::EdgeLightingEffect>();

    auto wireframeRenderer = std::make_shared<EdgeLighting::WireframeRenderer>();
    auto neonRenderer = std::make_shared<EdgeLighting::NeonRenderer>();
    auto neonMultiPassRenderer = std::make_shared<EdgeLighting::NeonMultiPassRenderer>();
    auto neonOptimizedRenderer = std::make_shared<EdgeLighting::NeonOptimizedRenderer>();

    gEffect->AddRenderer(wireframeRenderer);
    gEffect->AddRenderer(neonRenderer);
    gEffect->AddRenderer(neonMultiPassRenderer);
    gEffect->AddRenderer(neonOptimizedRenderer);

    EdgeLighting::Config config;
    config.geometry.width = displayW / 2;
    config.geometry.height = displayH / 2;
    config.geometry.position = glm::vec2(displayW / 4, displayH / 4);
    config.geometry.cornerRadius = 0.0f;
    config.neon.enable = true;
    config.wireframe.enable = true;
    config.wireframe.color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);

    gEffect->SetConfig(config);

    if (!gEffect->Initialize())
    {
        LOG_E("Failed to initialize EdgeLightingEffect");
        gEffect.reset();
        debugUI.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Debug background quad (verifies neon compositing: blend vs opaque).
    EdgeLightingDemo::BackgroundQuad background;
    if (!background.Init())
    {
        LOG_W("Background debug quad failed to initialise; continuing without it.");
    }

    EdgeLightingDemo::PrintControls();
    EdgeLightingDemo::PrintCurrentConfig(gEffect->GetConfig(), gEffect->GetClock().IsPlaying());

    float lastFrameTime = 0.0f;
    while (!glfwWindowShouldClose(window) && !glfwWindowShouldClose(debugUI.GetWindow()))
    {
        float currentFrameTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;

        // --- Debug UI (ImGui widgets + render to debug window) ---
        // Build the UI (mutates cfg from slider drags / preset picks), then
        // overlay the currently-active animation preset, then push back.
        {
            EdgeLighting::Config cfg = gEffect->GetConfig();
            debugUI.Build(cfg, *gEffect);
            debugUI.ApplyActiveAnimation(cfg, gEffect->GetClock().GetTime());
            gEffect->SetConfig(cfg);
        }
        debugUI.Render();

        // --- Render main window (no ImGui overlay) ---
        glfwMakeContextCurrent(window);
        {
            glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            int fbW, fbH;
            glfwGetFramebufferSize(window, &fbW, &fbH);

            // Debug checker background, drawn first so the effect composites
            // over it. Toggling Neon > Opaque shows blend vs occlude.
            if (debugUI.IsBackgroundEnabled())
            {
                background.Draw(debugUI.GetBackgroundCheckerSize(),
                                debugUI.GetBackgroundColorA(),
                                debugUI.GetBackgroundColorB());
            }

            gEffect->Update(deltaTime);

            double t0 = glfwGetTime();
            gEffect->Render(fbW, fbH);
            double t1 = glfwGetTime();
            debugUI.SetLastRenderTimeMs(static_cast<float>((t1 - t0) * 1000.0));

            glfwSwapBuffers(window);
        }

        glfwPollEvents();
    }

    // --- Shutdown ---
    debugUI.Shutdown();

    gEffect.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

void OnResize(GLFWwindow *window, int width, int height)
{
    glViewport(0, 0, width, height);

    if (gEffect)
    {
        EdgeLighting::Config config = gEffect->GetConfig();
        config.geometry.width = static_cast<float>(width) / 2;
        config.geometry.height = static_cast<float>(height) / 2;

        gEffect->SetConfig(config);
        EdgeLightingDemo::PrintCurrentConfig(config, gEffect->GetClock().IsPlaying());
    }
}

void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
    {
        return;
    }

    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(window, true);
        return;
    }

    if (!gEffect)
    {
        return;
    }
    EdgeLighting::Config config = gEffect->GetConfig();

    switch (key)
    {
    case GLFW_KEY_R:
    {
        config.neon.lineWidth = std::min(20.0f, config.neon.lineWidth + 1.0f);
        break;
    }
    case GLFW_KEY_F:
    {
        config.neon.lineWidth = std::max(1.0f, config.neon.lineWidth - 1.0f);
        break;
    }
    case GLFW_KEY_I:
    {
        config.neon.intensity = std::min(3.0f, config.neon.intensity + 0.1f);
        break;
    }
    case GLFW_KEY_LEFT_BRACKET:
    {
        config.neon.glowRadius = std::max(1.0f, config.neon.glowRadius - 1.0f);
        break;
    }
    case GLFW_KEY_RIGHT_BRACKET:
    {
        config.neon.glowRadius = std::min(80.0f, config.neon.glowRadius + 1.0f);
        break;
    }
    case GLFW_KEY_SPACE:
    {
        if (gEffect->GetClock().IsPlaying())
        {
            gEffect->GetClock().Pause();
        }
        else
        {
            gEffect->GetClock().Play();
        }
        break;
    }
    case GLFW_KEY_N:
    {
        if (mods & GLFW_MOD_SHIFT)
        {
            config.multipassNeon.enable = !config.multipassNeon.enable;
        }
        else
        {
            config.neon.enable = !config.neon.enable;
        }
        break;
    }
    case GLFW_KEY_G:
    {
        config.wireframe.enable = !config.wireframe.enable;
        break;
    }
    case GLFW_KEY_O:
    {
        if (mods & GLFW_MOD_SHIFT)
        {
            config.optimizedNeon.enable = !config.optimizedNeon.enable;
        }
        else
        {
            config.neon.intensity = std::max(0.0f, config.neon.intensity - 0.1f);
        }
        break;
    }
    case GLFW_KEY_W:
    {
        int w = static_cast<int>(config.geometry.winding);
        w = (w + 1) % 2;
        config.geometry.winding = static_cast<EdgeLighting::Winding>(w);
        break;
    }
    default:
    {
        return;
    }
    }

    gEffect->SetConfig(config);
    EdgeLightingDemo::PrintCurrentConfig(config, gEffect->GetClock().IsPlaying());
}
