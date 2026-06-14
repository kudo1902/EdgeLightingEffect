#include "gl/gl-header.h"
#include <GLFW/glfw3.h>
#include "core/edge-lighting.h"
#include "renderer/stroke-renderer.h"
#include "renderer/wireframe-renderer.h"
#include "renderer/particle-renderer.h"
#include "debug-ui.h"
#include "ui-controls.h"
#include "util/log-util.h"
#include <memory>

std::unique_ptr<EdgeLighting::EdgeLightingEffect> gEffect;
static std::vector<glm::vec2> gStoredMaskPoints;

// Preset custom paths in app space: rect top-left = (0,0), +X right, +Y down.
// Fits the initial 900×700 window.
static std::vector<glm::vec2> MakeTrianglePath()
{
    return {
        {410.0f, 30.0f},  // center near top edge
        {100.0f, 590.0f}, // near bottom-left corner
        {720.0f, 590.0f}, // near bottom-right corner
    };
}

static std::vector<glm::vec2> MakeDiamondPath()
{
    return {
        {410.0f, 30.0f},  // center near top edge
        {760.0f, 310.0f}, // near right edge, centered vertically
        {410.0f, 590.0f}, // center near bottom edge
        {60.0f, 310.0f},  // near left edge, centered vertically
    };
}

void OnResize(GLFWwindow *window, int width, int height);
void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods);

