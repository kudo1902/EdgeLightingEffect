#include "debug-ui.h"
#include "animation/animation-manager.h"
#include "core/config.h"
#include "core/edge-lighting.h"
#include "renderer/neon-tuning.h"
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

namespace
{
    /// Upper bound the debug UI enforces on colour-stop lists.
    constexpr int MAX_COLOR_STOPS = 128;
    /// Max value for the OptimizedNeon "LUT Size" slider. Matches the LUT
    /// width baked by NeonRenderer (256), which is more than enough for any
    /// gradient the human eye can resolve.
    constexpr int MAX_GRADIENT_LUT_SIZE = 256;

    /// Slider whose knob follows the currently-animated (active) value each
    /// frame so the user sees what the shader is actually drawing. Dragging
    /// still edits the BASE (authored) value: while the user is actively
    /// dragging THIS slider, the display is pinned to the base to avoid a
    /// tug-of-war between the drag and the per-frame animation overlay.
    ///
    /// Detected via @c ImGui::GetActiveID() - we compute the slider's ID
    /// before drawing so we know whether to seed the shown value from base
    /// (dragging) or active (idle / animating). The base is written whenever
    /// the widget reports a change; the animation on the next @c
    /// EdgeLightingEffect::Update then overlays on top of the new base.
    /// Draw one segment-lights row (Pos/Len/Boost + collapsible per-segment
    /// stops editor). Caller wraps in @c PushID so both the Neon and
    /// OptimizedNeon sections can share the same widget IDs without colliding.
    /// @return true if the row's remove button was clicked - caller erases.
    inline bool DrawSegmentRow(EdgeLighting::SegmentBoost &seg)
    {
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Pos##Seg", &seg.position, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Len##Seg", &seg.length, 0.02f, 0.5f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("Boost##Seg", &seg.boost, 0.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("X");

        // Collapsible per-segment stops editor. Header shows the count and an
        // "inherits base" hint when empty (which is the default and means the
        // segment reads its colour from the base gradient at each sample).
        char stopsHdr[64];
        std::snprintf(stopsHdr, sizeof(stopsHdr),
                      "Stops (%zu)%s##SegStops",
                      seg.colorStops.size(),
                      seg.colorStops.empty() ? " - inherits base" : "");
        ImGui::Indent();
        if (ImGui::CollapsingHeader(stopsHdr))
        {
            const char *blendItems[] = {"RGB", "HSV", "HSL"};
            int blendIdx = static_cast<int>(seg.blendSpace);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("Blend##Seg", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
            {
                seg.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
            }
            for (size_t j = 0; j < seg.colorStops.size(); ++j)
            {
                ImGui::PushID(static_cast<int>(j));
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("Pos##SegStop", &seg.colorStops[j].position, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::ColorEdit4("Col##SegStop", &seg.colorStops[j].color.x,
                                  ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    seg.colorStops.erase(seg.colorStops.begin() +
                                         static_cast<ptrdiff_t>(j));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (seg.colorStops.size() < MAX_COLOR_STOPS && ImGui::Button("+ Add Stop##Seg"))
            {
                float lastPos = seg.colorStops.empty() ? 0.0f
                                                       : seg.colorStops.back().position;
                seg.colorStops.push_back(
                    {std::min(1.0f, lastPos + 0.25f), glm::vec4(1.0f)});
            }
        }
        ImGui::Unindent();
        return remove;
    }

    /// Draw one arc row (Start / Length / Intensity + collapsible per-arc
    /// stops editor). Mirrors DrawSegmentRow. Caller wraps in @c PushID so
    /// the Neon and OptimizedNeon sections share widget IDs without collision.
    /// @return true if the row's remove button was clicked - caller erases.
    inline bool DrawArcRow(EdgeLighting::Arc &arc)
    {
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("Start##Arc", &arc.start, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("Len##Arc", &arc.length, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("Int##Arc", &arc.intensity, 0.0f, 4.0f, "%.2f");
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("X");

        // Collapsible per-arc stops editor. Header shows the count and an
        // "inherits base" hint when empty (default = read colour from the
        // base gradient at each sample the arc touches).
        char stopsHdr[64];
        std::snprintf(stopsHdr, sizeof(stopsHdr),
                      "Stops (%zu)%s##ArcStops",
                      arc.colorStops.size(),
                      arc.colorStops.empty() ? " - inherits base" : "");
        ImGui::Indent();
        if (ImGui::CollapsingHeader(stopsHdr))
        {
            const char *blendItems[] = {"RGB", "HSV", "HSL"};
            int blendIdx = static_cast<int>(arc.blendSpace);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("Blend##Arc", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
            {
                arc.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
            }
            for (size_t j = 0; j < arc.colorStops.size(); ++j)
            {
                ImGui::PushID(static_cast<int>(j));
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("Pos##ArcStop", &arc.colorStops[j].position, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::ColorEdit4("Col##ArcStop", &arc.colorStops[j].color.x,
                                  ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    arc.colorStops.erase(arc.colorStops.begin() +
                                         static_cast<ptrdiff_t>(j));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (arc.colorStops.size() < MAX_COLOR_STOPS && ImGui::Button("+ Add Stop##Arc"))
            {
                float lastPos = arc.colorStops.empty() ? 0.0f
                                                       : arc.colorStops.back().position;
                arc.colorStops.push_back(
                    {std::min(1.0f, lastPos + 0.25f), glm::vec4(1.0f)});
            }
        }
        ImGui::Unindent();
        return remove;
    }

    inline bool AnimatedSlider(const char *label, float &baseVal, float activeVal,
                               float minVal, float maxVal, const char *fmt = "%.2f")
    {
        // "Was I actively dragged last frame?" - stored per-slider via ImGui's
        // built-in state storage keyed by the slider's ID. We can't call
        // IsItemActive() BEFORE drawing (there's no item yet), so we consult
        // the previous frame's result to decide what to show, then record this
        // frame's active state for the next call.
        ImGuiStorage *storage = ImGui::GetStateStorage();
        const ImGuiID id = ImGui::GetID(label);
        const bool wasDragging = storage->GetBool(id, false);

        float shown = wasDragging ? baseVal : activeVal;
        const bool changed = ImGui::SliderFloat(label, &shown, minVal, maxVal, fmt);
        const bool isDragging = ImGui::IsItemActive();
        storage->SetBool(id, isDragging);

        if (changed && isDragging)
        {
            baseVal = shown;
        }
        return changed;
    }
}

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

    // The active config carries every animation's current overlay values
    const EdgeLighting::Config &active = effect.GetActiveConfig();
    buildGeometrySection(cfg);
    buildNeonSection(cfg, active);
    buildOptimizedNeonSection(cfg, active);
    buildDropletsSection(cfg);
    buildColorPickerSection(cfg);
    buildAnimationSection(cfg, effect.GetAnimationManager());
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

void DebugUI::buildNeonSection(EdgeLighting::Config &cfg,
                               const EdgeLighting::Config &active)
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
        ImGui::ColorEdit4("Opaque Color##Neon", &cfg.neon.opaqueColor.x,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview);
    }
    ImGui::Checkbox("Show Gradient LUT##Neon", &cfg.neon.showGradientLUT);
    ImGui::Checkbox("Show Color Stops##Neon", &cfg.neon.showColorStops);
    AnimatedSlider("Line Width##Neon", cfg.neon.lineWidth, active.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    AnimatedSlider("Filament Falloff##Neon", cfg.neon.filamentFalloff, active.neon.filamentFalloff, 0.0f, 5.0f);
    AnimatedSlider("Intensity##Neon", cfg.neon.intensity, active.neon.intensity, 0.0f, 3.0f);
    AnimatedSlider("Glow Radius##Neon", cfg.neon.glowRadius, active.neon.glowRadius, 0.0f, 80.0f, "%.0f");
    AnimatedSlider("Bloom Strength##Neon", cfg.neon.bloomStrength, active.neon.bloomStrength, 0.0f, 2.0f);
    AnimatedSlider("Hue Rotation Rate##Neon", cfg.neon.hueRotationRate, active.neon.hueRotationRate, 0.0f, 2.0f);

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

    // --- Travelling segments (independent additive lights on the perimeter) ---
    ImGui::TextDisabled("Segment Lights (%zu / %d) - additive, independent of intensity",
                        cfg.neon.segmentBoosts.size(),
                        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
    for (size_t i = 0; i < cfg.neon.segmentBoosts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(300 + i));
        bool remove = DrawSegmentRow(cfg.neon.segmentBoosts[i]);
        ImGui::PopID();
        if (remove)
        {
            cfg.neon.segmentBoosts.erase(cfg.neon.segmentBoosts.begin() +
                                         static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (static_cast<int>(cfg.neon.segmentBoosts.size()) <
        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
    {
        if (ImGui::Button("+ Add Segment##Neon"))
        {
            cfg.neon.segmentBoosts.push_back({0.0f, 0.15f, 4.0f});
        }
    }

    // --- Arcs (multiple lit slices; winner-take-all in overlap regions) ---
    ImGui::TextDisabled("Arcs (%zu / %d) - overlap resolves winner-take-all",
                        cfg.neon.arcs.size(),
                        EdgeLighting::NeonConfig::MAX_ARCS_CAP);
    for (size_t i = 0; i < cfg.neon.arcs.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(500 + i));
        bool remove = DrawArcRow(cfg.neon.arcs[i]);
        ImGui::PopID();
        if (remove)
        {
            cfg.neon.arcs.erase(cfg.neon.arcs.begin() +
                                static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (static_cast<int>(cfg.neon.arcs.size()) <
        EdgeLighting::NeonConfig::MAX_ARCS_CAP)
    {
        if (ImGui::Button("+ Add Arc##Neon"))
        {
            cfg.neon.arcs.push_back(EdgeLighting::Arc{});
        }
    }

    const char *blendItems[] = {"RGB", "HSV", "HSL"};
    int blendIdx = static_cast<int>(cfg.neon.blendSpace);
    if (ImGui::Combo("Blend Space##Neon", &blendIdx, blendItems, IM_ARRAYSIZE(blendItems)))
    {
        cfg.neon.blendSpace = static_cast<EdgeLighting::BlendSpace>(blendIdx);
    }

    // Cross-fade time when the stop set / blend space changes (0 = instant).
    ImGui::SliderFloat("Color Transition (s)##Neon", &cfg.neon.colorTransitionDuration,
                       0.0f, 2.0f, "%.2f");

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

    if (cfg.neon.colorStops.size() < MAX_COLOR_STOPS)
    {
        if (ImGui::Button("+ Add Stop##Neon"))
        {
            float lastPos = cfg.neon.colorStops.empty() ? 0.0f : cfg.neon.colorStops.back().position;
            cfg.neon.colorStops.push_back(
                {std::min(1.0f, lastPos + 0.1f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
        }
    }
}

void DebugUI::buildOptimizedNeonSection(EdgeLighting::Config &cfg,
                                        const EdgeLighting::Config &active)
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
    ImGui::SliderInt("Samples##Opt", &cfg.optimizedNeon.numSamples, 8, NEON_MAX_LOOP_SAMPLES);
    ImGui::SliderInt("LUT Size##Opt", &cfg.optimizedNeon.gradientLutSize, 32,
                     MAX_GRADIENT_LUT_SIZE);

    ImGui::Separator();
    ImGui::TextDisabled("Visual params (shared with Neon)");

    ImGui::Checkbox("Opaque (no blend)##Opt", &cfg.neon.opaque);
    if (cfg.neon.opaque)
    {
        ImGui::SameLine();
        ImGui::ColorEdit4("Opaque Color##Opt", &cfg.neon.opaqueColor.x,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview);
    }
    AnimatedSlider("Line Width##Opt", cfg.neon.lineWidth, active.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    AnimatedSlider("Filament Falloff##Opt", cfg.neon.filamentFalloff, active.neon.filamentFalloff, 0.5f, 5.0f);
    AnimatedSlider("Intensity##Opt", cfg.neon.intensity, active.neon.intensity, 0.0f, 3.0f);
    AnimatedSlider("Glow Radius##Opt", cfg.neon.glowRadius, active.neon.glowRadius, 0.0f, 80.0f, "%.0f");
    AnimatedSlider("Bloom Strength##Opt", cfg.neon.bloomStrength, active.neon.bloomStrength, 0.0f, 2.0f);
    AnimatedSlider("Hue Rotation Rate##Opt", cfg.neon.hueRotationRate, active.neon.hueRotationRate, 0.0f, 2.0f);

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

    ImGui::TextDisabled("Segment Lights (%zu / %d) - additive, independent of intensity",
                        cfg.neon.segmentBoosts.size(),
                        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
    for (size_t i = 0; i < cfg.neon.segmentBoosts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(400 + i));
        bool remove = DrawSegmentRow(cfg.neon.segmentBoosts[i]);
        ImGui::PopID();
        if (remove)
        {
            cfg.neon.segmentBoosts.erase(cfg.neon.segmentBoosts.begin() +
                                         static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (static_cast<int>(cfg.neon.segmentBoosts.size()) <
        EdgeLighting::NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
    {
        if (ImGui::Button("+ Add Segment##Opt"))
        {
            cfg.neon.segmentBoosts.push_back({0.0f, 0.15f, 4.0f});
        }
    }

    ImGui::TextDisabled("Arcs (%zu / %d) - winner-take-all",
                        cfg.neon.arcs.size(),
                        EdgeLighting::NeonConfig::MAX_ARCS_CAP);
    for (size_t i = 0; i < cfg.neon.arcs.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(600 + i));
        bool remove = DrawArcRow(cfg.neon.arcs[i]);
        ImGui::PopID();
        if (remove)
        {
            cfg.neon.arcs.erase(cfg.neon.arcs.begin() +
                                static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (static_cast<int>(cfg.neon.arcs.size()) <
        EdgeLighting::NeonConfig::MAX_ARCS_CAP)
    {
        if (ImGui::Button("+ Add Arc##Opt"))
        {
            cfg.neon.arcs.push_back(EdgeLighting::Arc{});
        }
    }

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

    if (cfg.neon.colorStops.size() < MAX_COLOR_STOPS)
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
        case EdgeLighting::AnimationState::PLAYING:
            col = ImVec4(0.30f, 0.85f, 0.35f, 1.0f);
            label = "PLAYING";
            break;
        case EdgeLighting::AnimationState::PAUSED:
            col = ImVec4(0.95f, 0.75f, 0.15f, 1.0f);
            label = "PAUSED";
            break;
        case EdgeLighting::AnimationState::STOPPED:
            col = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
            label = "STOPPED";
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

        // End action - what STOPPED-Apply writes to the target field:
        //   Hold current : field settles at wherever elapsed was when stopped. (Default.)
        //   Hold end     : field settles at ApplyAt(cfg, duration).
        //   Hold start   : field settles at ApplyAt(cfg, 0).
        //   Restore      : field settles at the pre-play value (subclass hook).
        // Only meaningful once the animation has played at least once; a
        // freshly-added Stopped animation is a no-op regardless. If you want
        // the base config to show through after Stop, detach the animation.
        const char *endActionItems[] = {
            "Hold current",
            "Hold end",
            "Hold start",
            "Restore",
        };
        int endActionIdx = static_cast<int>(anim.GetEndAction());
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("End action", &endActionIdx,
                         endActionItems, IM_ARRAYSIZE(endActionItems)))
        {
            anim.SetEndAction(static_cast<EdgeLighting::EndAction>(endActionIdx));
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
            const char *status = anim.IsPlaying() ? "running" : "stopped";
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

void DebugUI::buildDropletsSection(EdgeLighting::Config &cfg)
{
    if (!ImGui::CollapsingHeader("Droplets (rain on glass)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Enable##Droplets", &cfg.droplets.enable);
    if (!cfg.droplets.enable)
    {
        return;
    }

    ImGui::SliderFloat("Rain Amount##Droplets", &cfg.droplets.amount, 0.0f, 1.0f);
    ImGui::SliderFloat("Speed##Droplets", &cfg.droplets.speed, 0.0f, 4.0f);
    ImGui::SliderFloat("Scale##Droplets", &cfg.droplets.scale, 0.25f, 4.0f);
    ImGui::SliderFloat("Distortion##Droplets", &cfg.droplets.distortion, 0.0f, 3.0f);
    const char *modeItems[] = {"Wet Glass", "Lens", "Highlights"};
    int modeIdx = static_cast<int>(cfg.droplets.mode);
    if (ImGui::Combo("Mode##Droplets", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
    {
        cfg.droplets.mode = static_cast<EdgeLighting::DropletsMode>(modeIdx);
    }
    ImGui::BeginDisabled(cfg.droplets.mode != EdgeLighting::DropletsMode::WET_GLASS);
    ImGui::SliderFloat("Frost Blur##Droplets", &cfg.droplets.blur, 0.0f, 6.0f);
    ImGui::EndDisabled();
    ImGui::ColorEdit3("Tint##Droplets", &cfg.droplets.tint.x);

    ImGui::TextDisabled("Side follows Neon > Glow Side / Softness");
}

void DebugUI::buildAnimationSection(EdgeLighting::Config &cfg,
                                    EdgeLighting::AnimationManager &manager)
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
            anim->SetName(EdgeLightingDemo::PresetName(preset));

            std::weak_ptr<EdgeLighting::Animation> weakAnim = anim;
            anim->OnComplete = [weakAnim]()
            {
                if (auto a = weakAnim.lock())
                {
                    LOG_I("Animation '%s' completed.", a->GetName().c_str());
                }
            };
            anim->OnStateChanged =
                [weakAnim](EdgeLighting::AnimationState /*prev*/,
                           EdgeLighting::AnimationState now)
            {
                const char *stateName = "?";
                switch (now)
                {
                case EdgeLighting::AnimationState::PLAYING:
                {
                    stateName = "Playing";
                    break;
                }
                case EdgeLighting::AnimationState::PAUSED:
                {
                    stateName = "Paused";
                    break;
                }
                case EdgeLighting::AnimationState::STOPPED:
                {
                    stateName = "Stopped";
                    break;
                }
                }
                if (auto a = weakAnim.lock())
                {
                    LOG_I("Animation '%s' -> %s", a->GetName().c_str(), stateName);
                }
            };
            manager.Attach(anim);
        }
    }

    ImGui::Separator();

    // --- Added animation rows ---
    // Snapshot the manager's attached animations so a Detach during iteration
    // stays safe (Detach erases from the manager's own vector).
    std::vector<EdgeLighting::AnimationPtr> children;
    children.reserve(manager.GetCount());
    for (size_t i = 0; i < manager.GetCount(); ++i)
    {
        children.push_back(manager.GetAnimation(i));
    }
    if (children.empty())
    {
        ImGui::TextDisabled("No animations added. Pick a preset above to add one.");
    }
    for (size_t i = 0; i < children.size(); ++i)
    {
        const std::string &name = children[i]->GetName();
        const char *presetName = name.empty() ? "Animation" : name.c_str();
        char label[80];
        std::snprintf(label, sizeof(label), "%s##%zu", presetName, i);
        if (DrawAnimationRow(label, *children[i], cfg, /*allowRemove=*/true))
        {
            manager.Detach(children[i]);
            continue;
        }
        if (auto sub = std::dynamic_pointer_cast<EdgeLighting::AnimationGroup>(
                children[i]))
        {
            DrawGroupChildren(*sub, cfg);
        }
    }

    ImGui::TextDisabled(
        "Animated fields revert to their base (slider) value on Stop.");
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

    // Cap matches the file-scope MAX_COLOR_STOPS (which mirrors the C ABI's
    // fixed-size array cap) so the picker can produce one stop per shader
    // loop sample for near-1:1 image-to-neon colour reproduction.
    ImGui::SliderInt("Stop Count##CP", &mColorPickerStopCount, 2,
                     MAX_COLOR_STOPS);
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
