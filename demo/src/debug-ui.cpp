#include "debug-ui.h"
#include "core/config.h"
#include "core/edge-lighting.h"
#include "ui-controls.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include "util/screenshot-util.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cstdio>

namespace
{
    // Compact colour-stop editor (blend space + per-stop position/colour + add/
    // remove). Relies on the surrounding ImGui ID stack for uniqueness.
    void DrawColorStops(std::vector<EdgeLighting::ColorStop> &stops,
                        EdgeLighting::BlendSpace &blend, int maxStops)
    {
        using namespace EdgeLighting;
        const char *blendItems[] = {"RGB", "HSV"};
        int bi = static_cast<int>(blend);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("Blend", &bi, blendItems, IM_ARRAYSIZE(blendItems)))
        {
            blend = static_cast<BlendSpace>(bi);
        }

        int removeStop = -1;
        for (int i = 0; i < static_cast<int>(stops.size()); ++i)
        {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Pos", &stops[i].position, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            glm::vec4 c = stops[i].color;
            if (ImGui::ColorEdit4("Col", &c.x, ImGuiColorEditFlags_NoInputs))
            {
                stops[i].color = c;
            }
            if (stops.size() > 1)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                {
                    removeStop = i;
                }
            }
            ImGui::PopID();
        }
        if (removeStop >= 0)
        {
            stops.erase(stops.begin() + removeStop);
        }
        if (static_cast<int>(stops.size()) < maxStops && ImGui::SmallButton("+ Stop"))
        {
            float last = stops.empty() ? 0.0f : stops.back().position;
            stops.push_back({std::min(last + 0.1f, 1.0f), glm::vec4(1.0f)});
        }
    }

    // Segment-list editor shared by the Neon and Optimized sections (both read
    // Config::neon). @p suffix scopes the ImGui ID stack so both panels can be
    // open at once without colliding ("##Neon"/"##Opt").
    void DrawNeonSegments(EdgeLighting::NeonConfig &neon,
                          const EdgeLighting::RectGeometry &geom,
                          const char *suffix)
    {
        using namespace EdgeLighting;
        ImGui::PushID(suffix);

        // Quick presets that snap the segment list to common configurations.
        if (ImGui::Button("Side Edges"))
        {
            // GetEdgeArc returns {start, fullLength}; centre the 0.95-coverage
            // bar within the edge (position is the segment's start).
            auto edgeSeg = [](const glm::vec2 &e)
            {
                float len = e.y * 0.95f;
                return LitSegment{e.x + (e.y - len) * 0.5f, len, 1.0f};
            };
            neon.segments = {
                edgeSeg(GeometryUtils::GetEdgeArc(Edge::LEFT, geom)),
                edgeSeg(GeometryUtils::GetEdgeArc(Edge::RIGHT, geom)),
            };
        }
        ImGui::SameLine();
        if (ImGui::Button("Full Ring"))
        {
            neon.segments = {DefaultRingSegment()};
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Segs"))
        {
            neon.segments.clear();
        }

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(neon.segments.size()); ++i)
        {
            LitSegment &seg = neon.segments[i];
            ImGui::PushID(i);
            char header[24];
            std::snprintf(header, sizeof(header), "Segment %d", i);
            if (ImGui::TreeNode(header))
            {
                // Span.
                ImGui::SliderFloat("Pos", &seg.position, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Len", &seg.length, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Brightness", &seg.brightness, 0.0f, 5.0f, "%.2f");

                // Colour (empty stops => inherit the global gradient).
                bool ownColour = !seg.colorStops.empty();
                if (ImGui::Checkbox("Own colour", &ownColour))
                {
                    if (ownColour)
                    {
                        seg.colorStops = {{0.0f, glm::vec4(1, 0, 0, 1)}, {1.0f, glm::vec4(0, 0, 1, 1)}};
                    }
                    else
                    {
                        seg.colorStops.clear();
                    }
                }
                if (!seg.colorStops.empty())
                {
                    ImGui::Indent();
                    ImGui::SliderFloat("Hue Rate", &seg.hueRotationRate, -2.0f, 2.0f, "%.2f");
                    DrawColorStops(seg.colorStops, seg.blendSpace, NeonConfig::MAX_COLOR_STOPS);
                    ImGui::Unindent();
                }

                // Nested spot (boost > 0 = active).
                bool spotOn = seg.spot.boost > 0.0f;
                if (ImGui::Checkbox("Spot", &spotOn))
                {
                    seg.spot.boost = spotOn ? 3.0f : 0.0f;
                }
                if (seg.spot.boost > 0.0f)
                {
                    ImGui::Indent();
                    ImGui::SliderFloat("Spot Pos", &seg.spot.position, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Spot Len", &seg.spot.length, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Spot Boost", &seg.spot.boost, 0.0f, 8.0f, "%.2f");
                    bool spotColour = !seg.spot.colorStops.empty();
                    if (ImGui::Checkbox("Spot own colour", &spotColour))
                    {
                        if (spotColour)
                        {
                            seg.spot.colorStops = {{0.0f, glm::vec4(1.0f)}, {1.0f, glm::vec4(1.0f)}};
                        }
                        else
                        {
                            seg.spot.colorStops.clear();
                        }
                    }
                    if (!seg.spot.colorStops.empty())
                    {
                        DrawColorStops(seg.spot.colorStops, seg.spot.blendSpace, NeonConfig::MAX_COLOR_STOPS);
                    }
                    ImGui::Unindent();
                }

                if (ImGui::SmallButton("Remove segment"))
                {
                    removeIndex = i;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIndex >= 0)
        {
            neon.segments.erase(neon.segments.begin() + removeIndex);
        }

        if (static_cast<int>(neon.segments.size()) < NeonConfig::MAX_SEGMENTS &&
            ImGui::Button("Add Segment"))
        {
            neon.segments.push_back(LitSegment{0.5f, 0.2f, 1.0f});
        }

        ImGui::PopID();
    }
} // namespace

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

    // Pin the panel to fill its OS window so it never floats/overlaps; long
    // content scrolls inside instead of clipping against the window edge.
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("Debug Controls", nullptr, rootFlags);

    // --- Perf readout (always visible, sticky at the top) ---
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

    ImGui::Checkbox("Opaque (no blend)##Neon", &cfg.neon.opaque);
    ImGui::SliderFloat("Line Width##Neon", &cfg.neon.lineWidth, 0.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Neon", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Neon", &cfg.neon.glowRadius, 0.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Neon", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");

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

    // --- Lit segments: the whole colour/effect lives here (per-segment colour,
    //     rotation and spots; "Side Edges" / "Full Ring" presets). ---
    DrawNeonSegments(cfg.neon, cfg.geometry, "##Neon");
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

    ImGui::Separator();
    ImGui::TextDisabled("Visual params (shared with Neon)");

    ImGui::SliderFloat("Line Width##Opt", &cfg.neon.lineWidth, 1.0f, 20.0f, "%.0f");
    ImGui::SliderFloat("Intensity##Opt", &cfg.neon.intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Glow Radius##Opt", &cfg.neon.glowRadius, 1.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Bloom Strength##Opt", &cfg.neon.bloomStrength, 0.0f, 2.0f, "%.2f");

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

    // Lit segments are shared with the Neon renderer (both read Config::neon).
    DrawNeonSegments(cfg.neon, cfg.geometry, "##Opt");
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
