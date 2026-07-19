#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "edge-lighting-capi.h"
#include "debug-ui.h"
#include "background-quad.h"
#include "image-quad.h"
#include "ui-controls.h"

#include <cstdio>
#include <memory>

namespace
{
    // Effect handle is global so the resize / key callbacks can reach it.
    // Ownership is the main function's - callbacks only borrow.
    el_effect_handle_t gEffect = nullptr;

    void OnResize(GLFWwindow * /*window*/, int width, int height)
    {
        glViewport(0, 0, width, height);
        if (!gEffect)
        {
            return;
        }
        // Match the demo/ behaviour: rect fills the middle half of the frame.
        float curW = 0, curH = 0, curX = 0, curY = 0, curR = 0;
        el_effect_get_geometry(gEffect, &curW, &curH, &curX, &curY, &curR);
        el_effect_set_geometry(gEffect,
                               static_cast<float>(width) / 2.0f,
                               static_cast<float>(height) / 2.0f,
                               static_cast<float>(width) / 4.0f,
                               static_cast<float>(height) / 4.0f,
                               curR);
    }

    // Small helper: `nudge(getter, setter, delta, lo, hi)` reads the current
    // scalar via getter, clamps `value + delta` into [lo, hi], and writes it
    // back through setter. Keeps the OnKey switch legible.
    template <typename Getter, typename Setter>
    void Nudge(Getter get, Setter set, float delta, float lo, float hi)
    {
        float v = 0.0f;
        if (get(gEffect, &v) != EL_OK)
        {
            return;
        }
        v += delta;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        set(gEffect, v);
    }

    void OnKey(GLFWwindow *window, int key, int /*sc*/, int action, int mods)
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

        switch (key)
        {
        case GLFW_KEY_R:
        {
            Nudge(el_effect_get_line_width, el_effect_set_line_width, +1.0f, 1.0f, 20.0f);
            break;
        }
        case GLFW_KEY_F:
        {
            Nudge(el_effect_get_line_width, el_effect_set_line_width, -1.0f, 1.0f, 20.0f);
            break;
        }
        case GLFW_KEY_I:
        {
            Nudge(el_effect_get_intensity, el_effect_set_intensity, +0.1f, 0.0f, 3.0f);
            break;
        }
        case GLFW_KEY_LEFT_BRACKET:
        {
            Nudge(el_effect_get_glow_radius, el_effect_set_glow_radius, -1.0f, 1.0f, 80.0f);
            break;
        }
        case GLFW_KEY_RIGHT_BRACKET:
        {
            Nudge(el_effect_get_glow_radius, el_effect_set_glow_radius, +1.0f, 1.0f, 80.0f);
            break;
        }
        case GLFW_KEY_P:
        {
            Nudge(el_effect_get_hue_rotation_rate, el_effect_set_hue_rotation_rate, +0.1f, 0.0f, 5.0f);
            break;
        }
        case GLFW_KEY_L:
        {
            Nudge(el_effect_get_hue_rotation_rate, el_effect_set_hue_rotation_rate, -0.1f, 0.0f, 5.0f);
            break;
        }
        case GLFW_KEY_SPACE:
        {
            el_bool_t playing = 0;
            el_effect_clock_is_playing(gEffect, &playing);
            if (playing)
            {
                el_effect_clock_pause(gEffect);
            }
            else
            {
                el_effect_clock_play(gEffect);
            }
            break;
        }
        case GLFW_KEY_N:
        {
            el_bool_t on = 0;
            el_effect_get_neon_renderer_enabled(gEffect, &on);
            el_effect_set_neon_renderer_enabled(gEffect, on ? 0 : 1);
            break;
        }
        case GLFW_KEY_G:
        {
            el_bool_t on = 0;
            el_effect_get_wireframe_renderer_enabled(gEffect, &on);
            el_effect_set_wireframe_renderer_enabled(gEffect, on ? 0 : 1);
            break;
        }
        case GLFW_KEY_O:
        {
            if (mods & GLFW_MOD_SHIFT)
            {
                el_bool_t on = 0;
                el_effect_get_optimized_renderer_enabled(gEffect, &on);
                el_effect_set_optimized_renderer_enabled(gEffect, on ? 0 : 1);
            }
            else
            {
                Nudge(el_effect_get_intensity, el_effect_set_intensity, -0.1f, 0.0f, 3.0f);
            }
            break;
        }
        case GLFW_KEY_W:
        {
            el_winding_e w = EL_WINDING_CLOCKWISE;
            el_effect_get_winding(gEffect, &w);
            el_effect_set_winding(gEffect,
                                  w == EL_WINDING_CLOCKWISE ? EL_WINDING_COUNTER_CLOCKWISE : EL_WINDING_CLOCKWISE);
            break;
        }
        default:
        {
            return;
        }
        }
    }
} // namespace

