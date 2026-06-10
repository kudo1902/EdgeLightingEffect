#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/edge-lighting.h"
#include "renderer/segment-renderer.h"
#include "renderer/particle-renderer.h"
#include "ui-controls.h"
#include <iostream>
#include <memory>

// Global effect instance
EdgeLighting::EdgeLightingEffect *g_Effect = nullptr;
int g_ColorThemeIndex = 0;

// Callback function declarations (prefixed with 'On')
void OnResize(GLFWwindow *window, int width, int height);
void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods);

int main()
{
    // Initialize GLFW
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    // Configure GLFW for OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Create GLFW window
    int initialWidth = 900;
    int initialHeight = 700;
    GLFWwindow *window = glfwCreateWindow(initialWidth, initialHeight, "Edge Lighting Effect Demo", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Setup Viewport and callbacks
    glViewport(0, 0, initialWidth, initialHeight);
    glfwSetFramebufferSizeCallback(window, OnResize);
    glfwSetKeyCallback(window, OnKey);

    // Initialize Edge Lighting Effect
    g_Effect = new EdgeLighting::EdgeLightingEffect();

    // Create Renderer Modules
    auto segmentRenderer = std::make_shared<EdgeLighting::SegmentRenderer>();
    auto particleRenderer = std::make_shared<EdgeLighting::ParticleRenderer>();

    g_Effect->AddRenderer(segmentRenderer);
    g_Effect->AddRenderer(particleRenderer);

    // Set initial size of the rectangle to match window with margins
    EdgeLighting::Config config;
    config.width = static_cast<float>(initialWidth) - 80.0f;
    config.height = static_cast<float>(initialHeight) - 80.0f;
    config.borderRadius = 50.0f;
    config.glowWidth = 45.0f;
    config.lineWidth = 8.0f;
    config.speed = 0.6f;
    config.lineLength = 0.3f;
    config.colorMode = EdgeLighting::ColorMode::AMBIENT_RAINBOW;

    g_Effect->SetConfig(config);

    if (!g_Effect->Initialize())
    {
        std::cerr << "Failed to initialize EdgeLightingEffect" << std::endl;
        delete g_Effect;
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // Bind animation callbacks
    g_Effect->GetAnimation().OnLoopCompleted = []()
    {
        // Triggered every time a loop completes
    };

    // Print CLI controls instructions
    EdgeLightingDemo::PrintControls();
    EdgeLightingDemo::PrintCurrentConfig(g_Effect->GetConfig(), g_Effect->GetAnimation().IsPlaying());

    // Main render loop
    float lastFrameTime = 0.0f;
    while (!glfwWindowShouldClose(window))
    {
        float currentFrameTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;

        // Clear with a deep dark background
        glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update effect animation and active renderers
        g_Effect->Update(deltaTime);

        // Render the effect (gets viewport size dynamically)
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        g_Effect->Render(displayW, displayH);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    delete g_Effect;
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// Window resizing callback
void OnResize(GLFWwindow *window, int width, int height)
{
    glViewport(0, 0, width, height);

    if (g_Effect)
    {
        // Dynamically adjust rectangle dimensions to fit the window with margins
        EdgeLighting::Config config = g_Effect->GetConfig();
        config.width = static_cast<float>(width) - 80.0f;
        config.height = static_cast<float>(height) - 80.0f;
        g_Effect->SetConfig(config);
        EdgeLightingDemo::PrintCurrentConfig(config, g_Effect->GetAnimation().IsPlaying());
    }
}

// Keyboard input callback
void OnKey(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return;

    if (key == GLFW_KEY_ESCAPE)
    {
        glfwSetWindowShouldClose(window, true);
        return;
    }

    if (!g_Effect)
        return;
    EdgeLighting::Config config = g_Effect->GetConfig();
    EdgeLighting::Animation &animation = g_Effect->GetAnimation();

    switch (key)
    {
    case GLFW_KEY_SPACE:
        if (animation.IsPlaying())
        {
            animation.Pause();
        }
        else
        {
            animation.Play();
        }
        break;
    case GLFW_KEY_1:
        config.colorMode = EdgeLighting::ColorMode::STATIC;
        break;
    case GLFW_KEY_2:
        config.colorMode = EdgeLighting::ColorMode::GRADIENT;
        break;
    case GLFW_KEY_3:
        config.colorMode = EdgeLighting::ColorMode::AMBIENT_RAINBOW;
        break;
    case GLFW_KEY_UP:
        config.speed = std::min(5.0f, config.speed + 0.1f);
        break;
    case GLFW_KEY_DOWN:
        config.speed = std::max(0.1f, config.speed - 0.1f);
        break;
    case GLFW_KEY_RIGHT:
        config.lineLength = std::min(0.9f, config.lineLength + 0.05f);
        break;
    case GLFW_KEY_LEFT:
        config.lineLength = std::max(0.05f, config.lineLength - 0.05f);
        break;
    case GLFW_KEY_W:
        config.glowWidth = std::min(150.0f, config.glowWidth + 5.0f);
        break;
    case GLFW_KEY_S:
        config.glowWidth = std::max(5.0f, config.glowWidth - 5.0f);
        break;
    case GLFW_KEY_A:
        config.lineWidth = std::min(40.0f, config.lineWidth + 1.0f);
        break;
    case GLFW_KEY_D:
        config.lineWidth = std::max(1.0f, config.lineWidth - 1.0f);
        break;
    case GLFW_KEY_P:
        config.enableParticles = !config.enableParticles;
        break;
    case GLFW_KEY_L:
        config.lightCount = (config.lightCount % 4) + 1;
        break;
    case GLFW_KEY_C:
    {
        // Cycle primary color presets
        g_ColorThemeIndex = (g_ColorThemeIndex + 1) % 4;
        if (g_ColorThemeIndex == 0)
        {
            config.primaryColor = glm::vec4(0.0f, 0.8f, 1.0f, 1.0f);   // Cyan
            config.secondaryColor = glm::vec4(1.0f, 0.0f, 0.8f, 1.0f); // Magenta
        }
        else if (g_ColorThemeIndex == 1)
        {
            config.primaryColor = glm::vec4(0.0f, 1.0f, 0.4f, 1.0f);   // Emerald Green
            config.secondaryColor = glm::vec4(0.9f, 0.9f, 0.0f, 1.0f); // Gold
        }
        else if (g_ColorThemeIndex == 2)
        {
            config.primaryColor = glm::vec4(1.0f, 0.3f, 0.0f, 1.0f);   // Orange Red
            config.secondaryColor = glm::vec4(0.5f, 0.0f, 1.0f, 1.0f); // Purple
        }
        else
        {
            config.primaryColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);   // White
            config.secondaryColor = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f); // Dark grey
        }
        break;
    }
    case GLFW_KEY_R:
    {
        config = EdgeLighting::Config();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        config.width = static_cast<float>(w) - 80.0f;
        config.height = static_cast<float>(h) - 80.0f;
        config.borderRadius = 50.0f;
        config.glowWidth = 45.0f;
        config.lineWidth = 8.0f;
        config.speed = 0.6f;
        config.lineLength = 0.3f;
        animation.Play();
        break;
    }
    default:
        return;
    }

    g_Effect->SetConfig(config);
    EdgeLightingDemo::PrintCurrentConfig(config, animation.IsPlaying());
}
