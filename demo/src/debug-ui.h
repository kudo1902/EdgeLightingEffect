#ifndef _EDGE_LIGHTING_DEMO_DEBUG_UI_H_
#define _EDGE_LIGHTING_DEMO_DEBUG_UI_H_

#include "gl/gl-header.h"
#include "animation-presets.h"
#include "animation/animation.h"
#include "border-color-picker.h"
#include "gl/texture-2d.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

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

    /// Update the currently selected animation preset (if any) and apply
    /// its output to @p config.
    /// @p clockTime is the effect's clock time; we use its per-frame delta
    /// to drive @c Animation::Update. The animation owns its own state,
    /// elapsed accumulator, and completion latching - pausing the effect
    /// clock freezes dt to 0 and effectively pauses every animation.
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

    /// @name Border color picker image backdrop
    /// When enabled and an image is loaded, the demo draws the source image
    /// inside the rect geometry behind the neon effect so the sampled border
    /// colours can be verified visually against the source.
    ///@{
    bool IsImageBackdropEnabled() const
    {
        return mShowImageBackdrop && mColorPicker.HasImage() &&
               mColorPickerThumb && mColorPickerThumb->IsValid();
    }
    GLuint GetImageBackdropTextureId() const
    {
        return mColorPickerThumb ? mColorPickerThumb->GetId() : 0;
    }
    ///@}

private:
    void buildGeometrySection(EdgeLighting::Config &cfg);
    void buildNeonSection(EdgeLighting::Config &cfg);
    void buildOptimizedNeonSection(EdgeLighting::Config &cfg);
    void buildAnimationSection(EdgeLighting::Config &cfg, float clockTime);
    void buildBackgroundSection();
    void buildColorPickerSection(EdgeLighting::Config &cfg);

private:
    GLFWwindow *mWindow = nullptr;
    GLFWwindow *mMainWindow = nullptr;
    ImGuiContext *mContext = nullptr;

    float mLastRenderTimeMs = 0.0f;

    // --- Animation state ---
    // Multi-animation UX: mActiveGroup owns any presets the user has added.
    // AnimationGroup broadcasts Play/Pause/Stop/Reset/Update/Apply, so the
    // section-level "Play all" / "Pause all" controls fall out for free, and
    // each child can still be controlled individually via its own Animation
    // methods.
    std::shared_ptr<EdgeLighting::AnimationGroup> mActiveGroup =
        std::make_shared<EdgeLighting::AnimationGroup>();
    /// Preset index selected in the Add combo.
    int mAddPresetIdx = 0;
    /// Human-readable preset name per active child, parallel to
    /// mActiveGroup->GetChildren(). Kept because AnimationGroup only stores
    /// AnimationPtr, and we want each row header to say "Breathing" instead
    /// of "Animation #3". Preset name pointers come from PresetName() which
    /// returns string literals - safe to hold as raw const char*.
    std::vector<const char *> mActiveNames;
    float mLastClockTime = 0.0f; ///< Clock time at last ApplyActiveAnimation tick.

    // --- Debug background quad (demo verification aid) ---
    bool mShowBackground = false;
    float mBgCheckerSize = 24.0f;
    glm::vec3 mBgColorA = glm::vec3(0.55f, 0.55f, 0.58f); ///< Light checker square.
    glm::vec3 mBgColorB = glm::vec3(0.20f, 0.20f, 0.23f); ///< Dark checker square.

    // --- Border color picker ---
    BorderColorPicker mColorPicker;
    /// Thumbnail texture for the loaded image, created lazily after Load()
    /// (needs a live GL context). Reused across loads.
    std::unique_ptr<EdgeLighting::Texture2D> mColorPickerThumb;
    /// Number of stops SampleBorder should produce on the next apply.
    int mColorPickerStopCount = 20;
    /// Image filenames discovered under RES_DIR (populated lazily on the
    /// first render of the picker section, or when Refresh is clicked).
    std::vector<std::string> mColorPickerFiles;
    /// Index into @c mColorPickerFiles of the currently selected image.
    int mColorPickerSelectedIdx = 0;
    /// When true, the demo draws the loaded image inside the rect geometry
    /// behind the neon effect.
    bool mShowImageBackdrop = false;
    /// Hue-preserving brightness boost for sampled colours: each stop is scaled
    /// so its brightest channel reaches this target (capped so near-black
    /// pixels aren't amplified into noise), preserving hue. Lets a palette
    /// picked from a dark image border render as vivid neon under faithful
    /// colour instead of a dim glow. 0 = raw (no boost), 1 = boost to full.
    float mColorPickerVividness = 0.9f;
    /// Contrast gamma applied to sampled colours before Apply. Values > 1
    /// darken dark stops (compressing shadows toward black) while leaving
    /// bright stops nearly untouched - useful for images with a wide
    /// dynamic range where you want the dim regions to read as dim.
    float mColorPickerGamma = 1.0f;

    /// Populate @c mColorPickerFiles by scanning RES_DIR for common image
    /// extensions. Preserves the current selection when possible.
    void scanColorPickerFiles();
};

#endif
