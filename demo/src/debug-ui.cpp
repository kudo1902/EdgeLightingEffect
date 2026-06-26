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
    // Advance the animation-time accumulator regardless of whether we'll
    // actually call Apply this frame — so the clock-time reference stays
    // current even after the animation has completed.
    float dt = clockTime - mLastClockTime;
    mLastClockTime = clockTime;

    if (!mAnimation)
        return;

    // Once a one-shot animation has completed (and the OnComplete callback has
    // fired once), stop writing to config. This lets the user grab sliders for
    // fields the animation was driving (e.g. Arc Start / End after Outline
    // Tracer) and tweak them without the next Apply overwriting the drag.
    //
    // Loopers (PlaybackMode::LOOP) → IsComplete always false → mFiredComplete
    // never gets set, so they keep applying every frame as before.
    if (mFiredComplete)
        return;

    // Speed-scaled accumulator. Changing speed mid-flight only affects future
    // progression — past elapsed isn't recomputed, so the animation continues
    // smoothly from where it was (no fast-forward / rewind jumps).
    mAnimElapsed += dt * mAnimation->GetSpeed();
    mAnimation->Apply(config, mAnimElapsed);

    if (mAnimation->IsComplete(mAnimElapsed))
    {
        mFiredComplete = true;
        if (mAnimation->OnComplete)
            mAnimation->OnComplete();
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

    ImGui::SliderFloat("Line Width##Opt", &cfg.neon.lineWidth, 1.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Opt", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Opt", &cfg.neon.glowRadius, 1.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Opt", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("Hue Rotation Rate##Opt", &cfg.neon.hueRotationRate, 0.0f, 2.0f, "%.2f");

    const char *sideItems[] = {"Both", "Inside", "Outside"};
    int sideIdx = static_cast<int>(cfg.neon.glowSide);
    if (ImGui::Combo("Glow Side##Opt", &sideIdx, sideItems, IM_ARRAYSIZE(sideItems)))
    {
        cfg.neon.glowSide = static_cast<EdgeLighting::GlowSide>(sideIdx);
    }
    if (cfg.neon.glowSide != EdgeLighting::GlowSide::BOTH)
    {
        ImGui::SliderFloat("Side Softness##Opt", &cfg.neon.glowSideSoftness, 0.0f, 20.0f, "%.1f");
    }

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

void DebugUI::buildAnimationSection(EdgeLighting::Config & /*cfg*/, float clockTime)
{
    if (!ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // Combo over every preset name. Picking a new one rebuilds the underlying
    // Animation and snapshots the clock time so each preset starts from t = 0.
    static constexpr int PRESET_COUNT = static_cast<int>(EdgeLightingDemo::AnimationPreset::COUNT);
    const char *names[PRESET_COUNT];
    for (int i = 0; i < PRESET_COUNT; ++i)
    {
        names[i] = EdgeLightingDemo::PresetName(static_cast<EdgeLightingDemo::AnimationPreset>(i));
    }

    int presetIdx = static_cast<int>(mPreset);
    if (ImGui::Combo("Preset", &presetIdx, names, PRESET_COUNT))
    {
        mPreset = static_cast<EdgeLightingDemo::AnimationPreset>(presetIdx);
        mAnimation = EdgeLightingDemo::CreateAnimation(mPreset);
        // Example: log when a one-shot preset finishes. Library users would
        // wire OnComplete to whatever they want (chain into the next anim,
        // hide the effect, etc.).
        if (mAnimation)
        {
            auto presetName = EdgeLightingDemo::PresetName(mPreset);
            mAnimation->OnComplete = [presetName]()
            {
                LOG_I("Animation '%s' completed.", presetName);
            };
        }
        mAnimElapsed = 0.0f;
        mLastClockTime = clockTime;
        mFiredComplete = false;
    }

    if (mAnimation)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart"))
        {
            mAnimElapsed = 0.0f;
            mLastClockTime = clockTime;
            mFiredComplete = false;
        }

        // Speed multiplier — affects future progression only (no fast-forward).
        float speed = mAnimation->GetSpeed();
        if (ImGui::SliderFloat("Speed##Anim", &speed, 0.0f, 4.0f, "%.2fx"))
            mAnimation->SetSpeed(speed);

        float elapsed = mAnimElapsed;
        if (mAnimation->GetPlaybackMode() == EdgeLighting::PlaybackMode::LOOP)
        {
            ImGui::TextDisabled("Active for %.2fs (anim) — loops forever", elapsed);
        }
        else
        {
            float dur = mAnimation->GetDuration();
            bool done = mAnimation->IsComplete(elapsed);
            ImGui::TextDisabled("Active for %.2fs / %.2fs (anim) — %s",
                                elapsed, dur, done ? "complete" : "running");

            // Live duration tweak. Subclasses with internal modulators
            // (FadeIn/FadeOut) rebuild via OnDurationChanged() so the visual
            // fade stretches alongside the completion latch.
            float editable = dur;
            if (ImGui::SliderFloat("Duration##Anim", &editable, 0.05f, 10.0f, "%.2fs"))
            {
                mAnimation->SetDuration(editable);
                // If the new duration is shorter than what's already elapsed,
                // OnComplete is on the verge of firing — clear the latch so it
                // does, even though IsComplete just flipped true.
                if (editable < elapsed)
                {
                    mFiredComplete = false;
                }
            }
        }
        ImGui::TextDisabled("Note: sliders for animated fields will be overwritten each frame.");
    }
}
