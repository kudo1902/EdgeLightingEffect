#ifndef _EDGE_LIGHTING_DEMO_DEBUG_UI_H_
#define _EDGE_LIGHTING_DEMO_DEBUG_UI_H_

#include "gl/gl-header.h"
#include "animation-presets.h"
#include "animation/animation.h"
#include <GLFW/glfw3.h>

struct ImGuiContext;

namespace EdgeLighting
{
    struct Config;
    class EdgeLightingEffect;
}

class DebugUI
{
public:
    DebugUI() = default;
    ~DebugUI();

    bool Init(GLFWwindow *mainWindow, int mainW, int mainH);
    void Shutdown();

    /// Build ImGui widgets for the given config. Mutates @p config in place to
    /// reflect slider drags and preset selections. Caller is responsible for
    /// pushing the resulting config back to the effect.
    void Build(EdgeLighting::Config &config, EdgeLighting::EdgeLightingEffect &effect);

    /// Apply the currently selected animation preset (if any) to @p config.
    /// @p clockTime is the effect's clock time; this method subtracts the
    /// preset's start time so each preset evaluates from its own t = 0.
    /// Also fires the animation's @c OnComplete callback exactly once when
    /// the elapsed time first crosses the animation's duration.
    void ApplyActiveAnimation(EdgeLighting::Config &config, float clockTime);

    /// Render the ImGui frame into the debug window. Must be called after Build() each frame.
    void Render();

    GLFWwindow *GetWindow() const { return mWindow; }

private:
    void buildGeometrySection(EdgeLighting::Config &cfg);
    void buildNeonSection(EdgeLighting::Config &cfg);
    void buildMultiPassNeonSection(EdgeLighting::Config &cfg);
    void buildAnimationSection(EdgeLighting::Config &cfg, float clockTime);

    GLFWwindow *mWindow = nullptr;
    GLFWwindow *mMainWindow = nullptr;
    ImGuiContext *mContext = nullptr;

    // --- Animation preset state ---
    EdgeLightingDemo::AnimationPreset mPreset = EdgeLightingDemo::AnimationPreset::NONE;
    EdgeLighting::AnimationPtr mAnimation;
    float mAnimElapsed   = 0.0f;     ///< Animation-time accumulator (scaled by speed each frame).
    float mLastClockTime = 0.0f;     ///< Clock time at last ApplyActiveAnimation tick.
    bool  mFiredComplete = false;    ///< Edge-triggered OnComplete latch.
};

#endif
