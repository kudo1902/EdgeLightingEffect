#ifndef _EDGE_LIGHTING_CAPI_DEMO_DEBUG_UI_H_
#define _EDGE_LIGHTING_CAPI_DEMO_DEBUG_UI_H_

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "border-color-picker.h"
#include "edge-lighting-capi.h"

#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;

class DebugUI
{
public:
    DebugUI() = default;
    ~DebugUI();

    bool Init(GLFWwindow *mainWindow, int mainW, int mainH);
    void Shutdown();

    // Build one frame of widgets. Each widget reads its current value via
    // el_effect_get_*, and writes back via el_effect_set_* only when changed.
    void Build(el_effect_handle_t effect);

    void SetLastRenderTimeMs(float ms) { mLastRenderTimeMs = ms; }
    void Render();

    GLFWwindow *GetWindow() const { return mWindow; }

    bool IsBackgroundEnabled() const { return mShowBackground; }
    float GetBackgroundCheckerSize() const { return mBgCheckerSize; }
    const float *GetBackgroundColorA() const { return mBgColorA; }
    const float *GetBackgroundColorB() const { return mBgColorB; }

    bool IsImageBackdropEnabled() const
    {
        return mShowImageBackdrop && mColorPicker.HasImage() && mColorPickerTex != 0;
    }
    GLuint GetImageBackdropTextureId() const { return mColorPickerTex; }

private:
    void buildGeometrySection(el_effect_handle_t effect);
    void buildNeonSection(el_effect_handle_t effect);
    void buildOptimizedNeonSection(el_effect_handle_t effect);
    void buildDropletsSection(el_effect_handle_t effect);
    void buildSnowySection(el_effect_handle_t effect);
    void buildColorPickerSection(el_effect_handle_t effect);
    void buildAnimationSection(el_effect_handle_t effect);
    void buildBackgroundSection();
    void buildWireframeSection(el_effect_handle_t effect);

    void scanColorPickerFiles();
    void uploadColorPickerTexture();

private:
    GLFWwindow *mWindow = nullptr;
    GLFWwindow *mMainWindow = nullptr;
    ImGuiContext *mContext = nullptr;

    float mLastRenderTimeMs = 0.0f;

    // --- Animation state (demo-owned vector of capi handles) ---
    // Animations are attached to the effect; the effect owns their update cycle,
    // but the demo owns their handles (must destroy on Remove or shutdown).
    struct AnimEntry
    {
        el_animation_handle_t handle;
        std::string name;
    };
    std::vector<AnimEntry> mAnimations;
    int mAddPresetIdx = 0;

    // --- Background quad (demo-side) ---
    bool mShowBackground = false;
    float mBgCheckerSize = 24.0f;
    float mBgColorA[3] = {0.55f, 0.55f, 0.58f};
    float mBgColorB[3] = {0.20f, 0.20f, 0.23f};

    // --- Border color picker ---
    EdgeLightingCapiDemo::BorderColorPicker mColorPicker;
    // Thumbnail GL texture (demo owns; created lazily after first successful load).
    GLuint mColorPickerTex = 0;
    int mColorPickerStopCount = 20;
    std::vector<std::string> mColorPickerFiles;
    int mColorPickerSelectedIdx = 0;
    bool mShowImageBackdrop = false;
    bool mColorPickerAutoIntensity = true;
    float mColorPickerGamma = 1.0f;
};

#endif
