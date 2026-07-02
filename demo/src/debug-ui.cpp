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
    ImGui_ImplOpenGL3_Init(GLSL_VERSION);

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

    bool playing = effect.GetClock().IsPlaying();

    ImGui::Begin("Debug Controls");

    // --- Perf readout (always visible, sticky at the top) ---
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("FPS: %.1f  |  %.2f ms (frame)  |  %.2f ms (render)",
                io.Framerate, 1000.0f / io.Framerate, mLastRenderTimeMs);
    ImGui::Separator();

    buildGeometrySection(cfg);
    buildNeonSection(cfg);
    buildMultiPassNeonSection(cfg);
    buildOptimizedNeonSection(cfg);
    buildAnimationSection(cfg, effect.GetClock().GetTime());
    buildBackgroundSection();

    ImGui::Checkbox("Wireframe", &cfg.wireframe.enable);

    ImGui::Separator();
    ImGui::Text("Animation: %s", playing ? "PLAYING" : "PAUSED");
    if (ImGui::Button(playing ? "Pause" : "Play"))
    {
        if (playing)
        {
            effect.GetClock().Pause();
        }
        else
        {
            effect.GetClock().Play();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        effect.GetClock().Stop();
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
        EdgeLightingDemo::PrintCurrentConfig(effect.GetConfig(), effect.GetClock().IsPlaying());
        std::cout << "\n";
    }
    ImGui::End();
}