int main()
{
    if (!glfwInit())
    {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

#if defined(PLATFORM_MACOS)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    int mainW = 900, mainH = 700;
    GLFWwindow *window = glfwCreateWindow(mainW, mainH,
                                          "Edge Lighting Effect (capi demo)", nullptr, nullptr);
    if (!window)
    {
        std::fprintf(stderr, "Failed to create main window\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

#if defined(PLATFORM_MACOS)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::fprintf(stderr, "Failed to initialize GLAD\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
#else
    if (!gladLoadGLES2Loader((GLADloadproc)glfwGetProcAddress))
    {
        std::fprintf(stderr, "Failed to initialize GLAD (GLES 3.0)\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
#endif

    int displayW = 0, displayH = 0;
    glfwGetFramebufferSize(window, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glfwSetFramebufferSizeCallback(window, OnResize);
    glfwSetKeyCallback(window, OnKey);

    // --- Debug UI (separate window, shared context) ---
    DebugUI debugUI;
    if (!debugUI.Init(window, mainW, mainH))
    {
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // --- Effect setup (all through capi) ---
    gEffect = el_effect_create();
    if (!gEffect)
    {
        std::fprintf(stderr, "el_effect_create failed\n");
        debugUI.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    if (el_effect_init(gEffect) != EL_OK)
    {
        std::fprintf(stderr, "el_effect_init failed\n");
        el_effect_destroy(gEffect);
        gEffect = nullptr;
        debugUI.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    el_effect_set_geometry(gEffect,
                           static_cast<float>(displayW) / 2.0f,
                           static_cast<float>(displayH) / 2.0f,
                           static_cast<float>(displayW) / 4.0f,
                           static_cast<float>(displayH) / 4.0f,
                           0.0f);
    el_effect_set_neon_renderer_enabled(gEffect, 1);
    el_effect_set_wireframe_renderer_enabled(gEffect, 1);
    el_effect_set_wireframe_color(gEffect, 0.0f, 1.0f, 0.0f, 1.0f);

    EdgeLightingCapiDemo::BackgroundQuad background;
    if (!background.Init())
    {
        std::fprintf(stderr, "background quad init failed; continuing without it\n");
    }
    EdgeLightingCapiDemo::ImageQuad imageQuad;
    if (!imageQuad.Init())
    {
        std::fprintf(stderr, "image quad init failed; continuing without it\n");
    }

    EdgeLightingCapiDemo::PrintControls();
    EdgeLightingCapiDemo::PrintCurrentStatus(gEffect);

    float lastFrameTime = 0.0f;
    while (!glfwWindowShouldClose(window) && !glfwWindowShouldClose(debugUI.GetWindow()))
    {
        const float now = static_cast<float>(glfwGetTime());
        const float dt = now - lastFrameTime;
        lastFrameTime = now;

        // --- Debug UI (ImGui) ---
        debugUI.Build(gEffect);
        debugUI.Render();

        // --- Main window ---
        glfwMakeContextCurrent(window);
        {
            glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            int fbW = 0, fbH = 0;
            glfwGetFramebufferSize(window, &fbW, &fbH);

            if (debugUI.IsBackgroundEnabled())
            {
                background.Draw(debugUI.GetBackgroundCheckerSize(),
                                debugUI.GetBackgroundColorA(),
                                debugUI.GetBackgroundColorB());
            }
            if (debugUI.IsImageBackdropEnabled())
            {
                float geoW = 0, geoH = 0, geoX = 0, geoY = 0, geoR = 0;
                el_effect_get_geometry(gEffect, &geoW, &geoH, &geoX, &geoY, &geoR);
                imageQuad.Draw(fbW, fbH, geoX, geoY, geoW, geoH,
                               debugUI.GetImageBackdropTextureId());
            }

            el_effect_update(gEffect, dt);
            const double t0 = glfwGetTime();
            el_effect_render(gEffect, fbW, fbH);
            const double t1 = glfwGetTime();
            debugUI.SetLastRenderTimeMs(static_cast<float>((t1 - t0) * 1000.0));

            glfwSwapBuffers(window);
        }

        glfwPollEvents();
    }

    debugUI.Shutdown();
    el_effect_destroy(gEffect);
    gEffect = nullptr;
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
