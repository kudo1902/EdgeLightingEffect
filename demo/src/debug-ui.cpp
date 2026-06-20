#include "debug-ui.h"
#include "core/config.h"
#include "core/edge-lighting.h"
#include "ui-controls.h"
#include "util/log-util.h"
#include "util/screenshot-util.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DebugUI::~DebugUI()
{
    Shutdown();
}

bool DebugUI::Init(GLFWwindow *mainWindow, int mainW, int mainH)
{
    int dbgW = 420, dbgH = 700;
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    mWindow = glfwCreateWindow(dbgW, dbgH, "Debug Controls", nullptr, mainWindow);
    if (!mWindow)
    {
        LOG_E("Failed to create debug window");
        return false;
    }
    glfwSetWindowPos(mWindow, mainW + 20, 40);
    glfwSetWindowAttrib(mWindow, GLFW_FLOATING, GLFW_TRUE);

    mMainWindow = mainWindow;

    // Keep main context current after window creation (shared context)
    glfwMakeContextCurrent(mainWindow);

    // Second ImGui context for the debug window
    IMGUI_CHECKVERSION();
    mContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(mContext);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(mWindow, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    return true;
}

void DebugUI::Shutdown()
{
    if (mContext)
    {
        ImGui::SetCurrentContext(mContext);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(mContext);
        mContext = nullptr;
    }
    if (mWindow)
    {
        glfwDestroyWindow(mWindow);
        mWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

void DebugUI::Build(EdgeLighting::Config &cfg, EdgeLighting::EdgeLightingEffect &effect)
{
    ImGui::SetCurrentContext(mContext);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    bool playing = effect.GetAnimation().IsPlaying();

    ImGui::Begin("Debug Controls");
    // --- Perf readout (always visible, sticky at the top) ---
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("FPS: %.1f  |  %.2f ms/frame", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Separator();

    buildGeometrySection(cfg);
    buildNeonSection(cfg);
    buildMultiPassNeonSection(cfg);

    ImGui::Checkbox("Wireframe", &cfg.wireframe.enable);

    ImGui::Separator();
    ImGui::Text("Animation: %s", playing ? "PLAYING" : "PAUSED");
    if (ImGui::Button(playing ? "Pause" : "Play"))
    {
        if (playing)
        {
            effect.GetAnimation().Pause();
        }
        else
            effect.GetAnimation().Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        effect.GetAnimation().Stop();
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot"))
    {
        int fbW, fbH;
        glfwGetFramebufferSize(mMainWindow, &fbW, &fbH);
        std::string path = EdgeLighting::ScreenshotUtil::TimestampedPath(RES_DIR, "screenshot_", "png");
        EdgeLighting::ScreenshotUtil::SaveScreenshot(path, fbW, fbH);
        LOG_I("Screenshot saved: %s", path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump Config"))
    {
        EdgeLightingDemo::PrintCurrentConfig(effect.GetConfig(), effect.GetAnimation().IsPlaying());
        std::cout << "\n";
    }
    ImGui::End();

    effect.SetConfig(cfg);
}

void DebugUI::Render()
{
    ImGui::Render();

    glfwMakeContextCurrent(mWindow);
    int fbDW, fbDH;
    glfwGetFramebufferSize(mWindow, &fbDW, &fbDH);
    glViewport(0, 0, fbDW, fbDH);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(mWindow);
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void DebugUI::buildGeometrySection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::SliderFloat("Width", &cfg.geometry.width, 100.0f, 1600.0f, "%.0f");
    ImGui::SliderFloat("Height", &cfg.geometry.height, 100.0f, 1200.0f, "%.0f");
    ImGui::SliderFloat("Corner Radius", &cfg.geometry.cornerRadius, 0.0f, 200.0f, "%.0f");

    const char *windingItems[] = {"CW", "CCW"};
    int windingIdx = static_cast<int>(cfg.geometry.winding);
    if (ImGui::Combo("Winding", &windingIdx, windingItems, IM_ARRAYSIZE(windingItems)))
    {
        cfg.geometry.winding = static_cast<EdgeLighting::Winding>(windingIdx);
    }
}

void DebugUI::buildNeonSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Neon", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Enable##Neon", &cfg.neon.enable);
    if (!cfg.neon.enable)
    {
        return;
    }

    ImGui::SliderFloat("Line Width##Neon", &cfg.neon.lineWidth, 1.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Neon", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Neon", &cfg.neon.glowRadius, 1.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Neon", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Sweep Speed##Neon", &cfg.neon.sweepSpeed, 0.0f, 2.0f, "%.2f");

    const char *sideItems[] = {"Both", "Inside", "Outside"};
    int sideIdx = static_cast<int>(cfg.neon.glowSide);
    if (ImGui::Combo("Glow Side##Neon", &sideIdx, sideItems, IM_ARRAYSIZE(sideItems)))
    {
        cfg.neon.glowSide = static_cast<EdgeLighting::GlowSide>(sideIdx);
    }
    if (cfg.neon.glowSide != EdgeLighting::GlowSide::BOTH)
    {
        ImGui::SliderFloat("Side Softness##Neon", &cfg.neon.glowSideSoftness, 0.0f, 20.0f, "%.1f");
    }

    const char *blendItems[] = {"RGB", "HSV"};
    int blendIdx = static_cast<int>(cfg.neon.blendSpace);
    if (ImGui::Combo("Blend Space##Neon", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
    {
        cfg.neon.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
    }

    for (size_t i = 0; i < cfg.neon.colorStops.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        float p = cfg.neon.colorStops[i].position;
        if (ImGui::SliderFloat("Pos##Neon", &p, 0.0f, 1.0f, "%.2f"))
        {
            cfg.neon.colorStops[i].position = p;
        }
        ImGui::SameLine();
        glm::vec4 c = cfg.neon.colorStops[i].color;
        if (ImGui::ColorEdit4("Col##Neon", &c.x, ImGuiColorEditFlags_NoInputs))
        {
            cfg.neon.colorStops[i].color = c;
        }
        ImGui::SameLine();
        if (cfg.neon.colorStops.size() > 1 && ImGui::SmallButton("X"))
        {
            cfg.neon.colorStops.erase(cfg.neon.colorStops.begin() + static_cast<ptrdiff_t>(i));
        }
        ImGui::PopID();
    }

    if (cfg.neon.colorStops.size() < EdgeLighting::NeonConfig::MAX_COLOR_STOPS)
    {
        if (ImGui::Button("+ Add Stop##Neon"))
        {
            float lastPos = cfg.neon.colorStops.empty() ? 0.0f : cfg.neon.colorStops.back().position;
            cfg.neon.colorStops.push_back(
                {std::min(1.0f, lastPos + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
        }
    }
}

void DebugUI::buildMultiPassNeonSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("MultiPass Neon", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Enable##MultiPass", &cfg.multipassNeon.enable);
    ImGui::SameLine();
    ImGui::Checkbox("Show Gradient##MP", &cfg.multipassNeon.showGradient);
    if (!cfg.multipassNeon.enable)
    {
        return;
    }

    ImGui::SliderFloat("Thickness##MP", &cfg.multipassNeon.thickness, 1.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##MP", &cfg.multipassNeon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Size##MP", &cfg.multipassNeon.glowSize, 1.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Speed##MP", &cfg.multipassNeon.speed, 0.0f, 2.0f, "%.2f");

    const char *blendItems[] = {"RGB", "HSV"};
    int blendIdx = static_cast<int>(cfg.multipassNeon.blendSpace);
    if (ImGui::Combo("Blend Space##MP", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
    {
        cfg.multipassNeon.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
    }

    for (size_t i = 0; i < cfg.multipassNeon.colorStops.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i + 100));
        float p = cfg.multipassNeon.colorStops[i].position;
        if (ImGui::SliderFloat("Pos##MP", &p, 0.0f, 1.0f, "%.2f"))
        {
            cfg.multipassNeon.colorStops[i].position = p;
        }
        ImGui::SameLine();
        glm::vec4 c = cfg.multipassNeon.colorStops[i].color;
        if (ImGui::ColorEdit4("Col##MP", &c.x, ImGuiColorEditFlags_NoInputs))
        {
            cfg.multipassNeon.colorStops[i].color = c;
        }
        ImGui::SameLine();
        if (cfg.multipassNeon.colorStops.size() > 1 && ImGui::SmallButton("X"))
        {
            cfg.multipassNeon.colorStops.erase(cfg.multipassNeon.colorStops.begin() + static_cast<ptrdiff_t>(i));
        }
        ImGui::PopID();
    }

    if (cfg.multipassNeon.colorStops.size() < EdgeLighting::MultiPassNeonConfig::MAX_COLOR_STOPS)
    {
        if (ImGui::Button("+ Add Stop##MP"))
        {
            float lastPos = cfg.multipassNeon.colorStops.empty() ? 0.0f : cfg.multipassNeon.colorStops.back().position;
            cfg.multipassNeon.colorStops.push_back(
                {std::min(1.0f, lastPos + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
        }
    }
}
