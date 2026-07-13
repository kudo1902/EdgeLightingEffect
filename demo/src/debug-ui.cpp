#include "debug-ui.h"
#include "core/config.h"
#include "core/edge-lighting.h"
#include "ui-controls.h"
#include "util/log-util.h"
#include "util/screenshot-util.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

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
    buildOptimizedNeonSection(cfg);
    buildColorPickerSection(cfg);
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
    if (cfg.neon.opaque)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("Opaque Color##Neon", &cfg.neon.opaqueColor.x,
                          ImGuiColorEditFlags_NoInputs);
    }
    ImGui::Checkbox("Show Gradient LUT##Neon", &cfg.neon.showGradientLUT);
    ImGui::Checkbox("Show Color Stops##Neon", &cfg.neon.showColorStops);
    ImGui::SliderFloat("Line Width##Neon", &cfg.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Filament Falloff##Neon", &cfg.neon.filamentFalloff, 0.0f, 5.0f, "%.2f");
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

    // --- Travelling segments (zero or more Gaussian brightness peaks) ---
    ImGui::TextDisabled("Segment Boosts (%zu / %d)",
                        cfg.neon.segmentBoosts.size(),
                        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
    for (size_t i = 0; i < cfg.neon.segmentBoosts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(300 + i));
        auto &seg = cfg.neon.segmentBoosts[i];
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Pos##Seg", &seg.position, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Len##Seg", &seg.length, 0.02f, 0.5f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Boost##Seg", &seg.boost, 0.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            cfg.neon.segmentBoosts.erase(cfg.neon.segmentBoosts.begin() +
                                         static_cast<ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (static_cast<int>(cfg.neon.segmentBoosts.size()) <
        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
    {
        if (ImGui::Button("+ Add Segment##Neon"))
        {
            cfg.neon.segmentBoosts.push_back({0.0f, 0.15f, 4.0f});
        }
    }

    // --- Arc gating (0..1 = full perimeter; shrink to "draw" part of the rect) ---
    ImGui::SliderFloat("Arc Start##Neon", &cfg.neon.arcStart, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Arc Length##Neon", &cfg.neon.arcLength, 0.0f, 1.0f, "%.2f");

    const char *blendItems[] = {"RGB", "HSV", "HSL"};
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
    if (cfg.neon.opaque)
    {
        ImGui::SameLine();
        ImGui::ColorEdit3("Opaque Color##Opt", &cfg.neon.opaqueColor.x,
                          ImGuiColorEditFlags_NoInputs);
    }
    ImGui::SliderFloat("Line Width##Opt", &cfg.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Filament Falloff##Opt", &cfg.neon.filamentFalloff, 0.5f, 5.0f, "%.2f");
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

    ImGui::TextDisabled("Segment Boosts (%zu / %d)",
                        cfg.neon.segmentBoosts.size(),
                        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
    for (size_t i = 0; i < cfg.neon.segmentBoosts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(400 + i));
        auto &seg = cfg.neon.segmentBoosts[i];
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Pos##Seg", &seg.position, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Len##Seg", &seg.length, 0.02f, 0.5f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Boost##Seg", &seg.boost, 0.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            cfg.neon.segmentBoosts.erase(cfg.neon.segmentBoosts.begin() +
                                         static_cast<ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (static_cast<int>(cfg.neon.segmentBoosts.size()) <
        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
    {
        if (ImGui::Button("+ Add Segment##Opt"))
        {
            cfg.neon.segmentBoosts.push_back({0.0f, 0.15f, 4.0f});
        }
    }

    ImGui::SliderFloat("Arc Start##Opt", &cfg.neon.arcStart, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Arc Length##Opt", &cfg.neon.arcLength, 0.0f, 1.0f, "%.2f");

    const char *blendItems[] = {"RGB", "HSV", "HSL"};
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
        // into cfg - leaves state unchanged (Playing keeps playing from the
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
        // - those would need a per-subclass params panel to expose here.

        // Speed multiplier - 0 acts as "pause at the value level".
        float speed = anim.GetSpeed();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.0f, 4.0f, "%.2fx"))
        {
            anim.SetSpeed(speed);
        }

        // Playback mode - LOOP wraps elapsed at duration; ONE_SHOT completes
        // after one cycle. Toggling is live: switching a Playing looper to
        // ONE_SHOT will complete on the next Update if elapsed already >= dur.
        int modeIdx = (anim.GetPlaybackMode() == EdgeLighting::PlaybackMode::LOOP)
                          ? 0
                          : 1;
        const char *modeItems[] = {"Loop", "One-shot"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Mode", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
        {
            anim.SetPlaybackMode(modeIdx == 0
                                     ? EdgeLighting::PlaybackMode::LOOP
                                     : EdgeLighting::PlaybackMode::ONE_SHOT);
        }

        // Duration - cycle length in seconds. Subclasses with internal
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

    // Recursively render an AnimationGroup's children as indented sub-rows.
    // Each child gets a full row (state badge, control buttons, Speed / Mode /
    // Duration sliders) so per-child parameters like SegmentTravel's revolution
    // time or IntensityPulse's period are reachable even when the outer group
    // is what the user added. Nested groups keep nesting.
    //
    // Removing a sub-row detaches from the innermost group, leaving siblings
    // intact. The outer preset row keeps its own Remove button for the whole
    // composite.
    void DrawGroupChildren(EdgeLighting::AnimationGroup &group,
                           EdgeLighting::Config &cfg)
    {
        // Iterate a snapshot so an inline Remove from within a child row
        // doesn't invalidate our loop over the group's children vector.
        const auto children = group.GetChildren();
        if (children.empty())
            return;

        ImGui::Indent(18.0f);
        for (size_t i = 0; i < children.size(); ++i)
        {
            char childLabel[48];
            std::snprintf(childLabel, sizeof(childLabel), "Child #%zu", i + 1);
            if (DrawAnimationRow(childLabel, *children[i], cfg,
                                 /*allowRemove=*/true))
            {
                group.Remove(children[i]);
            }
            // Recurse for nested groups.
            if (auto sub = std::dynamic_pointer_cast<EdgeLighting::AnimationGroup>(
                    children[i]))
            {
                DrawGroupChildren(*sub, cfg);
            }
        }
        ImGui::Unindent(18.0f);
    }
}

void DebugUI::buildAnimationSection(EdgeLighting::Config &cfg, float clockTime)
{
    if (!ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // --- Add row: preset combo (no separate Add button - changing the
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
                case EdgeLighting::AnimationState::Playing:
                    stateName = "Playing";
                    break;
                case EdgeLighting::AnimationState::Paused:
                    stateName = "Paused";
                    break;
                case EdgeLighting::AnimationState::Stopped:
                    stateName = "Stopped";
                    break;
                case EdgeLighting::AnimationState::Completed:
                    stateName = "Completed";
                    break;
                }
                LOG_I("Animation '%s' → %s", presetName, stateName);
            };
            // Added animations start Stopped and DON'T touch the config
            // yet - the animated field keeps whatever value it was showing
            // in the sliders. The animation only starts writing when the
            // user clicks Play on the row. (Reset(cfg) is available on the
            // row's Reset button for the "seed baseline before Play" case,
            // but we don't force it here.)
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
            continue; // vector snapshot means the iterator is still valid,
                      // but the child is gone - skip its group-children draw.
        }
        // If this preset is an AnimationGroup (Shimmer, Aurora, …), expose
        // its children as indented sub-rows so the per-child Duration slider
        // is reachable. Non-group animations skip this branch.
        if (auto sub = std::dynamic_pointer_cast<EdgeLighting::AnimationGroup>(
                children[i]))
        {
            DrawGroupChildren(*sub, cfg);
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

void DebugUI::scanColorPickerFiles()
{
    static const std::vector<std::string> exts = {
        ".png",
        ".jpg",
        ".jpeg",
        ".bmp",
        ".tga",
    };
    // Remember the currently selected filename so we can restore it after
    // a refresh, in case its index shifted.
    std::string prev;
    if (mColorPickerSelectedIdx >= 0 &&
        mColorPickerSelectedIdx < static_cast<int>(mColorPickerFiles.size()))
    {
        prev = mColorPickerFiles[mColorPickerSelectedIdx];
    }
    mColorPickerFiles.clear();

    std::error_code ec;
    for (auto &entry : std::filesystem::directory_iterator(RES_DIR, ec))
    {
        if (ec || !entry.is_regular_file())
        {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        if (std::find(exts.begin(), exts.end(), ext) == exts.end())
        {
            continue;
        }
        mColorPickerFiles.push_back(entry.path().filename().string());
    }
    std::sort(mColorPickerFiles.begin(), mColorPickerFiles.end());

    mColorPickerSelectedIdx = 0;
    if (!prev.empty())
    {
        auto it = std::find(mColorPickerFiles.begin(), mColorPickerFiles.end(), prev);
        if (it != mColorPickerFiles.end())
        {
            mColorPickerSelectedIdx =
                static_cast<int>(std::distance(mColorPickerFiles.begin(), it));
        }
    }
}

void DebugUI::buildColorPickerSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Border Color Picker", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::TextDisabled("Pick an image from res/; sample its border into color stops.");

    if (mColorPickerFiles.empty())
    {
        scanColorPickerFiles();
    }

    // File dropdown + Refresh + Load, all on one line.
    ImGui::SetNextItemWidth(-160.0f);
    const char *current =
        (mColorPickerSelectedIdx >= 0 &&
         mColorPickerSelectedIdx < static_cast<int>(mColorPickerFiles.size()))
            ? mColorPickerFiles[mColorPickerSelectedIdx].c_str()
            : "(no images in res/)";
    if (ImGui::BeginCombo("##CPFile", current))
    {
        for (int i = 0; i < static_cast<int>(mColorPickerFiles.size()); ++i)
        {
            bool selected = (i == mColorPickerSelectedIdx);
            if (ImGui::Selectable(mColorPickerFiles[i].c_str(), selected))
            {
                mColorPickerSelectedIdx = i;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh##CP"))
    {
        scanColorPickerFiles();
    }
    ImGui::SameLine();
    const bool canLoad = mColorPickerSelectedIdx >= 0 &&
                         mColorPickerSelectedIdx < static_cast<int>(mColorPickerFiles.size());
    ImGui::BeginDisabled(!canLoad);
    if (ImGui::Button("Load##CP") && canLoad)
    {
        std::string path = std::string(RES_DIR) + "/" +
                           mColorPickerFiles[mColorPickerSelectedIdx];
        if (mColorPicker.Load(path))
        {
            if (!mColorPickerThumb)
            {
                mColorPickerThumb = std::make_unique<EdgeLighting::Texture2D>();
                mColorPickerThumb->SetParams(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
            }
            while (glGetError() != GL_NO_ERROR)
            {
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            mColorPickerThumb->SetData(mColorPicker.Pixels().data(),
                                       mColorPicker.Width(),
                                       mColorPicker.Height());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LOG_E("BorderColorPicker: glTexImage2D(%dx%d) failed: 0x%04x",
                      mColorPicker.Width(), mColorPicker.Height(), err);
            }
        }
    }
    ImGui::EndDisabled();

    if (!mColorPicker.HasImage())
    {
        ImGui::TextDisabled("(no image loaded)");
        return;
    }

    ImGui::Text("Loaded: %dx%d", mColorPicker.Width(), mColorPicker.Height());
    ImGui::Checkbox("Show as backdrop##CP", &mShowImageBackdrop);

    if (mColorPickerThumb && mColorPickerThumb->IsValid())
    {
        const float maxW = 200.0f;
        const float aspect = static_cast<float>(mColorPicker.Height()) /
                             static_cast<float>(mColorPicker.Width());
        const ImVec2 size(maxW, maxW * aspect);
        ImGui::Image(static_cast<ImTextureID>(
                         static_cast<uintptr_t>(mColorPickerThumb->GetId())),
                     size);
    }

    // Cap matches NeonConfig::MAX_COLOR_STOPS (also equals NUM_LOOP_SAMPLES) so
    // the picker can produce one stop per shader loop sample for near-1:1
    // image-to-neon colour reproduction.
    ImGui::SliderInt("Stop Count##CP", &mColorPickerStopCount, 2,
                     EdgeLighting::NeonConfig::MAX_COLOR_STOPS);
    ImGui::TextDisabled("(higher = closer image match)");

    ImGui::SliderFloat("Contrast (gamma)##CP", &mColorPickerGamma, 0.5f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("(1 = linear, >1 darkens shadows)");

    // Preview the sampled stops as a horizontal color strip so the user sees
    // what will be applied before clicking Apply.
    auto stops = mColorPicker.SampleBorder(mColorPickerStopCount,
                                           cfg.geometry.winding,
                                           static_cast<int>(cfg.geometry.width),
                                           static_cast<int>(cfg.geometry.height));

    // Contrast gamma: pow(c, gamma) - dark stops shrink toward 0 while bright
    // stops stay near 1 (0.9^2 = 0.81, 0.06^2 = 0.0036). Applied before Apply
    // so the LUT baked into the shader reflects the compressed range.
    if (std::abs(mColorPickerGamma - 1.0f) > 1e-3f)
    {
        for (auto &s : stops)
        {
            s.color.r = std::pow(std::clamp(s.color.r, 0.0f, 1.0f), mColorPickerGamma);
            s.color.g = std::pow(std::clamp(s.color.g, 0.0f, 1.0f), mColorPickerGamma);
            s.color.b = std::pow(std::clamp(s.color.b, 0.0f, 1.0f), mColorPickerGamma);
        }
    }

    if (!stops.empty())
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float stripW = ImGui::GetContentRegionAvail().x;
        const float stripH = 20.0f;
        const float cellW = stripW / static_cast<float>(stops.size());
        for (size_t i = 0; i < stops.size(); ++i)
        {
            const glm::vec4 &c = stops[i].color;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
            dl->AddRectFilled(ImVec2(p.x + cellW * i, p.y),
                              ImVec2(p.x + cellW * (i + 1), p.y + stripH),
                              col);
        }
        ImGui::Dummy(ImVec2(stripW, stripH));
    }

    // Compute an intensity that keeps the brightest sampled colour near the
    // tone-map knee (pre-tonemap ~3.5, output ~0.85) so dark stops still show
    // as dark. FILAMENT_GAIN in the shader is 12, and the tone map is
    // x / (x + 0.6); solving for `output ≈ 0.85` at brightest gives
    // intensity ≈ 3.5 / (12 * maxChan). Only applied when the auto toggle is
    // on so users who want the classic HDR-neon look can opt out.
    float maxChan = 0.0f;
    for (const auto &s : stops)
    {
        maxChan = std::max(maxChan, std::max({s.color.r, s.color.g, s.color.b}));
    }
    const float autoIntensity = maxChan > 1e-3f
                                    ? std::clamp(3.5f / (12.0f * maxChan), 0.05f, 1.0f)
                                    : 1.0f;

    ImGui::Checkbox("Auto-adjust intensity##CP", &mColorPickerAutoIntensity);
    if (mColorPickerAutoIntensity)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(→ %.2f)", autoIntensity);
    }

    auto applyStops = [&](std::vector<EdgeLighting::ColorStop> &target,
                          float &hueRate, float &intensity)
    {
        target = stops;
        // Applying picker-sampled colours pins each colour to a specific
        // perimeter position, so hue-rotation would drift the halo away.
        hueRate = 0.0f;
        if (mColorPickerAutoIntensity)
        {
            intensity = autoIntensity;
        }
    };

    if (ImGui::Button("Apply to Neon##CP"))
    {
        applyStops(cfg.neon.colorStops, cfg.neon.hueRotationRate, cfg.neon.intensity);
    }
}
