#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "edge-lighting-capi.h"
#include "debug-ui.h"
#include "background-quad.h"
#include "image-quad.h"
#include "ui-controls.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    // Effect handle is global so the resize / key callbacks can reach it.
    // Ownership is the main function's - callbacks only borrow.
    el_effect_handle_t gEffect = nullptr;

    void OnResize(GLFWwindow * /*window*/, int width, int height)
    {
        // No glViewport here. This callback fires from glfwPollEvents on the
        // MAIN thread, which under --threaded does not hold the main window's
        // GL context - the render thread does. DrawMainWindow sets the
        // viewport from the published size every frame instead, which is
        // correct in both modes.
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
        if (get(gEffect, &v) != EL_SUCCESS)
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
            el_effect_get_debug_show_wireframe(gEffect, &on);
            el_effect_set_debug_show_wireframe(gEffect, on ? 0 : 1);
            break;
        }
        case GLFW_KEY_O:
        {
            if (mods & GLFW_MOD_SHIFT)
            {
                // Toggle the neon layer between full resolution and half.
                // There is no second renderer to switch to any more - this IS
                // the old "optimized on/off", expressed as the scale it always
                // was. Mirrors the same hotkey in demo/src/main.cpp.
                float scale = 1.0f;
                el_effect_get_neon_resolution_scale(gEffect, &scale);
                el_effect_set_neon_resolution_scale(gEffect, (scale < 1.0f) ? 1.0f : 0.5f);
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

    // ======================================================================
    // --threaded (EL_THREADING_SPLIT)
    //
    // The MAIN thread is the data thread. It has to be: on macOS GLFW
    // windowing and event polling are main-thread-only, and the ImGui debug
    // window is where the config is authored. A spawned RENDER thread owns the
    // main window's GL context for its whole life and does nothing but draw.
    //
    // The two windows already had separate, sharing contexts, so each thread
    // ends up with exactly one context current: the render thread holds the
    // main window's, and the main thread holds the debug window's - which is
    // what DebugUI::Render leaves current anyway.
    //
    // Everything the render thread needs but may not ask for itself (the
    // framebuffer size and the demo-side toggles are main-thread GLFW / ImGui
    // state; the rect geometry lives in the staging config) is published here
    // once per main-thread frame and snapshotted whole, so a drawn frame never
    // mixes halves of two publications.
    // ======================================================================
    struct RenderParams
    {
        int fbWidth = 0;
        int fbHeight = 0;
        bool backgroundEnabled = false;
        float checkerSize = 24.0f;
        float colorA[3] = {0.0f, 0.0f, 0.0f};
        float colorB[3] = {0.0f, 0.0f, 0.0f};
        bool imageBackdropEnabled = false;
        GLuint imageTexture = 0;
        float geoW = 0.0f, geoH = 0.0f, geoX = 0.0f, geoY = 0.0f, geoR = 0.0f;
    };

    std::mutex gParamsMutex;
    RenderParams gParams;

    std::atomic<bool> gRunning{true};
    std::atomic<bool> gRenderReady{false};
    std::atomic<bool> gRenderFailed{false};
    std::atomic<float> gLastRenderMs{0.0f};

    /// DATA THREAD. Gather this frame's render-side inputs.
    RenderParams CollectParams(GLFWwindow *window, const DebugUI &debugUI)
    {
        RenderParams p;
        glfwGetFramebufferSize(window, &p.fbWidth, &p.fbHeight);
        p.backgroundEnabled = debugUI.IsBackgroundEnabled();
        p.checkerSize = debugUI.GetBackgroundCheckerSize();
        std::memcpy(p.colorA, debugUI.GetBackgroundColorA(), sizeof(p.colorA));
        std::memcpy(p.colorB, debugUI.GetBackgroundColorB(), sizeof(p.colorB));
        p.imageBackdropEnabled = debugUI.IsImageBackdropEnabled();
        p.imageTexture = debugUI.GetImageBackdropTextureId();
        el_effect_get_geometry(gEffect, &p.geoW, &p.geoH, &p.geoX, &p.geoY, &p.geoR);
        return p;
    }

    void PublishParams(const RenderParams &p)
    {
        std::lock_guard<std::mutex> lock(gParamsMutex);
        gParams = p;
    }

    /// One frame of main-window drawing, on whichever thread owns that
    /// window's context: the main thread in single mode, the render thread
    /// under --threaded. Byte for byte the same work either way, which is the
    /// whole claim the flag is here to demonstrate.
    void DrawMainWindow(GLFWwindow *window,
                        EdgeLightingCapiDemo::BackgroundQuad &background,
                        EdgeLightingCapiDemo::ImageQuad &imageQuad,
                        const RenderParams &p, float dt)
    {
        glViewport(0, 0, p.fbWidth, p.fbHeight);
        glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (p.backgroundEnabled)
        {
            background.Draw(p.checkerSize, p.colorA, p.colorB);
        }
        if (p.imageBackdropEnabled)
        {
            imageQuad.Draw(p.fbWidth, p.fbHeight, p.geoX, p.geoY, p.geoW, p.geoH, p.imageTexture);
        }

        el_effect_update(gEffect, dt);
        const double t0 = glfwGetTime();
        el_effect_render(gEffect, p.fbWidth, p.fbHeight);
        const double t1 = glfwGetTime();
        gLastRenderMs.store(static_cast<float>((t1 - t0) * 1000.0));

        glfwSwapBuffers(window);
    }

    /// RENDER THREAD. Takes the main window's context, allocates everything
    /// that needs one, draws until told to stop, then destroys the effect and
    /// hands the context back.
    void RenderThreadMain(GLFWwindow *window,
                          EdgeLightingCapiDemo::BackgroundQuad *background,
                          EdgeLightingCapiDemo::ImageQuad *imageQuad)
    {
        glfwMakeContextCurrent(window);
        // Vsync on the render thread only - it applies to the current context,
        // so the debug window keeps swapping freely on the main thread. This
        // is the split's actual payoff made visible: the UI stays responsive
        // at its own rate while the effect is pinned to the display's.
        glfwSwapInterval(1);

        // el_effect_init belongs to the thread that owns GL: it compiles every
        // shader and allocates every buffer the renderers use. Same for the
        // two demo-side quads, which is why they are constructed on the main
        // thread but initialised here.
        if (el_effect_init(gEffect) != EL_SUCCESS)
        {
            std::fprintf(stderr, "el_effect_init failed on the render thread\n");
            el_effect_destroy(gEffect);
            gEffect = nullptr;
            gRenderFailed.store(true);
            gRenderReady.store(true);
            glfwMakeContextCurrent(nullptr);
            return;
        }
        if (!background->Init())
        {
            std::fprintf(stderr, "background quad init failed; continuing without it\n");
        }
        if (!imageQuad->Init())
        {
            std::fprintf(stderr, "image quad init failed; continuing without it\n");
        }
        gRenderReady.store(true);

        double last = glfwGetTime();
        while (gRunning.load())
        {
            const double now = glfwGetTime();
            const float dt = static_cast<float>(now - last);
            last = now;

            RenderParams p;
            {
                std::lock_guard<std::mutex> lock(gParamsMutex);
                p = gParams;
            }
            if (p.fbWidth <= 0 || p.fbHeight <= 0)
            {
                // Minimised, or the main thread has not published yet.
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            DrawMainWindow(window, *background, *imageQuad, p, dt);
        }

        // Step 2 of the shutdown protocol. The data thread has already called
        // el_effect_shutdown and stopped touching the handle, so destroying it
        // here - on the thread that owns its GL objects - is the safe half.
        el_effect_destroy(gEffect);
        gEffect = nullptr;
        glfwMakeContextCurrent(nullptr);
    }
} // namespace

int main(int argc, char **argv)
{
    bool threaded = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--threaded") == 0)
        {
            threaded = true;
        }
    }

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

    // Before el_effect_init and immutable after it, so it has to happen here.
    if (threaded && el_effect_set_threading_mode(gEffect, EL_THREADING_SPLIT) != EL_SUCCESS)
    {
        std::fprintf(stderr, "el_effect_set_threading_mode(SPLIT) failed\n");
        el_effect_destroy(gEffect);
        gEffect = nullptr;
        debugUI.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Staging-only, so it is legal before init in either mode - and in split
    // mode it means the mailbox is seeded with the authored config and the
    // render thread's first frame is not a default one.
    el_effect_set_geometry(gEffect,
                           static_cast<float>(displayW) / 2.0f,
                           static_cast<float>(displayH) / 2.0f,
                           static_cast<float>(displayW) / 4.0f,
                           static_cast<float>(displayH) / 4.0f,
                           0.0f);
    el_effect_set_neon_renderer_enabled(gEffect, 1);
    el_effect_set_debug_show_wireframe(gEffect, 1);
    el_effect_set_debug_wireframe_color(gEffect, 0.0f, 1.0f, 0.0f, 1.0f);

    // Constructed here, initialised wherever the main window's context ends up
    // living - see RenderThreadMain for the split-mode half.
    EdgeLightingCapiDemo::BackgroundQuad background;
    EdgeLightingCapiDemo::ImageQuad imageQuad;

    std::thread renderer;
    if (threaded)
    {
        // Release the main window's context so the render thread can take it.
        glfwMakeContextCurrent(nullptr);
        PublishParams(CollectParams(window, debugUI));
        renderer = std::thread(RenderThreadMain, window, &background, &imageQuad);
        while (!gRenderReady.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (gRenderFailed.load())
        {
            renderer.join();
            debugUI.Shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
        // The main thread's steady state from here: the debug window's
        // context, which is also what DebugUI::Render leaves current.
        glfwMakeContextCurrent(debugUI.GetWindow());
    }
    else
    {
        if (el_effect_init(gEffect) != EL_SUCCESS)
        {
            std::fprintf(stderr, "el_effect_init failed\n");
            el_effect_destroy(gEffect);
            gEffect = nullptr;
            debugUI.Shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
        if (!background.Init())
        {
            std::fprintf(stderr, "background quad init failed; continuing without it\n");
        }
        if (!imageQuad.Init())
        {
            std::fprintf(stderr, "image quad init failed; continuing without it\n");
        }
    }

    EdgeLightingCapiDemo::PrintControls();
    EdgeLightingCapiDemo::PrintCurrentStatus(gEffect);
    std::printf("threading: %s\n",
                threaded ? "EL_THREADING_SPLIT - main thread authors config, render thread owns GL"
                         : "EL_THREADING_SINGLE - everything on this thread (pass --threaded for the split)");

    float lastFrameTime = static_cast<float>(glfwGetTime());
    while (!glfwWindowShouldClose(window) && !glfwWindowShouldClose(debugUI.GetWindow()))
    {
        const float now = static_cast<float>(glfwGetTime());
        const float dt = now - lastFrameTime;
        lastFrameTime = now;

        // --- Debug UI (ImGui). Pure data-thread work in both modes: every
        //     widget reads and writes the staging config and nothing else.
        debugUI.SetLastRenderTimeMs(gLastRenderMs.load());
        debugUI.Build(gEffect);
        debugUI.Render();

        if (threaded)
        {
            // Hand the render thread this frame's config and parameters, then
            // run whatever it deferred back to us. A completion callback runs
            // HERE, on the thread that owns the staging config, which is what
            // makes it legal for one to call el_effect_set_*.
            PublishParams(CollectParams(window, debugUI));
            el_effect_publish(gEffect);
            el_effect_poll_callbacks(gEffect);
        }
        else
        {
            glfwMakeContextCurrent(window);
            DrawMainWindow(window, background, imageQuad,
                           CollectParams(window, debugUI), dt);
        }

        glfwPollEvents();
    }

    if (threaded)
    {
        // Step 1 of the shutdown protocol, and the order matters: close the
        // data thread's side while the render thread is still draining, THEN
        // stop it, THEN let it destroy the effect. Reversing the first two
        // would race the destroy against a publish.
        el_effect_shutdown(gEffect);
        gRunning.store(false);
        renderer.join();
        // Reclaim the context the render thread handed back, so ImGui's
        // shutdown and the quads' destructors still have one current - the
        // same state the single-threaded path leaves here.
        glfwMakeContextCurrent(window);
    }
    else
    {
        el_effect_destroy(gEffect);
        gEffect = nullptr;
    }

    debugUI.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
