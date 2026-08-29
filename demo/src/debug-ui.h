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
    class AnimationManager;
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

    /// Render the ImGui frame into the debug window. Must be called after Build() each frame.
    void Render();

    GLFWwindow *GetWindow() const { return mWindow; }

    /// @name Offscreen frame capture
    /// The Capture button cannot read pixels itself: Build() runs while the
    /// debug window's context is current and before this frame has been drawn,
    /// so anything it read back would be the wrong buffer. It only raises a
    /// request; the demo's render loop draws the scene into an offscreen
    /// framebuffer at the requested size and saves that.
    ///@{
    /// Consumes a pending capture request, if any, and reports the size the
    /// frame should be rendered at. @return @c true exactly once per click.
    bool ConsumeCaptureRequest(int &outWidth, int &outHeight);
    ///@}

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
    void buildNeonSection(EdgeLighting::Config &cfg,
                          const EdgeLighting::Config &active);
    /// Overlays drawn by DebugRenderer. Takes no `active` config: these are
    /// plain toggles, with nothing an animation could be driving.
    void buildDebugSection(EdgeLighting::Config &cfg);
    void buildDropletsSection(EdgeLighting::Config &cfg);
    void buildLensFlareSection(EdgeLighting::Config &cfg);
    void buildAnimationSection(EdgeLighting::Config &cfg,
                               EdgeLighting::AnimationManager &manager);
    void buildBackgroundSection();
    void buildColorPickerSection(EdgeLighting::Config &cfg);

private:
    GLFWwindow *mWindow = nullptr;
    GLFWwindow *mMainWindow = nullptr;
    ImGuiContext *mContext = nullptr;

    float mLastRenderTimeMs = 0.0f;

    // --- Offscreen frame capture ---
    /// Set by the Capture button, cleared by ConsumeCaptureRequest.
    bool mCaptureRequested = false;
    /// Capture resolution. Fixed rather than window-derived on purpose: the
    /// window framebuffer is 2x on Retina and 1x elsewhere, so a
    /// window-sized capture is not comparable across machines.
    int mCaptureSize[2] = {1280, 720};

    // --- Animation state ---
    // Animations are owned by the effect's AnimationManager (via
    // effect.GetAnimationManager()); this section only attaches/detaches presets to it
    // and drives each row's per-animation controls. The effect advances and
    // composites them each Update.
    /// Preset index selected in the Add combo.
    int mAddPresetIdx = 0;

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
    /// When true, Apply-to-* also computes an intensity that keeps the
    /// brightest sampled colour near the tone-map knee so dark stops stay
    /// legibly dark instead of being pushed to white by FILAMENT_GAIN.
    bool mColorPickerAutoIntensity = true;
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