void DebugUI::ApplyActiveAnimation(EdgeLighting::Config &config, float clockTime)
{
    // Compute the frame delta ourselves: main.cpp still hands us the effect's
    // clock time so we can freeze animations by pausing that clock, but the
    // animation itself now owns state / elapsed / completion latching, so we
    // just forward dt to Update() and call Apply().
    float dt = clockTime - mLastClockTime;
    mLastClockTime = clockTime;

    // AnimationGroup::Update / Apply broadcast to each child, respecting each
    // child's own state (Stopped → skip, Paused → hold, Playing → advance).
    // The shader consumes cfg.neon.hueRotationRate directly via uTime; a
    // preset that modulates the rate (HueRotationReverse etc.) writes into
    // config and the next frame's Render sends the new rate to the shader.
    if (mActiveGroup)
    {
        mActiveGroup->Update(dt);
        mActiveGroup->Apply(config);
    }
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
    {
        return;
    }

    ImGui::SliderFloat("Width", &cfg.geometry.width, 100.0f, 1600.0f, "%.0f");
    ImGui::SliderFloat("Height", &cfg.geometry.height, 100.0f, 1200.0f, "%.0f");
    ImGui::SliderFloat("Pos X", &cfg.geometry.position.x, 0.0f, 1600.0f, "%.0f");
    ImGui::SliderFloat("Pos Y", &cfg.geometry.position.y, 0.0f, 1200.0f, "%.0f");
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

    ImGui::Checkbox("Opaque (no blend)##Neon", &cfg.neon.opaque);
    ImGui::SliderFloat("Line Width##Neon", &cfg.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Neon", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Neon", &cfg.neon.glowRadius, 0.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Neon", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Hue Rotation Rate##Neon", &cfg.neon.hueRotationRate, 0.0f, 2.0f, "%.2f");

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

    // --- Travelling segment (set Boost > 0 to enable) ---
    ImGui::SliderFloat("Segment Pos##Neon", &cfg.neon.segmentPosition, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Segment Length##Neon", &cfg.neon.segmentLength, 0.02f, 0.5f, "%.2f");
    ImGui::SliderFloat("Segment Boost##Neon", &cfg.neon.segmentBoost, 0.0f, 10.0f, "%.1f");

    // --- Arc gating (0..1 = full perimeter; shrink to "draw" part of the rect) ---
    ImGui::SliderFloat("Arc Start##Neon", &cfg.neon.arcStart, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Arc Length##Neon", &cfg.neon.arcLength, 0.0f, 1.0f, "%.2f");

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
    ImGui::Checkbox("Show Perimeter##MP", &cfg.multipassNeon.showPerimeterGradient);
    ImGui::SameLine();
    ImGui::Checkbox("Show Full Grad##MP", &cfg.multipassNeon.showFullGradient);
    if (!cfg.multipassNeon.enable)
    {
        return;
    }

    ImGui::SliderFloat("Line Width##MP", &cfg.multipassNeon.lineWidth, 1.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##MP", &cfg.multipassNeon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##MP", &cfg.multipassNeon.glowRadius, 1.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##MP", &cfg.multipassNeon.bloomStrength, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Hue Rotation Rate##MP", &cfg.multipassNeon.hueRotationRate, 0.0f, 2.0f, "%.2f");

    const char *mpSideItems[] = {"Both", "Inside", "Outside"};
    int mpSideIdx = static_cast<int>(cfg.multipassNeon.glowSide);
    if (ImGui::Combo("Glow Side##MP", &mpSideIdx, mpSideItems, IM_ARRAYSIZE(mpSideItems)))
    {
        cfg.multipassNeon.glowSide = static_cast<EdgeLighting::GlowSide>(mpSideIdx);
    }
    if (cfg.multipassNeon.glowSide != EdgeLighting::GlowSide::BOTH)
    {
        ImGui::SliderFloat("Side Softness##MP", &cfg.multipassNeon.glowSideSoftness, 0.0f, 20.0f, "%.1f");
    }

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

void DebugUI::buildOptimizedNeonSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Optimized Neon (½-res)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Enable##Optimized", &cfg.optimizedNeon.enable);
    if (!cfg.optimizedNeon.enable)
    {
        return;
    }

    ImGui::SameLine();
    ImGui::Checkbox("Show Half-Res##Optimized", &cfg.optimizedNeon.showHalfRes);

    ImGui::SliderFloat("Res Scale##Opt", &cfg.optimizedNeon.resolutionScale, 0.125f, 1.0f, "%.3f");
    ImGui::SliderInt("Samples##Opt", &cfg.optimizedNeon.numSamples, 8, 64);
    ImGui::SliderInt("LUT Size##Opt", &cfg.optimizedNeon.gradientLutSize, 32, 256);

    ImGui::Separator();
    ImGui::TextDisabled("Visual params (shared with Neon)");

    ImGui::Checkbox("Opaque (no blend)##Opt", &cfg.neon.opaque);
    ImGui::SliderFloat("Line Width##Opt", &cfg.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Opt", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Opt", &cfg.neon.glowRadius, 0.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Opt", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Hue Rotation Rate##Opt", &cfg.neon.hueRotationRate, 0.0f, 2.0f, "%.2f");

    const char *sideItems[] = {"Both", "Inside", "Outside"};
    int sideIdx = static_cast<int>(cfg.neon.glowSide);
    if (ImGui::Combo("Glow Side##Opt", &sideIdx, sideItems, IM_ARRAYSIZE(sideItems)))
    {
        cfg.neon.glowSide = static_cast<EdgeLighting::GlowSide>(sideIdx);
    }
    // Always show Side Softness so the control is discoverable regardless of
    // the Glow Side mode (it only feathers the one-sided cut, but keeping it
    // live avoids the slider vanishing when Glow Side is Both).
    ImGui::SliderFloat("Side Softness##Opt", &cfg.neon.glowSideSoftness, 0.0f, 20.0f, "%.1f");

    ImGui::SliderFloat("Segment Pos##Opt", &cfg.neon.segmentPosition, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Segment Length##Opt", &cfg.neon.segmentLength, 0.02f, 0.5f, "%.2f");
    ImGui::SliderFloat("Segment Boost##Opt", &cfg.neon.segmentBoost, 0.0f, 10.0f, "%.1f");

    ImGui::SliderFloat("Arc Start##Opt", &cfg.neon.arcStart, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Arc Length##Opt", &cfg.neon.arcLength, 0.0f, 1.0f, "%.2f");

    const char *blendItems[] = {"RGB", "HSV"};
    int blendIdx = static_cast<int>(cfg.neon.blendSpace);
    if (ImGui::Combo("Blend Space##Opt", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
    {
        cfg.neon.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
    }

    for (size_t i = 0; i < cfg.neon.colorStops.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i + 200));
        float p = cfg.neon.colorStops[i].position;
        if (ImGui::SliderFloat("Pos##Opt", &p, 0.0f, 1.0f, "%.2f"))
        {
            cfg.neon.colorStops[i].position = p;
        }
        ImGui::SameLine();
        glm::vec4 c = cfg.neon.colorStops[i].color;
        if (ImGui::ColorEdit4("Col##Opt", &c.x, ImGuiColorEditFlags_NoInputs))
        {
            cfg.neon.colorStops[i].color = c;
        }
        ImGui::SameLine();
        if (cfg.neon.colorStops.size() > 1 && ImGui::SmallButton("X"))
        {
            cfg.neon.colorStops.erase(
                cfg.neon.colorStops.begin() + static_cast<ptrdiff_t>(i));
        }
        ImGui::PopID();
    }

    if (cfg.neon.colorStops.size() < EdgeLighting::NeonConfig::MAX_COLOR_STOPS)
    {
        if (ImGui::Button("+ Add Stop##Opt"))
        {
            float lastPos = cfg.neon.colorStops.empty()
                                ? 0.0f
                                : cfg.neon.colorStops.back().position;
            cfg.neon.colorStops.push_back(
                {std::min(1.0f, lastPos + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
        }
    }
}

namespace
{
    // Colour-coded label for an animation's current state.
    void DrawStateBadge(EdgeLighting::AnimationState s)
    {
        ImVec4 col;
        const char *label = "?";
        switch (s)
        {
        case EdgeLighting::AnimationState::Playing:
            col = ImVec4(0.30f, 0.85f, 0.35f, 1.0f);
            label = "PLAYING";
            break;
        case EdgeLighting::AnimationState::Paused:
            col = ImVec4(0.95f, 0.75f, 0.15f, 1.0f);
            label = "PAUSED";
            break;
        case EdgeLighting::AnimationState::Stopped:
            col = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
            label = "STOPPED";
            break;
        case EdgeLighting::AnimationState::Completed:
            col = ImVec4(0.40f, 0.70f, 1.00f, 1.0f);
            label = "DONE";
            break;
        }
        ImGui::TextColored(col, "%-7s", label);
    }

    // Render one row of controls for @p anim. Returns true if the user hit
    // the Remove button (caller is responsible for the actual detach).
    // @p allowRemove hides the Remove button for pinned rows (currently
    // there are no pinned rows, but the flag is kept for future use).
    bool DrawAnimationRow(const char *label,
                          EdgeLighting::Animation &anim,
                          EdgeLighting::Config &cfg,
                          bool allowRemove)
    {
        ImGui::PushID(label);
        DrawStateBadge(anim.GetState());
        ImGui::SameLine();
        ImGui::Text("%s", label);

        ImGui::SameLine();
        if (ImGui::SmallButton("Play"))
        {
            anim.Play();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Pause"))
        {
            anim.Pause();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
        {
            anim.Stop();
        }
        ImGui::SameLine();
        // Reset rewinds elapsed to 0 AND writes the modulator@t=0 baseline
        // into cfg — leaves state unchanged (Playing keeps playing from the
        // top; Stopped stays Stopped but the config field is restored).
        if (ImGui::SmallButton("Reset"))
        {
            anim.Reset(cfg);
        }

        bool wantRemove = false;
        if (allowRemove)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                wantRemove = true;
            }
        }

        // --- Per-animation params ---
        // These are the pieces of state Animation's base class exposes to
        // every subclass. Editing them here is a "live tweak" of the added
        // instance; subclass-specific ctor arguments (baseRate, easing,
        // segment length, …) are still baked in at Add-time via the preset
        // — those would need a per-subclass params panel to expose here.

        // Speed multiplier — 0 acts as "pause at the value level".
        float speed = anim.GetSpeed();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.0f, 4.0f, "%.2fx"))
        {
            anim.SetSpeed(speed);
        }

        // Playback mode — LOOP wraps elapsed at duration; ONE_SHOT completes
        // after one cycle. Toggling is live: switching a Playing looper to
        // ONE_SHOT will complete on the next Update if elapsed already >= dur.
        int modeIdx = (anim.GetPlaybackMode() == EdgeLighting::PlaybackMode::LOOP)
                          ? 0 : 1;
        const char *modeItems[] = {"Loop", "One-shot"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Mode", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
        {
            anim.SetPlaybackMode(modeIdx == 0
                                     ? EdgeLighting::PlaybackMode::LOOP
                                     : EdgeLighting::PlaybackMode::ONE_SHOT);
        }

        // Duration — cycle length in seconds. Subclasses with internal
        // modulators (FadeIn/FadeOut/OutlineTracer) rebuild them via
        // OnDurationChanged so the visual matches the completion latch.
        // 0 means "modulator owns its own periodicity" (oscillator-based
        // subclasses), so we disable the slider in that case rather than
        // silently clamping.
        float dur = anim.GetDuration();
        ImGui::SetNextItemWidth(160.0f);
        if (dur > 0.0f)
        {
            float editable = dur;
            if (ImGui::SliderFloat("Duration", &editable, 0.05f, 10.0f, "%.2fs"))
            {
                anim.SetDuration(editable);
            }
        }
        else
        {
            ImGui::BeginDisabled();
            float placeholder = 0.0f;
            ImGui::SliderFloat("Duration", &placeholder, 0.0f, 1.0f,
                               "modulator-owned");
            ImGui::EndDisabled();
        }

        // --- Elapsed status line ---
        float elapsed = anim.GetElapsed();
        if (anim.GetPlaybackMode() == EdgeLighting::PlaybackMode::LOOP)
        {
            if (dur > 0.0f)
            {
                ImGui::TextDisabled("t=%.2fs / cycle=%.2fs (looping)", elapsed, dur);
            }
            else
            {
                ImGui::TextDisabled("t=%.2fs (looping)", elapsed);
            }
        }
        else
        {
            const char *status = anim.IsCompleted() ? "done" : "running";
            ImGui::TextDisabled("t=%.2fs / dur=%.2fs (%s)", elapsed, dur, status);
        }

        ImGui::PopID();
        return wantRemove;
    }
}

void DebugUI::buildAnimationSection(EdgeLighting::Config &cfg, float clockTime)
{
    if (!ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // --- Add row: preset combo (no separate Add button — changing the
    // selection commits immediately, so picking a preset is a single click). ---
    static constexpr int PRESET_COUNT = static_cast<int>(EdgeLightingDemo::AnimationPreset::COUNT);
    const char *names[PRESET_COUNT];
    for (int i = 0; i < PRESET_COUNT; ++i)
    {
        names[i] = EdgeLightingDemo::PresetName(
            static_cast<EdgeLightingDemo::AnimationPreset>(i));
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Add animation", &mAddPresetIdx, names, PRESET_COUNT))
    {
        auto preset = static_cast<EdgeLightingDemo::AnimationPreset>(mAddPresetIdx);
        if (auto anim = EdgeLightingDemo::CreateAnimation(preset))
        {
            const char *presetName = EdgeLightingDemo::PresetName(preset);
            // Log completion + state changes for the added animation. In a
            // real app these hooks would drive UI transitions, chain the
            // next animation, etc.
            anim->OnComplete = [presetName]()
            { LOG_I("Animation '%s' completed.", presetName); };
            anim->OnStateChanged =
                [presetName](EdgeLighting::AnimationState /*prev*/,
                             EdgeLighting::AnimationState now)
            {
                const char *stateName = "?";
                switch (now)
                {
                case EdgeLighting::AnimationState::Playing:   stateName = "Playing"; break;
                case EdgeLighting::AnimationState::Paused:    stateName = "Paused"; break;
                case EdgeLighting::AnimationState::Stopped:   stateName = "Stopped"; break;
                case EdgeLighting::AnimationState::Completed: stateName = "Completed"; break;
                }
                LOG_I("Animation '%s' → %s", presetName, stateName);
            };
            // Added animations start Stopped (Animation's default state).
            // Reset(cfg) writes the modulator's t=0 baseline into the target
            // field so the "before Play" state is the animation's starting
            // frame, not whatever value the config held. User clicks Play in
            // the row to actually start the animation.
            anim->Reset(cfg);
            mActiveGroup->Add(anim);
            // Remember the human-readable name so the row header reads
            // "Breathing" instead of "Animation #3". Parallel vector because
            // AnimationGroup only stores AnimationPtr, not names.
            mActiveNames.push_back(presetName);
        }
        mLastClockTime = clockTime;
    }

    ImGui::Separator();

    // --- Added animation rows ---
    // Iterate a snapshot of the children so removing during iteration is
    // safe (mActiveGroup->Remove(...) invalidates any iterator otherwise).
    const auto children = mActiveGroup->GetChildren();
    if (children.empty())
    {
        ImGui::TextDisabled("No animations added. Pick a preset above to add one.");
    }
    for (size_t i = 0; i < children.size(); ++i)
    {
        const char *presetName = i < mActiveNames.size() ? mActiveNames[i]
                                                          : "Animation";
        char label[80];
        std::snprintf(label, sizeof(label), "%s##%zu", presetName, i);
        if (DrawAnimationRow(label, *children[i], cfg, /*allowRemove=*/true))
        {
            mActiveGroup->Remove(children[i]);
            if (i < mActiveNames.size())
            {
                mActiveNames.erase(mActiveNames.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    ImGui::TextDisabled(
        "Sliders for animated fields will be overwritten each frame.");
}

void DebugUI::buildBackgroundSection()
{
    if (!ImGui::CollapsingHeader("Background (debug)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Show Checker##Bg", &mShowBackground);
    ImGui::TextDisabled("Toggle Neon > 'Opaque' to see blend vs occlude.");
    if (!mShowBackground)
    {
        return;
    }

    ImGui::SliderFloat("Checker Size##Bg", &mBgCheckerSize, 4.0f, 128.0f, "%.0f");
    ImGui::ColorEdit3("Color A##Bg", &mBgColorA.x, ImGuiColorEditFlags_NoInputs);
    ImGui::ColorEdit3("Color B##Bg", &mBgColorB.x, ImGuiColorEditFlags_NoInputs);
}
