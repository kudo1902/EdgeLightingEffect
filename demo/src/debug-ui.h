#ifndef _EDGE_LIGHTING_DEMO_DEBUG_UI_H_
#define _EDGE_LIGHTING_DEMO_DEBUG_UI_H_

#include "gl/gl-header.h"
#include "animation-presets.h"
#include "animation/animation.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

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

    /// Feed the last frame's render time (only gEffect->Render, no ImGui) for display.
    void SetLastRenderTimeMs(float ms) { mLastRenderTimeMs = ms; }

    /// Apply the currently selected animation preset (if any) to @p config.
    /// @p clockTime is the effect's clock time; this method subtracts the
    /// preset's start time so each preset evaluates from its own t = 0.
    /// Also fires the animation's @c OnComplete callback exactly once when
    /// the elapsed time first crosses the animation's duration.
    void ApplyActiveAnimation(EdgeLighting::Config &config, float clockTime);

    /// Render the ImGui frame into the debug window. Must be called after Build() each frame.
    void Render();

    GLFWwindow *GetWindow() const { return mWindow; }

    /// @name Debug background quad (demo verification aid)
    /// Drawn by the demo behind the effect to visualise neon compositing.
    ///@{
    bool IsBackgroundEnabled() const { return mShowBackground; }
    float GetBackgroundCheckerSize() const { return mBgCheckerSize; }
    const glm::vec3 &GetBackgroundColorA() const { return mBgColorA; }
    const glm::vec3 &GetBackgroundColorB() const { return mBgColorB; }
    ///@}

private:
    void buildGeometrySection(EdgeLighting::Config &cfg);
    void buildNeonSection(EdgeLighting::Config &cfg);
    void buildMultiPassNeonSection(EdgeLighting::Config &cfg);
    void buildOptimizedNeonSection(EdgeLighting::Config &cfg);
    void buildAnimationSection(EdgeLighting::Config &cfg, float clockTime);
    void buildBackgroundSection();

    GLFWwindow *mWindow = nullptr;
    GLFWwindow *mMainWindow = nullptr;
    ImGuiContext *mContext = nullptr;

    float mLastRenderTimeMs = 0.0f;

    // --- Animation preset state ---
    EdgeLightingDemo::AnimationPreset mPreset = EdgeLightingDemo::AnimationPreset::NONE;
    EdgeLighting::AnimationPtr mAnimation;
    float mAnimElapsed   = 0.0f;     ///< Animation-time accumulator (scaled by speed each frame).
    float mLastClockTime = 0.0f;     ///< Clock time at last ApplyActiveAnimation tick.
    bool  mFiredComplete = false;    ///< Edge-triggered OnComplete latch.

    // --- Debug background quad (demo verification aid) ---
    bool      mShowBackground = false;
    float     mBgCheckerSize  = 24.0f;
    glm::vec3 mBgColorA       = glm::vec3(0.55f, 0.55f, 0.58f); ///< Light checker square.
    glm::vec3 mBgColorB       = glm::vec3(0.20f, 0.20f, 0.23f); ///< Dark checker square.
};

#endif
