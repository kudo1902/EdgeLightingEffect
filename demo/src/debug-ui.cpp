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

    buildStrokeSection(cfg);
    buildGeometrySection(cfg);
    buildPathSection(cfg);
    buildParticlesSection(cfg);

    ImGui::Checkbox("Wireframe", &cfg.wireframe.enable);

    ImGui::Separator();
    ImGui::Text("Animation: %s", playing ? "PLAYING" : "PAUSED");
    if (ImGui::Button(playing ? "Pause" : "Play"))
    {
        if (playing)
            effect.GetAnimation().Pause();
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

void DebugUI::buildStrokeSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Stroke", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::SliderFloat("Thickness", &cfg.stroke.thickness, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat("Intensity", &cfg.stroke.intensity, 0.0f, 1.0f, "%.2f");

    const char *alignItems[] = {"CENTER", "INNER", "OUTER"};
    int alignIdx = static_cast<int>(cfg.stroke.alignment);
    if (ImGui::Combo("Alignment", &alignIdx, alignItems, IM_ARRAYSIZE(alignItems)))
    {
        cfg.stroke.alignment = static_cast<EdgeLighting::StrokeAlignment>(alignIdx);
    }

    const char *animItems[] = {"STATIC", "MOVING", "FLASHING"};
    int animIdx = static_cast<int>(cfg.stroke.animation);
    if (ImGui::Combo("Animation", &animIdx, animItems, IM_ARRAYSIZE(animItems)))
    {
        cfg.stroke.animation = static_cast<EdgeLighting::StrokeAnimation>(animIdx);
    }

    if (cfg.stroke.animation == EdgeLighting::StrokeAnimation::MOVING)
    {
        ImGui::SliderFloat("Segment Len", &cfg.stroke.segmentLength, 0.05f, 1.0f, "%.2f");
        ImGui::SliderInt("Line Count", &cfg.stroke.lineCount, 1, 16);
    }
    ImGui::SliderFloat("Speed", &cfg.stroke.speed, 0.1f, 5.0f, "%.1f");

    const char *blendItems[] = {"RGB", "HSV"};
    int blendIdx = static_cast<int>(cfg.stroke.blendSpace);
    if (ImGui::Combo("Blend Space", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
    {
        cfg.stroke.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
    }

    int toRemove = -1;
    for (int i = 0; i < (int)cfg.stroke.colorStops.size(); i++)
    {
        ImGui::PushID(i);
        ImGui::SliderFloat("Pos", &cfg.stroke.colorStops[i].position, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::ColorEdit3("##col", &cfg.stroke.colorStops[i].color.x,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        if (ImGui::SmallButton("X") && cfg.stroke.colorStops.size() > 1)
            toRemove = i;
        ImGui::PopID();
    }
    if (toRemove >= 0)
        cfg.stroke.colorStops.erase(cfg.stroke.colorStops.begin() + toRemove);
    if (cfg.stroke.colorStops.size() < EdgeLighting::Config::Stroke::MAX_COLOR_STOPS)
    {
        if (ImGui::Button("+ Add Stop"))
        {
            float lastPos = cfg.stroke.colorStops.empty() ? 0.0f : cfg.stroke.colorStops.back().position;
            cfg.stroke.colorStops.push_back(
                {std::min(1.0f, lastPos + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
        }
    }

    ImGui::SliderFloat("Fade Range", &cfg.stroke.fadeRange, 0.0f, cfg.stroke.thickness, "%.1f");

    const char *fadeItems[] = {"SINGLE", "DOUBLE"};
    int fadeIdx = static_cast<int>(cfg.stroke.fadeMode);
    if (ImGui::Combo("Fade Mode", &fadeIdx, fadeItems, IM_ARRAYSIZE(fadeItems)))
    {
        cfg.stroke.fadeMode = static_cast<EdgeLighting::FadeMode>(fadeIdx);
    }

    ImGui::Checkbox("Glow", &cfg.stroke.glowEnable);
    if (cfg.stroke.glowEnable)
    {
        ImGui::SliderFloat("Glow Size", &cfg.stroke.glowSize, 1.0f, 40.0f, "%.0f");
        ImGui::SliderFloat("Glow Int", &cfg.stroke.glowIntensity, 0.0f, 1.0f, "%.2f");
    }
}

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

void DebugUI::buildPathSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Path", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    ImGui::SliderFloat("Start Pos", &cfg.path.startPos, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("End Pos", &cfg.path.endPos, 0.0f, 1.0f, "%.2f");
}

void DebugUI::buildParticlesSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Particles", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Enable", &cfg.particles.enable);
    ImGui::SliderInt("Max Count", &cfg.particles.maxCount, 10, 500);
    ImGui::SliderFloat("Size", &cfg.particles.size, 1.0f, 30.0f, "%.0f");
    ImGui::SliderFloat("Intensity", &cfg.particles.intensity, 0.0f, 1.0f, "%.2f");
}
