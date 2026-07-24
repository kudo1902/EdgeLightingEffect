#include "debug-ui.h"
#include "animation-presets.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace
{
    constexpr int MAX_COLOR_STOPS = 128;
    constexpr int MAX_SEGMENT_BOOSTS = 8;
    constexpr int MAX_ARCS = 8;
    constexpr int MAX_GRADIENT_LUT_SIZE = 256;
    constexpr int MAX_LOOP_SAMPLES = 128;

#if defined(PLATFORM_MACOS)
    constexpr const char *GLSL_VERSION = "#version 330 core";
#else
    constexpr const char *GLSL_VERSION = "#version 300 es";
#endif

    // --- Blend-space combo used by Neon / Optimized / segment / arc rows.
    bool BlendCombo(const char *label, el_blend_space_e &space)
    {
        const char *items[] = {"RGB", "HSV", "HSL"};
        int idx = static_cast<int>(space);
        if (ImGui::Combo(label, &idx, items, IM_ARRAYSIZE(items)))
        {
            space = static_cast<el_blend_space_e>(idx);
            return true;
        }
        return false;
    }

    const char *AnimStateName(el_animation_state_e s)
    {
        switch (s)
        {
        case EL_ANIM_STATE_PLAYING:
        {
            return "PLAYING";
        }
        case EL_ANIM_STATE_PAUSED:
        {
            return "PAUSED";
        }
        case EL_ANIM_STATE_STOPPED:
        {
            return "STOPPED";
        }
        default:
        {
            return "?";
        }
        }
    }

    ImVec4 AnimStateColor(el_animation_state_e s)
    {
        switch (s)
        {
        case EL_ANIM_STATE_PLAYING:
        {
            return ImVec4(0.30f, 0.85f, 0.35f, 1.0f);
        }
        case EL_ANIM_STATE_PAUSED:
        {
            return ImVec4(0.95f, 0.75f, 0.15f, 1.0f);
        }
        case EL_ANIM_STATE_STOPPED:
        {
            return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
        }
        default:
        {
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        }
    }
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DebugUI::~DebugUI()
{
    Shutdown();
}

bool DebugUI::Init(GLFWwindow *mainWindow, int mainW, int /*mainH*/)
{
    int dbgW = 420, dbgH = 700;
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    mWindow = glfwCreateWindow(dbgW, dbgH, "Debug Controls", nullptr, mainWindow);
    if (!mWindow)
    {
        std::fprintf(stderr, "Failed to create debug window\n");
        return false;
    }
    glfwSetWindowPos(mWindow, mainW + 20, 40);
    glfwSetWindowAttrib(mWindow, GLFW_FLOATING, GLFW_TRUE);

    mMainWindow = mainWindow;
    glfwMakeContextCurrent(mainWindow);

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
    for (auto &e : mAnimations)
    {
        if (e.handle)
        {
            el_animation_destroy(e.handle);
        }
    }
    mAnimations.clear();

    if (mColorPickerTex)
    {
        glDeleteTextures(1, &mColorPickerTex);
        mColorPickerTex = 0;
    }

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

void DebugUI::Build(el_effect_handle_t effect)
{
    ImGui::SetCurrentContext(mContext);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    el_bool_t playing = 0;
    el_effect_clock_is_playing(effect, &playing);

    ImGui::Begin("Debug Controls");

    const ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("FPS: %.1f  |  %.2f ms (frame)  |  %.2f ms (render)",
                io.Framerate, 1000.0f / io.Framerate, mLastRenderTimeMs);
    ImGui::Separator();

    buildGeometrySection(effect);
    buildNeonSection(effect);
    buildOptimizedNeonSection(effect);
    buildDropletsSection(effect);
    buildLensFlareSection(effect);
    buildColorPickerSection(effect);
    buildAnimationSection(effect);
    buildBackgroundSection();
    buildWireframeSection(effect);

    ImGui::Separator();
    ImGui::Text("Clock: %s", playing ? "PLAYING" : "PAUSED");
    if (ImGui::Button(playing ? "Pause" : "Play"))
    {
        if (playing)
            el_effect_clock_pause(effect);
        else
            el_effect_clock_play(effect);
    }
    ImGui::End();
}

void DebugUI::Render()
{
    ImGui::Render();
    glfwMakeContextCurrent(mWindow);
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(mWindow, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(mWindow);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void DebugUI::buildGeometrySection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    float w = 0, h = 0, x = 0, y = 0, r = 0;
    el_effect_get_geometry(effect, &w, &h, &x, &y, &r);
    bool changed = false;
    changed |= ImGui::SliderFloat("Width", &w, 100.0f, 1600.0f, "%.0f");
    changed |= ImGui::SliderFloat("Height", &h, 100.0f, 1200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Pos X", &x, 0.0f, 1600.0f, "%.0f");
    changed |= ImGui::SliderFloat("Pos Y", &y, 0.0f, 1200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Corner Radius", &r, 0.0f, 200.0f, "%.0f");
    if (changed)
    {
        el_effect_set_geometry(effect, w, h, x, y, r);
    }

    el_winding_e winding = EL_WINDING_CLOCKWISE;
    el_effect_get_winding(effect, &winding);
    const char *items[] = {"CW", "CCW"};
    int idx = static_cast<int>(winding);
    if (ImGui::Combo("Winding", &idx, items, IM_ARRAYSIZE(items)))
    {
        el_effect_set_winding(effect, static_cast<el_winding_e>(idx));
    }
}

// ---------------------------------------------------------------------------
// Neon
// ---------------------------------------------------------------------------

namespace
{
    // Draw the sliders shared between Neon and Optimized Neon sections. Reads
    // via getters and writes via setters. Suffix distinguishes ID scopes.
    void DrawSharedNeonSliders(el_effect_handle_t effect, const char *idSuffix)
    {
        auto slider = [&](const char *base, auto getFn, auto setFn,
                          float lo, float hi, const char *fmt = "%.2f")
        {
            char label[64];
            std::snprintf(label, sizeof(label), "%s##%s", base, idSuffix);
            float v = 0.0f;
            getFn(effect, &v);
            if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
            {
                setFn(effect, v);
            }
        };

        slider("Line Width", el_effect_get_line_width, el_effect_set_line_width, 0.0f, 20.0f, "%.0f");
        slider("Filament Falloff", el_effect_get_filament_falloff, el_effect_set_filament_falloff, 0.0f, 5.0f);
        slider("Intensity", el_effect_get_intensity, el_effect_set_intensity, 0.0f, 3.0f);
        slider("Glow Radius", el_effect_get_glow_radius, el_effect_set_glow_radius, 0.0f, 80.0f, "%.0f");
        slider("Bloom Strength", el_effect_get_bloom_strength, el_effect_set_bloom_strength, 0.0f, 2.0f);
        slider("Hue Rotation Rate", el_effect_get_hue_rotation_rate, el_effect_set_hue_rotation_rate, 0.0f, 2.0f);

        el_glow_side_e side = EL_GLOW_SIDE_BOTH;
        el_effect_get_glow_side(effect, &side);
        const char *sideItems[] = {"Both", "Inside", "Outside"};
        int sideIdx = static_cast<int>(side);
        char sideLabel[64];
        std::snprintf(sideLabel, sizeof(sideLabel), "Glow Side##%s", idSuffix);
        if (ImGui::Combo(sideLabel, &sideIdx, sideItems, IM_ARRAYSIZE(sideItems)))
        {
            el_effect_set_glow_side(effect, static_cast<el_glow_side_e>(sideIdx));
        }
        float softness = 0.0f;
        el_effect_get_glow_side_softness(effect, &softness);
        char softLabel[64];
        std::snprintf(softLabel, sizeof(softLabel), "Side Softness##%s", idSuffix);
        if (ImGui::SliderFloat(softLabel, &softness, 0.0f, 20.0f, "%.1f"))
        {
            el_effect_set_glow_side_softness(effect, softness);
        }

        auto cutoffRow = [&](const char *base,
                             auto getFn, auto setFn)
        {
            el_bool_t enable = 0;
            float size = 0.0f, soft = 0.0f;
            getFn(effect, &enable, &size, &soft);
            bool en = enable != 0;
            char enableLabel[64], sizeLabel[64], softLabelInner[64];
            std::snprintf(enableLabel, sizeof(enableLabel), "%s##%s", base, idSuffix);
            std::snprintf(sizeLabel, sizeof(sizeLabel), "%s size##%s", base, idSuffix);
            std::snprintf(softLabelInner, sizeof(softLabelInner), "%s softness##%s", base, idSuffix);
            bool changed = ImGui::Checkbox(enableLabel, &en);
            ImGui::Indent();
            if (!en)
                ImGui::BeginDisabled();
            changed |= ImGui::SliderFloat(sizeLabel, &size, 0.0f, 200.0f, "%.0f");
            changed |= ImGui::SliderFloat(softLabelInner, &soft, 0.0f, 20.0f, "%.1f");
            if (!en)
                ImGui::EndDisabled();
            ImGui::Unindent();
            if (changed)
            {
                setFn(effect, en ? 1 : 0, size, soft);
            }
        };
        cutoffRow("Inside Cutoff", el_effect_get_inside_cutoff, el_effect_set_inside_cutoff);
        cutoffRow("Outside Cutoff", el_effect_get_outside_cutoff, el_effect_set_outside_cutoff);
    }

    // Segment boost row: reads/writes one segment through capi accessors.
    // Returns true if the row's Remove button was clicked (caller handles the
    // shift-down + count-shrink).
    bool DrawSegmentRow(el_effect_handle_t effect, int32_t segIdx, const char *idSuffix)
    {
        float p = 0, l = 0, b = 0;
        el_effect_get_segment_boost(effect, segIdx, &p, &l, &b);

        char id[32];
        std::snprintf(id, sizeof(id), "##Seg%s%d", idSuffix, segIdx);

        ImGui::PushID(segIdx);
        ImGui::SetNextItemWidth(90.0f);
        bool changed = ImGui::SliderFloat((std::string("Pos") + id).c_str(), &p, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat((std::string("Len") + id).c_str(), &l, 0.02f, 0.5f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat((std::string("Boost") + id).c_str(), &b, 0.0f, 10.0f, "%.1f");
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("X");

        if (changed)
        {
            el_effect_set_segment_boost(effect, segIdx, p, l, b);
        }

        // Per-segment stops + blend space
        char header[64];
        int32_t stopCount = 0;
        el_effect_get_segment_color_stop_count(effect, segIdx, &stopCount);
        std::snprintf(header, sizeof(header),
                      "Stops (%d)%s##SegHdr%s%d",
                      stopCount, stopCount == 0 ? " - inherits base" : "", idSuffix, segIdx);

        ImGui::Indent();
        if (ImGui::CollapsingHeader(header))
        {
            el_blend_space_e blend = EL_BLEND_SPACE_RGB;
            el_effect_get_segment_blend_space(effect, segIdx, &blend);
            ImGui::SetNextItemWidth(120.0f);
            char blendLabel[64];
            std::snprintf(blendLabel, sizeof(blendLabel), "Blend##SegBlend%s%d", idSuffix, segIdx);
            if (BlendCombo(blendLabel, blend))
            {
                el_effect_set_segment_blend_space(effect, segIdx, blend);
            }
            for (int32_t j = 0; j < stopCount; ++j)
            {
                ImGui::PushID(j);
                float sp = 0, sr = 0, sg = 0, sb = 0, sa = 0;
                el_effect_get_segment_color_stop(effect, segIdx, j, &sp, &sr, &sg, &sb, &sa);
                bool sc = false;
                ImGui::SetNextItemWidth(90.0f);
                sc |= ImGui::SliderFloat("Pos##SegStop", &sp, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                float col[4] = {sr, sg, sb, sa};
                sc |= ImGui::ColorEdit4("Col##SegStop", col, ImGuiColorEditFlags_NoInputs);
                if (sc)
                {
                    el_effect_set_segment_color_stop(effect, segIdx, j, sp, col[0], col[1], col[2], col[3]);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    // Shift-down + shrink for one entry.
                    for (int32_t k = j; k < stopCount - 1; ++k)
                    {
                        float p2 = 0, r2 = 0, g2 = 0, b2 = 0, a2 = 0;
                        el_effect_get_segment_color_stop(effect, segIdx, k + 1, &p2, &r2, &g2, &b2, &a2);
                        el_effect_set_segment_color_stop(effect, segIdx, k, p2, r2, g2, b2, a2);
                    }
                    el_effect_set_segment_color_stop_count(effect, segIdx, stopCount - 1);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            char addLabel[64];
            std::snprintf(addLabel, sizeof(addLabel), "+ Add Stop##SegAdd%s%d", idSuffix, segIdx);
            if (stopCount < MAX_COLOR_STOPS && ImGui::Button(addLabel))
            {
                float lastPos = 0.0f;
                if (stopCount > 0)
                {
                    float lp = 0, lr = 0, lg = 0, lb = 0, la = 0;
                    el_effect_get_segment_color_stop(effect, segIdx, stopCount - 1, &lp, &lr, &lg, &lb, &la);
                    lastPos = lp;
                }
                el_effect_set_segment_color_stop_count(effect, segIdx, stopCount + 1);
                el_effect_set_segment_color_stop(effect, segIdx, stopCount,
                                                 std::min(1.0f, lastPos + 0.25f),
                                                 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
        return remove;
    }

    bool DrawArcRow(el_effect_handle_t effect, int32_t arcIdx, const char *idSuffix)
    {
        float st = 0, ln = 0, in = 0;
        el_blend_space_e blend = EL_BLEND_SPACE_RGB;
        el_effect_get_arc(effect, arcIdx, &st, &ln, &in, &blend);

        char id[32];
        std::snprintf(id, sizeof(id), "##Arc%s%d", idSuffix, arcIdx);

        ImGui::PushID(arcIdx);
        ImGui::SetNextItemWidth(80.0f);
        bool changed = ImGui::SliderFloat((std::string("Start") + id).c_str(), &st, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        changed |= ImGui::SliderFloat((std::string("Len") + id).c_str(), &ln, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        changed |= ImGui::SliderFloat((std::string("Int") + id).c_str(), &in, 0.0f, 4.0f, "%.2f");
        ImGui::SameLine();
        bool remove = ImGui::SmallButton("X");

        if (changed)
        {
            el_effect_set_arc(effect, arcIdx, st, ln, in, blend);
        }

        int32_t stopCount = 0;
        el_effect_get_arc_color_stop_count(effect, arcIdx, &stopCount);
        char header[64];
        std::snprintf(header, sizeof(header),
                      "Stops (%d)%s##ArcHdr%s%d",
                      stopCount, stopCount == 0 ? " - inherits base" : "", idSuffix, arcIdx);

        ImGui::Indent();
        if (ImGui::CollapsingHeader(header))
        {
            ImGui::SetNextItemWidth(120.0f);
            char blendLabel[64];
            std::snprintf(blendLabel, sizeof(blendLabel), "Blend##ArcBlend%s%d", idSuffix, arcIdx);
            if (BlendCombo(blendLabel, blend))
            {
                el_effect_set_arc(effect, arcIdx, st, ln, in, blend);
            }
            for (int32_t j = 0; j < stopCount; ++j)
            {
                ImGui::PushID(j);
                float sp = 0, sr = 0, sg = 0, sb = 0, sa = 0;
                el_effect_get_arc_color_stop(effect, arcIdx, j, &sp, &sr, &sg, &sb, &sa);
                bool sc = false;
                ImGui::SetNextItemWidth(90.0f);
                sc |= ImGui::SliderFloat("Pos##ArcStop", &sp, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                float col[4] = {sr, sg, sb, sa};
                sc |= ImGui::ColorEdit4("Col##ArcStop", col, ImGuiColorEditFlags_NoInputs);
                if (sc)
                {
                    el_effect_set_arc_color_stop(effect, arcIdx, j, sp, col[0], col[1], col[2], col[3]);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    for (int32_t k = j; k < stopCount - 1; ++k)
                    {
                        float p2 = 0, r2 = 0, g2 = 0, b2 = 0, a2 = 0;
                        el_effect_get_arc_color_stop(effect, arcIdx, k + 1, &p2, &r2, &g2, &b2, &a2);
                        el_effect_set_arc_color_stop(effect, arcIdx, k, p2, r2, g2, b2, a2);
                    }
                    el_effect_set_arc_color_stop_count(effect, arcIdx, stopCount - 1);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            char addLabel[64];
            std::snprintf(addLabel, sizeof(addLabel), "+ Add Stop##ArcAdd%s%d", idSuffix, arcIdx);
            if (stopCount < MAX_COLOR_STOPS && ImGui::Button(addLabel))
            {
                float lastPos = 0.0f;
                if (stopCount > 0)
                {
                    float lp = 0, lr = 0, lg = 0, lb = 0, la = 0;
                    el_effect_get_arc_color_stop(effect, arcIdx, stopCount - 1, &lp, &lr, &lg, &lb, &la);
                    lastPos = lp;
                }
                el_effect_set_arc_color_stop_count(effect, arcIdx, stopCount + 1);
                el_effect_set_arc_color_stop(effect, arcIdx, stopCount,
                                             std::min(1.0f, lastPos + 0.25f),
                                             1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
        return remove;
    }

    // Removes segment @p removeIdx by shifting later entries down then shrinking count.
    void RemoveSegment(el_effect_handle_t effect, int32_t removeIdx, int32_t count)
    {
        for (int32_t k = removeIdx; k < count - 1; ++k)
        {
            float p = 0, l = 0, b = 0;
            el_effect_get_segment_boost(effect, k + 1, &p, &l, &b);
            el_effect_set_segment_boost(effect, k, p, l, b);
        }
        el_effect_set_segment_boost_count(effect, count - 1);
    }

    void RemoveArc(el_effect_handle_t effect, int32_t removeIdx, int32_t count)
    {
        for (int32_t k = removeIdx; k < count - 1; ++k)
        {
            float st = 0, ln = 0, in = 0;
            el_blend_space_e bl = EL_BLEND_SPACE_RGB;
            el_effect_get_arc(effect, k + 1, &st, &ln, &in, &bl);
            el_effect_set_arc(effect, k, st, ln, in, bl);
        }
        el_effect_set_arc_count(effect, count - 1);
    }

    void DrawSegmentAndArcRows(el_effect_handle_t effect, const char *idSuffix)
    {
        int32_t segCount = 0;
        el_effect_get_segment_boost_count(effect, &segCount);
        ImGui::TextDisabled("Segment Lights (%d / %d) - additive",
                            segCount, MAX_SEGMENT_BOOSTS);
        for (int32_t i = 0; i < segCount; ++i)
        {
            if (DrawSegmentRow(effect, i, idSuffix))
            {
                RemoveSegment(effect, i, segCount);
                break;
            }
        }
        char addSegLabel[64];
        std::snprintf(addSegLabel, sizeof(addSegLabel), "+ Add Segment##%s", idSuffix);
        if (segCount < MAX_SEGMENT_BOOSTS && ImGui::Button(addSegLabel))
        {
            el_effect_set_segment_boost_count(effect, segCount + 1);
            el_effect_set_segment_boost(effect, segCount, 0.0f, 0.15f, 4.0f);
        }

        int32_t arcCount = 0;
        el_effect_get_arc_count(effect, &arcCount);
        ImGui::TextDisabled("Arcs (%d / %d) - winner-take-all", arcCount, MAX_ARCS);
        for (int32_t i = 0; i < arcCount; ++i)
        {
            if (DrawArcRow(effect, i, idSuffix))
            {
                RemoveArc(effect, i, arcCount);
                break;
            }
        }
        char addArcLabel[64];
        std::snprintf(addArcLabel, sizeof(addArcLabel), "+ Add Arc##%s", idSuffix);
        if (arcCount < MAX_ARCS && ImGui::Button(addArcLabel))
        {
            el_effect_set_arc_count(effect, arcCount + 1);
            el_effect_set_arc(effect, arcCount, 0.0f, 0.25f, 1.0f, EL_BLEND_SPACE_RGB);
        }
    }

    void DrawColorStopsList(el_effect_handle_t effect, const char *idSuffix)
    {
        int32_t stopCount = 0;
        el_effect_get_color_stop_count(effect, &stopCount);
        for (int32_t i = 0; i < stopCount; ++i)
        {
            ImGui::PushID(i);
            float p = 0, r = 0, g = 0, b = 0, a = 0;
            el_effect_get_color_stop(effect, i, &p, &r, &g, &b, &a);
            bool changed = false;
            char posLabel[64];
            std::snprintf(posLabel, sizeof(posLabel), "Pos##%s", idSuffix);
            changed |= ImGui::SliderFloat(posLabel, &p, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            float col[4] = {r, g, b, a};
            char colLabel[64];
            std::snprintf(colLabel, sizeof(colLabel), "Col##%s", idSuffix);
            changed |= ImGui::ColorEdit4(colLabel, col, ImGuiColorEditFlags_NoInputs);
            if (changed)
            {
                el_effect_set_color_stop(effect, i, p, col[0], col[1], col[2], col[3]);
            }
            ImGui::SameLine();
            if (stopCount > 1 && ImGui::SmallButton("X"))
            {
                for (int32_t k = i; k < stopCount - 1; ++k)
                {
                    float p2 = 0, r2 = 0, g2 = 0, b2 = 0, a2 = 0;
                    el_effect_get_color_stop(effect, k + 1, &p2, &r2, &g2, &b2, &a2);
                    el_effect_set_color_stop(effect, k, p2, r2, g2, b2, a2);
                }
                el_effect_set_color_stop_count(effect, stopCount - 1);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        char addLabel[64];
        std::snprintf(addLabel, sizeof(addLabel), "+ Add Stop##%s", idSuffix);
        if (stopCount < MAX_COLOR_STOPS && ImGui::Button(addLabel))
        {
            float lastPos = 0.0f;
            if (stopCount > 0)
            {
                float lp = 0, lr = 0, lg = 0, lb = 0, la = 0;
                el_effect_get_color_stop(effect, stopCount - 1, &lp, &lr, &lg, &lb, &la);
                lastPos = lp;
            }
            el_effect_set_color_stop_count(effect, stopCount + 1);
            el_effect_set_color_stop(effect, stopCount,
                                     std::min(1.0f, lastPos + 0.1f),
                                     1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
} // namespace

void DebugUI::buildNeonSection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Neon", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    el_bool_t enable = 0;
    el_effect_get_neon_renderer_enabled(effect, &enable);
    bool en = enable;
    if (ImGui::Checkbox("Enable##Neon", &en))
    {
        el_effect_set_neon_renderer_enabled(effect, en ? 1 : 0);
    }
    if (!en)
        return;

    el_opaque_mode_e opaqueMode = EL_OPAQUE_MODE_NONE;
    el_effect_get_opaque_mode(effect, &opaqueMode);
    const char *opaqueItems[] = {"None", "Outside", "Inside", "Both", "All"};
    int opaqueIdx = static_cast<int>(opaqueMode);
    if (ImGui::Combo("Opaque##Neon", &opaqueIdx, opaqueItems, IM_ARRAYSIZE(opaqueItems)))
    {
        el_effect_set_opaque_mode(effect, static_cast<el_opaque_mode_e>(opaqueIdx));
    }
    if (opaqueMode != EL_OPAQUE_MODE_NONE)
    {
        float r = 0, g = 0, b = 0, a = 0;
        el_effect_get_opaque_color(effect, &r, &g, &b, &a);
        float col[4] = {r, g, b, a};
        ImGui::SameLine();
        if (ImGui::ColorEdit4("Opaque Color##Neon", col,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview))
        {
            el_effect_set_opaque_color(effect, col[0], col[1], col[2], col[3]);
        }
        float opaqueSoftness = 0.0f;
        el_effect_get_opaque_softness(effect, &opaqueSoftness);
        if (ImGui::SliderFloat("Opaque Softness##Neon", &opaqueSoftness, 0.0f, 20.0f, "%.1f"))
        {
            el_effect_set_opaque_softness(effect, opaqueSoftness);
        }
    }

    el_bool_t showLut = 0;
    el_effect_get_show_gradient_lut(effect, &showLut);
    bool sl = showLut;
    if (ImGui::Checkbox("Show Gradient LUT##Neon", &sl))
    {
        el_effect_set_show_gradient_lut(effect, sl ? 1 : 0);
    }
    el_bool_t showStops = 0;
    el_effect_get_show_color_stops(effect, &showStops);
    bool ss = showStops;
    if (ImGui::Checkbox("Show Color Stops##Neon", &ss))
    {
        el_effect_set_show_color_stops(effect, ss ? 1 : 0);
    }

    DrawSharedNeonSliders(effect, "Neon");
    DrawSegmentAndArcRows(effect, "Neon");

    el_blend_space_e blend = EL_BLEND_SPACE_RGB;
    el_effect_get_blend_space(effect, &blend);
    if (BlendCombo("Blend Space##Neon", blend))
    {
        el_effect_set_blend_space(effect, blend);
    }

    float transDur = 0.0f;
    el_effect_get_color_transition_duration(effect, &transDur);
    if (ImGui::SliderFloat("Color Transition (s)##Neon", &transDur, 0.0f, 2.0f, "%.2f"))
    {
        el_effect_set_color_transition_duration(effect, transDur);
    }

    DrawColorStopsList(effect, "Neon");
}

void DebugUI::buildOptimizedNeonSection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Optimized Neon (1/2-res)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    el_bool_t on = 0;
    el_effect_get_optimized_renderer_enabled(effect, &on);
    bool en = on;
    if (ImGui::Checkbox("Enable##Opt", &en))
    {
        el_effect_set_optimized_renderer_enabled(effect, en ? 1 : 0);
    }
    if (!en)
        return;

    el_bool_t showHalf = 0;
    el_effect_get_optimized_show_half_res(effect, &showHalf);
    bool sh = showHalf;
    ImGui::SameLine();
    if (ImGui::Checkbox("Show Half-Res##Opt", &sh))
    {
        el_effect_set_optimized_show_half_res(effect, sh ? 1 : 0);
    }

    float scale = 0.0f;
    el_effect_get_optimized_resolution_scale(effect, &scale);
    if (ImGui::SliderFloat("Res Scale##Opt", &scale, 0.125f, 1.0f, "%.3f"))
    {
        el_effect_set_optimized_resolution_scale(effect, scale);
    }

    int32_t samples = 0;
    el_effect_get_optimized_num_samples(effect, &samples);
    if (ImGui::SliderInt("Samples##Opt", &samples, 8, MAX_LOOP_SAMPLES))
    {
        el_effect_set_optimized_num_samples(effect, samples);
    }

    int32_t lutSize = 0;
    el_effect_get_optimized_gradient_lut_size(effect, &lutSize);
    if (ImGui::SliderInt("LUT Size##Opt", &lutSize, 32, MAX_GRADIENT_LUT_SIZE))
    {
        el_effect_set_optimized_gradient_lut_size(effect, lutSize);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Visual params (shared with Neon)");

    DrawSharedNeonSliders(effect, "Opt");
    DrawSegmentAndArcRows(effect, "Opt");

    el_blend_space_e blend = EL_BLEND_SPACE_RGB;
    el_effect_get_blend_space(effect, &blend);
    if (BlendCombo("Blend Space##Opt", blend))
    {
        el_effect_set_blend_space(effect, blend);
    }

    DrawColorStopsList(effect, "Opt");
}

// ---------------------------------------------------------------------------
// Droplets
// ---------------------------------------------------------------------------

void DebugUI::buildDropletsSection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Droplets (rain on glass)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    el_bool_t on = 0;
    el_effect_get_droplets_renderer_enabled(effect, &on);
    bool en = on;
    if (ImGui::Checkbox("Enable##Drop", &en))
    {
        el_effect_set_droplets_renderer_enabled(effect, en ? 1 : 0);
    }
    if (!en)
        return;

    float amount = 0.0f;
    el_effect_get_droplets_amount(effect, &amount);
    if (ImGui::SliderFloat("Rain Amount##Drop", &amount, 0.0f, 1.0f))
    {
        el_effect_set_droplets_amount(effect, amount);
    }

    float speed = 0.0f;
    el_effect_get_droplets_speed(effect, &speed);
    if (ImGui::SliderFloat("Speed##Drop", &speed, 0.0f, 4.0f))
    {
        el_effect_set_droplets_speed(effect, speed);
    }

    int lanes = 1;
    el_effect_get_droplets_lanes(effect, &lanes);
    if (ImGui::SliderInt("Lanes##Drop", &lanes, 1, 6))
    {
        el_effect_set_droplets_lanes(effect, lanes);
    }

    float tint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    el_effect_get_droplets_tint(effect, &tint[0], &tint[1], &tint[2], &tint[3]);
    if (ImGui::ColorEdit4("Tint##Drop", tint,
                          ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview))
    {
        el_effect_set_droplets_tint(effect, tint[0], tint[1], tint[2], tint[3]);
    }

    ImGui::TextDisabled("Side follows Neon > Glow Side / Softness");
}

// ---------------------------------------------------------------------------
// Lens flare
// ---------------------------------------------------------------------------

void DebugUI::buildLensFlareSection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Lens Flare", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    el_bool_t on = 0;
    el_effect_get_lens_flare_renderer_enabled(effect, &on);
    bool en = on;
    if (ImGui::Checkbox("Enable##Lens", &en))
    {
        el_effect_set_lens_flare_renderer_enabled(effect, en ? 1 : 0);
    }
    if (!en)
        return;

    float pos = 0.0f;
    el_effect_get_lens_flare_perimeter_position(effect, &pos);
    if (ImGui::SliderFloat("Perimeter Pos##Lens", &pos, 0.0f, 1.0f, "%.3f"))
    {
        el_effect_set_lens_flare_perimeter_position(effect, pos);
    }

    float offset = 0.0f;
    el_effect_get_lens_flare_perimeter_offset(effect, &offset);
    if (ImGui::SliderFloat("Perimeter Offset##Lens", &offset, -500.0f, 500.0f, "%.1f px"))
    {
        el_effect_set_lens_flare_perimeter_offset(effect, offset);
    }

    float size = 1.0f;
    el_effect_get_lens_flare_size(effect, &size);
    if (ImGui::SliderFloat("Size##Lens", &size, 0.1f, 5.0f, "%.2f"))
    {
        el_effect_set_lens_flare_size(effect, size);
    }

    float color[4] = {0, 0, 0, 0};
    el_effect_get_lens_flare_color(effect, &color[0], &color[1], &color[2], &color[3]);
    if (ImGui::ColorEdit4("Color##Lens", color,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_HDR |
                              ImGuiColorEditFlags_Float))
    {
        el_effect_set_lens_flare_color(effect, color[0], color[1], color[2], color[3]);
    }

    float intensity = 1.0f;
    el_effect_get_lens_flare_intensity(effect, &intensity);
    if (ImGui::SliderFloat("Intensity##Lens", &intensity, 0.0f, 4.0f, "%.2f"))
    {
        el_effect_set_lens_flare_intensity(effect, intensity);
    }

    float spread = 1.0f;
    el_effect_get_lens_flare_spread(effect, &spread);
    if (ImGui::SliderFloat("Spread##Lens", &spread, 0.0f, 3.0f, "%.2f"))
    {
        el_effect_set_lens_flare_spread(effect, spread);
    }

    float ghostSpacing = 1.0f;
    el_effect_get_lens_flare_ghost_spacing(effect, &ghostSpacing);
    if (ImGui::SliderFloat("Ghost Spacing##Lens", &ghostSpacing, 0.1f, 4.0f, "%.2f"))
    {
        el_effect_set_lens_flare_ghost_spacing(effect, ghostSpacing);
    }

    float ghostSize = 2.2f;
    el_effect_get_lens_flare_ghost_size(effect, &ghostSize);
    if (ImGui::SliderFloat("Ghost Size##Lens", &ghostSize, 1.0f, 5.0f, "%.2f"))
    {
        el_effect_set_lens_flare_ghost_size(effect, ghostSize);
    }

    float ghostOffset = -1.5f;
    el_effect_get_lens_flare_ghost_offset(effect, &ghostOffset);
    if (ImGui::SliderFloat("Ghost Offset##Lens", &ghostOffset, -4.0f, 3.0f, "%.2f"))
    {
        el_effect_set_lens_flare_ghost_offset(effect, ghostOffset);
    }

    float ghostColor[3] = {1.0f, 1.0f, 1.0f};
    el_effect_get_lens_flare_ghost_color(effect, &ghostColor[0], &ghostColor[1], &ghostColor[2]);
    if (ImGui::ColorEdit3("Ghost Color##Lens", ghostColor))
    {
        el_effect_set_lens_flare_ghost_color(effect, ghostColor[0], ghostColor[1], ghostColor[2]);
    }

    float ghostTint = 0.0f;
    el_effect_get_lens_flare_ghost_tint(effect, &ghostTint);
    if (ImGui::SliderFloat("Ghost Tint##Lens", &ghostTint, 0.0f, 1.0f, "%.2f"))
    {
        el_effect_set_lens_flare_ghost_tint(effect, ghostTint);
    }

    float rayDensity = 0.25f;
    el_effect_get_lens_flare_ray_density(effect, &rayDensity);
    if (ImGui::SliderFloat("Ray Density##Lens", &rayDensity, 0.0f, 1.0f, "%.2f"))
    {
        el_effect_set_lens_flare_ray_density(effect, rayDensity);
    }

    float rotationRate = 0.0f;
    el_effect_get_lens_flare_rotation_rate(effect, &rotationRate);
    if (ImGui::SliderFloat("Rotation Rate##Lens", &rotationRate, -2.0f, 2.0f, "%.3f rev/s"))
    {
        el_effect_set_lens_flare_rotation_rate(effect, rotationRate);
    }

    el_bool_t ghostsFollowRotation = 1;
    el_effect_get_lens_flare_ghosts_follow_rotation(effect, &ghostsFollowRotation);
    bool follow = ghostsFollowRotation != 0;
    if (ImGui::Checkbox("Ghosts Follow Rotation##Lens", &follow))
    {
        el_effect_set_lens_flare_ghosts_follow_rotation(effect, follow ? 1 : 0);
    }

    float ghostRotOffset = 0.0f;
    el_effect_get_lens_flare_ghost_rotation_offset(effect, &ghostRotOffset);
    if (ImGui::SliderFloat("Ghost Rot Offset##Lens", &ghostRotOffset, -180.0f, 180.0f, "%.1f deg"))
    {
        el_effect_set_lens_flare_ghost_rotation_offset(effect, ghostRotOffset);
    }
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

void DebugUI::buildAnimationSection(el_effect_handle_t effect)
{
    if (!ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    // Add row: picking a preset commits immediately (matches the C++ demo).
    constexpr int PRESET_COUNT = EdgeLightingCapiDemo::PRESET_LAST + 1;
    const char *names[PRESET_COUNT];
    for (int i = 0; i < PRESET_COUNT; ++i)
    {
        names[i] = EdgeLightingCapiDemo::PresetName(static_cast<el_animation_preset_e>(i));
    }
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Add animation", &mAddPresetIdx, names, PRESET_COUNT))
    {
        auto preset = static_cast<el_animation_preset_e>(mAddPresetIdx);
        if (preset != EL_ANIM_NONE)
        {
            if (el_animation_handle_t h = el_animation_create(preset))
            {
                el_effect_attach_animation(effect, h);
                mAnimations.push_back({h, EdgeLightingCapiDemo::PresetName(preset)});
            }
        }
    }
    ImGui::Separator();

    if (mAnimations.empty())
    {
        ImGui::TextDisabled("No animations added. Pick a preset above to add one.");
    }

    for (size_t i = 0; i < mAnimations.size(); ++i)
    {
        auto &entry = mAnimations[i];
        el_animation_handle_t h = entry.handle;

        ImGui::PushID(static_cast<int>(i));

        el_animation_state_e state = EL_ANIM_STATE_STOPPED;
        el_animation_get_state(h, &state);
        ImGui::TextColored(AnimStateColor(state), "%-7s", AnimStateName(state));
        ImGui::SameLine();
        ImGui::Text("%s", entry.name.c_str());

        ImGui::SameLine();
        if (ImGui::SmallButton("Play"))
        {
            el_animation_play(h);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Pause"))
        {
            el_animation_pause(h);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
        {
            el_animation_stop(h);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset"))
        {
            el_animation_reset(h, effect);
        }
        ImGui::SameLine();
        bool wantRemove = ImGui::SmallButton("Remove");

        // Per-animation params
        float speed = 1.0f;
        el_animation_get_speed(h, &speed);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.0f, 4.0f, "%.2fx"))
        {
            el_animation_set_speed(h, speed);
        }

        el_playback_mode_e mode = EL_PLAYBACK_LOOP;
        el_animation_get_playback_mode(h, &mode);
        int modeIdx = (mode == EL_PLAYBACK_LOOP) ? 0 : 1;
        const char *modeItems[] = {"Loop", "One-shot"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Mode", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
        {
            el_animation_set_playback_mode(h, modeIdx == 0 ? EL_PLAYBACK_LOOP : EL_PLAYBACK_ONE_SHOT);
        }

        el_end_action_e end = EL_END_ACTION_HOLD_CURRENT;
        el_animation_get_end_action(h, &end);
        int endIdx = static_cast<int>(end);
        const char *endItems[] = {"Hold current", "Hold end", "Hold start", "Restore"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("End action", &endIdx, endItems, IM_ARRAYSIZE(endItems)))
        {
            el_animation_set_end_action(h, static_cast<el_end_action_e>(endIdx));
        }

        float dur = 0.0f;
        el_animation_get_duration(h, &dur);
        ImGui::SetNextItemWidth(160.0f);
        if (dur > 0.0f)
        {
            float editable = dur;
            if (ImGui::SliderFloat("Duration", &editable, 0.05f, 10.0f, "%.2fs"))
            {
                el_animation_set_duration(h, editable);
            }
        }
        else
        {
            ImGui::BeginDisabled();
            float placeholder = 0.0f;
            ImGui::SliderFloat("Duration", &placeholder, 0.0f, 1.0f, "modulator-owned");
            ImGui::EndDisabled();
        }

        float elapsed = 0.0f;
        el_animation_get_elapsed(h, &elapsed);
        if (mode == EL_PLAYBACK_LOOP)
        {
            if (dur > 0.0f)
                ImGui::TextDisabled("t=%.2fs / cycle=%.2fs (looping)", elapsed, dur);
            else
                ImGui::TextDisabled("t=%.2fs (looping)", elapsed);
        }
        else
        {
            const char *status = (state == EL_ANIM_STATE_PLAYING) ? "running" : "stopped";
            ImGui::TextDisabled("t=%.2fs / dur=%.2fs (%s)", elapsed, dur, status);
        }

        ImGui::PopID();

        if (wantRemove)
        {
            el_effect_detach_animation(effect, h);
            el_animation_destroy(h);
            mAnimations.erase(mAnimations.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }

    ImGui::TextDisabled("Animated fields revert to their base value on Stop.");
}

// ---------------------------------------------------------------------------
// Background + Wireframe
// ---------------------------------------------------------------------------

void DebugUI::buildBackgroundSection()
{
    if (!ImGui::CollapsingHeader("Background (debug)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    ImGui::Checkbox("Show Checker##Bg", &mShowBackground);
    ImGui::TextDisabled("Toggle Neon > 'Opaque' to see blend vs occlude.");
    if (!mShowBackground)
        return;

    ImGui::SliderFloat("Checker Size##Bg", &mBgCheckerSize, 4.0f, 128.0f, "%.0f");
    ImGui::ColorEdit3("Color A##Bg", mBgColorA, ImGuiColorEditFlags_NoInputs);
    ImGui::ColorEdit3("Color B##Bg", mBgColorB, ImGuiColorEditFlags_NoInputs);
}

void DebugUI::buildWireframeSection(el_effect_handle_t effect)
{
    ImGui::Separator();
    el_bool_t on = 0;
    el_effect_get_wireframe_renderer_enabled(effect, &on);
    bool en = on;
    if (ImGui::Checkbox("Wireframe", &en))
    {
        el_effect_set_wireframe_renderer_enabled(effect, en ? 1 : 0);
    }
    if (en)
    {
        float r = 0, g = 0, b = 0, a = 0;
        el_effect_get_wireframe_color(effect, &r, &g, &b, &a);
        float col[4] = {r, g, b, a};
        ImGui::SameLine();
        if (ImGui::ColorEdit4("Wireframe Color", col,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview))
        {
            el_effect_set_wireframe_color(effect, col[0], col[1], col[2], col[3]);
        }
    }
}

// ---------------------------------------------------------------------------
// Border color picker
// ---------------------------------------------------------------------------

void DebugUI::scanColorPickerFiles()
{
    static const std::vector<std::string> exts = {
        ".png",
        ".jpg",
        ".jpeg",
        ".bmp",
        ".tga",
    };
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
            mColorPickerSelectedIdx = static_cast<int>(std::distance(mColorPickerFiles.begin(), it));
        }
    }
}

void DebugUI::uploadColorPickerTexture()
{
    if (!mColorPickerTex)
    {
        glGenTextures(1, &mColorPickerTex);
        glBindTexture(GL_TEXTURE_2D, mColorPickerTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, mColorPickerTex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 mColorPicker.Width(), mColorPicker.Height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 mColorPicker.Pixels().data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void DebugUI::buildColorPickerSection(el_effect_handle_t effect)
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
        std::string path = std::string(RES_DIR) + "/" + mColorPickerFiles[mColorPickerSelectedIdx];
        if (mColorPicker.Load(path))
        {
            uploadColorPickerTexture();
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
    if (mColorPickerTex)
    {
        const float maxW = 200.0f;
        const float aspect = static_cast<float>(mColorPicker.Height()) /
                             static_cast<float>(mColorPicker.Width());
        const ImVec2 size(maxW, maxW * aspect);
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(mColorPickerTex)), size);
    }

    ImGui::SliderInt("Stop Count##CP", &mColorPickerStopCount, 2, MAX_COLOR_STOPS);
    ImGui::TextDisabled("(higher = closer image match)");
    ImGui::SliderFloat("Contrast (gamma)##CP", &mColorPickerGamma, 0.5f, 4.0f, "%.2f");
    ImGui::SameLine();
    ImGui::TextDisabled("(1 = linear, >1 darkens shadows)");

    // Sample stops for preview + apply. Reads rect w/h/winding via capi so the
    // sampler and shader stay in agreement.
    float geoW = 0, geoH = 0, geoX = 0, geoY = 0, geoR = 0;
    el_effect_get_geometry(effect, &geoW, &geoH, &geoX, &geoY, &geoR);
    el_winding_e winding = EL_WINDING_CLOCKWISE;
    el_effect_get_winding(effect, &winding);

    auto stops = mColorPicker.SampleBorder(mColorPickerStopCount, winding,
                                           static_cast<int>(geoW),
                                           static_cast<int>(geoH));

    if (std::abs(mColorPickerGamma - 1.0f) > 1e-3f)
    {
        for (auto &s : stops)
        {
            s.r = std::pow(std::clamp(s.r, 0.0f, 1.0f), mColorPickerGamma);
            s.g = std::pow(std::clamp(s.g, 0.0f, 1.0f), mColorPickerGamma);
            s.b = std::pow(std::clamp(s.b, 0.0f, 1.0f), mColorPickerGamma);
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
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(stops[i].r, stops[i].g, stops[i].b, stops[i].a));
            dl->AddRectFilled(ImVec2(p.x + cellW * i, p.y),
                              ImVec2(p.x + cellW * (i + 1), p.y + stripH), col);
        }
        ImGui::Dummy(ImVec2(stripW, stripH));
    }

    // Auto-intensity: hold brightest stop near tone-map knee.
    float maxChan = 0.0f;
    for (const auto &s : stops)
    {
        maxChan = std::max({maxChan, s.r, s.g, s.b});
    }
    const float autoIntensity = maxChan > 1e-3f
                                    ? std::clamp(3.5f / (12.0f * maxChan), 0.05f, 1.0f)
                                    : 1.0f;

    ImGui::Checkbox("Auto-adjust intensity##CP", &mColorPickerAutoIntensity);
    if (mColorPickerAutoIntensity)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(-> %.2f)", autoIntensity);
    }

    if (ImGui::Button("Apply to Neon##CP") && !stops.empty())
    {
        // Apply picker stops: push count + values, kill hue rotation (fixed
        // stop-to-position mapping breaks under rotation), optionally
        // renormalise intensity.
        el_effect_set_color_stop_count(effect, static_cast<int32_t>(stops.size()));
        for (size_t i = 0; i < stops.size(); ++i)
        {
            el_effect_set_color_stop(effect, static_cast<int32_t>(i),
                                     stops[i].position, stops[i].r, stops[i].g, stops[i].b, stops[i].a);
        }
        el_effect_set_hue_rotation_rate(effect, 0.0f);
        if (mColorPickerAutoIntensity)
        {
            el_effect_set_intensity(effect, autoIntensity);
        }
    }
}