int main()
{
    if (!glfwInit())
    {
        LOG_E("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
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

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        LOG_E("Failed to initialize GLAD");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

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

    auto strokeRenderer = std::make_shared<EdgeLighting::StrokeRenderer>();
    auto wireframeRenderer = std::make_shared<EdgeLighting::WireframeRenderer>();
    auto particleRenderer = std::make_shared<EdgeLighting::ParticleRenderer>();
    gEffect->AddRenderer(strokeRenderer);
    gEffect->AddRenderer(wireframeRenderer);
    gEffect->AddRenderer(particleRenderer);

    EdgeLighting::Config config;
    config.geometry.width = displayW / 2;
    config.geometry.height = displayH / 2;
    config.geometry.position = glm::vec2(0, 0);
    config.geometry.borderRadius = 0.0f;
    config.stroke.primaryColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    config.stroke.secondaryColor = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
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

    EdgeLightingDemo::PrintControls();
    EdgeLightingDemo::PrintCurrentConfig(gEffect->GetConfig(), gEffect->GetAnimation().IsPlaying());

    float lastFrameTime = 0.0f;
    while (!glfwWindowShouldClose(window) && !glfwWindowShouldClose(debugUI.GetWindow()))
    {
        float currentFrameTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;

        // --- Debug UI (ImGui widgets + render to debug window) ---
        {
            EdgeLighting::Config cfg = gEffect->GetConfig();
            debugUI.Build(cfg, *gEffect);
        }
        debugUI.Render();

        // --- Render main window (no ImGui overlay) ---
        glfwMakeContextCurrent(window);
        {
            glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            gEffect->Update(deltaTime);

            int fbW, fbH;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            gEffect->Render(fbW, fbH);

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
        float oldW = config.geometry.width;
        float oldH = config.geometry.height;
        config.geometry.width = static_cast<float>(width) / 2;
        config.geometry.height = static_cast<float>(height) / 2;

        if (config.path.source == EdgeLighting::PathSource::MASK && !gStoredMaskPoints.empty())
        {
            float sx = config.geometry.width / oldW;
            float sy = config.geometry.height / oldH;
            for (auto &pt : config.path.points)
            {
                pt.x *= sx;
                pt.y *= sy;
            }
            gStoredMaskPoints = config.path.points;
        }

        gEffect->SetConfig(config);
        EdgeLightingDemo::PrintCurrentConfig(config, gEffect->GetAnimation().IsPlaying());
    }
}

void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return;

    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(window, true);
        return;
    }

    if (!gEffect)
        return;
    EdgeLighting::Config config = gEffect->GetConfig();

    switch (key)
    {
    case GLFW_KEY_R:
        config.stroke.thickness = std::min(40.0f, config.stroke.thickness + 1.0f);
        break;
    case GLFW_KEY_F:
        config.stroke.thickness = std::max(1.0f, config.stroke.thickness - 1.0f);
        break;
    case GLFW_KEY_K:
        config.geometry.borderRadius = std::min(200.0f, config.geometry.borderRadius + 5.0f);
        break;
    case GLFW_KEY_J:
        config.geometry.borderRadius = std::max(0.0f, config.geometry.borderRadius - 5.0f);
        break;
    case GLFW_KEY_I:
        config.stroke.intensity = std::min(1.0f, config.stroke.intensity + 0.1f);
        break;
    case GLFW_KEY_O:
        config.stroke.intensity = std::max(0.0f, config.stroke.intensity - 0.1f);
        break;
    case GLFW_KEY_T:
    {
        int s = static_cast<int>(config.stroke.alignment);
        s = (s + 1) % 3;
        config.stroke.alignment = static_cast<EdgeLighting::StrokeAlignment>(s);
        break;
    }
    case GLFW_KEY_H:
        if (mods & GLFW_MOD_SHIFT)
            config.stroke.fadeRange = std::max(0.0f, config.stroke.fadeRange - 1.0f);
        else
            config.stroke.fadeRange = std::min(config.stroke.thickness, config.stroke.fadeRange + 1.0f);
        break;
    case GLFW_KEY_M:
    {
        int m = static_cast<int>(config.stroke.animation);
        m = (m + 1) % 3;
        config.stroke.animation = static_cast<EdgeLighting::StrokeAnimation>(m);
        break;
    }
    case GLFW_KEY_U:
        config.stroke.segmentLength = std::min(1.0f, config.stroke.segmentLength + 0.05f);
        break;
    case GLFW_KEY_Y:
        config.stroke.segmentLength = std::max(0.05f, config.stroke.segmentLength - 0.05f);
        break;
    case GLFW_KEY_P:
        config.stroke.speed = std::min(5.0f, config.stroke.speed + 0.1f);
        break;
    case GLFW_KEY_L:
        config.stroke.speed = std::max(0.1f, config.stroke.speed - 0.1f);
        break;
    case GLFW_KEY_SPACE:
        if (gEffect->GetAnimation().IsPlaying())
            gEffect->GetAnimation().Pause();
        else
            gEffect->GetAnimation().Play();
        break;
    case GLFW_KEY_B:
    {
        int m = static_cast<int>(config.stroke.fadeMode);
        m = (m + 1) % 2;
        config.stroke.fadeMode = static_cast<EdgeLighting::FadeMode>(m);
        break;
    }
    case GLFW_KEY_C:
    {
        int m = static_cast<int>(config.stroke.colorMode);
        m = (m + 1) % 5;
        config.stroke.colorMode = static_cast<EdgeLighting::StrokeColorMode>(m);
        break;
    }
    case GLFW_KEY_COMMA:
        config.stroke.lineCount = std::max(1, config.stroke.lineCount - 1);
        break;
    case GLFW_KEY_PERIOD:
        config.stroke.lineCount = std::min(16, config.stroke.lineCount + 1);
        break;
    case GLFW_KEY_N:
        config.particles.enable = !config.particles.enable;
        break;
    case GLFW_KEY_V:
        config.stroke.glowEnable = !config.stroke.glowEnable;
        break;
    case GLFW_KEY_LEFT_BRACKET:
        config.stroke.glowSize = std::max(1.0f, config.stroke.glowSize - 1.0f);
        break;
    case GLFW_KEY_RIGHT_BRACKET:
        config.stroke.glowSize = std::min(40.0f, config.stroke.glowSize + 1.0f);
        break;
    case GLFW_KEY_SEMICOLON:
        config.stroke.glowIntensity = std::max(0.0f, config.stroke.glowIntensity - 0.05f);
        break;
    case GLFW_KEY_APOSTROPHE:
        config.stroke.glowIntensity = std::min(1.0f, config.stroke.glowIntensity + 0.05f);
        break;
    case GLFW_KEY_G:
        config.wireframe.enable = !config.wireframe.enable;
        break;
    case GLFW_KEY_X:
    {
        int s = static_cast<int>(config.path.source);
        s = (s + 1) % 3;
        config.path.source = static_cast<EdgeLighting::PathSource>(s);
        if (config.path.source == EdgeLighting::PathSource::CUSTOM)
        {
            config.path.points = MakeTrianglePath();
            config.path.closed = true;
        }
        else if (config.path.source == EdgeLighting::PathSource::MASK && !gStoredMaskPoints.empty())
        {
            config.path.points = gStoredMaskPoints;
            config.path.closed = true;
        }
        break;
    }
    case GLFW_KEY_1:
        config.path.startPos = std::max(0.0f, config.path.startPos - 0.05f);
        break;
    case GLFW_KEY_2:
        config.path.startPos = std::min(1.0f, config.path.startPos + 0.05f);
        break;
    case GLFW_KEY_3:
        config.path.endPos = std::max(0.0f, config.path.endPos - 0.05f);
        break;
    case GLFW_KEY_4:
        config.path.endPos = std::min(1.0f, config.path.endPos + 0.05f);
        break;
    case GLFW_KEY_5:
        config.path.closed = !config.path.closed;
        break;
    case GLFW_KEY_Z:
    {
        if (gEffect->SetMaskFromFile(RES_DIR "/mask.png"))
        {
            config = gEffect->GetConfig();
            gStoredMaskPoints = config.path.points;
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
    case GLFW_KEY_6:
        if (config.path.source == EdgeLighting::PathSource::CUSTOM)
        {
            config.path.points = MakeDiamondPath();
        }
        break;
    default:
        return;
    }

    gEffect->SetConfig(config);
    EdgeLightingDemo::PrintCurrentConfig(config, gEffect->GetAnimation().IsPlaying());
}
