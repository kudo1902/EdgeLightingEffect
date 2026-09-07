#ifndef _EDGE_LIGHTING_GRADIENT_RING_LUT_H_
#define _EDGE_LIGHTING_GRADIENT_RING_LUT_H_

#include "core/config.h"
#include "renderer/base-lut.h"
#include "util/color-utils.h"
#include <algorithm>
#include <vector>

namespace EdgeLighting
{
    /// The base colour ring baked into a 1 x N RGBA8 texture, plus the
    /// cross-fade that carries one stop set into the next.
    ///
    /// Baking the whole ring on the CPU is what makes the shaders
    /// colour-stop-agnostic: each perimeter sample becomes one texture lookup
    /// instead of an in-shader stops walk and HSV/HSL blend. The texture is
    /// one row, sampled at v = 0.5, REPEAT on U so the sweep wraps.
    ///
    /// RGBA8, not float - see @ref BaseLUT, which owns the texture and the
    /// upload format; the ring quantises through @ref ColorUtils::ToByte on its
    /// way there.
    ///
    /// A colour change does not snap. @ref Bake stores the new ring as the
    /// target and snapshots what is currently on screen as the source;
    /// @ref Tick blends source -> target and re-uploads until the fade lands.
    /// Cross-fading in LUT space (rather than per stop) is what lets a fade
    /// run between stop sets that differ in count or position - there is no
    /// per-stop pairing to work out.
    ///
    /// Owned by both neon renderers. Per frame:
    ///
    ///     // OnConfigChanged - self-guarding, safe to call unconditionally
    ///     mGradientLUT.Bake(config.neon.colorStops, config.neon.blendSpace,
    ///                       GRADIENT_LUT_SIZE, config.neon.colorTransitionDuration);
    ///     // Update
    ///     mGradientLUT.Tick(deltaTime);
    ///     // Render
    ///     mGradientLUT.Bind(0);
    class GradientRingLUT : public BaseLUT
    {
    public:
        GradientRingLUT() = default;

        /// Bake @p stops into the target ring and start - or immediately land -
        /// the fade toward it.
        ///
        /// Does nothing when (@p stops, @p space, @p size) match the last bake.
        /// That guard is the reason this is safe to call every frame:
        /// OnConfigChanged fires on ANY change to the composited config, which
        /// with an animation attached is nearly every frame, so it almost
        /// always arrives with the gradient inputs untouched.
        ///
        /// @param size          ring width in texels. The documented range is
        ///                      32-256; the floor here is only a guard against a
        ///                      nonsense value reaching glTexImage2D.
        /// @param fadeDuration  seconds; <= 0 snaps straight to the new ring.
        void Bake(const std::vector<ColorStop> &stops, BlendSpace space,
                  int size, float fadeDuration)
        {
            size = std::max(size, 4);
            if (HasUploaded() && size == mSize && space == mBakedSpace && stops == mBakedStops)
            {
                return;
            }

            // Sorted once here, not per texel - SampleRing walks the ring in
            // order and an unsorted list bakes a silently wrong gradient.
            const std::vector<ColorStop> sorted = ColorUtils::SortStops(stops);
            mTarget.resize(static_cast<size_t>(size) * 4);
            for (int i = 0; i < size; ++i)
            {
                // t = i / size, NOT i / (size - 1): the ring is cyclic, so the
                // last texel must land one step BEFORE the wrap point rather
                // than on it, or the gradient repeats its first colour.
                float t = static_cast<float>(i) / static_cast<float>(size);
                glm::vec4 c = ColorUtils::SampleRing(t, sorted, space);
                mTarget[i * 4 + 0] = c.r;
                mTarget[i * 4 + 1] = c.g;
                mTarget[i * 4 + 2] = c.b;
                mTarget[i * 4 + 3] = c.a;
            }

            const bool sizeChanged = (size != mSize);
            mSize = size;
            mBakedStops = stops;
            mBakedSpace = space;

            // Snap paths, all of which re-seed every buffer so the NEXT change
            // has a settled ring to fade from:
            //   - first bake: there is nothing on screen to fade from;
            //   - no fade requested;
            //   - the width changed, so source and target have different
            //     lengths and cannot be blended element-wise.
            if (!HasUploaded() || sizeChanged || fadeDuration <= 0.0f)
            {
                mFrom = mTarget;
                mDisplay = mTarget;
                upload(); // sets HasUploaded(), which is what the guards above read
                mFading = false;
                return;
            }

            // Fade from whatever is currently on screen - settled or mid-fade -
            // toward the new target. The first blended upload happens in this
            // same frame's Tick, since SetConfig -> OnConfigChanged runs before
            // Update.
            mFrom = mDisplay;
            mElapsed = 0.0f;
            mDuration = fadeDuration;
            mFading = true;
        }

        /// Advance an in-flight cross-fade and re-upload; a no-op once settled.
        ///
        /// @param deltaTime raw frame delta, NOT clock time - a colour change
        ///        should still fade smoothly while the animation clock is
        ///        paused.
        /// @return true if the ring texture was re-uploaded by this call.
        ///         Anything CACHED from the ring has to be rebuilt when it is -
        ///         a fade moves the texture with no config change to announce
        ///         it, so a consumer gating on OnConfigChanged alone would hold
        ///         a stale derivative for the length of the fade. The neon
        ///         emission table is that consumer; see
        ///         @c NeonRenderer::isEmissionTableStale.
        bool Tick(float deltaTime)
        {
            if (!mFading)
            {
                return false;
            }

            mElapsed += deltaTime;
            float u = (mDuration > 0.0f) ? (mElapsed / mDuration) : 1.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            float s = u * u * (3.0f - 2.0f * u); // smoothstep ease-in-out

            const size_t n = static_cast<size_t>(mSize) * 4;
            mDisplay.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                mDisplay[i] = mFrom[i] + (mTarget[i] - mFrom[i]) * s;
            }
            upload();

            if (u >= 1.0f)
            {
                mDisplay = mTarget; // land exactly on the target
                mFading = false;
            }
            return true;
        }

    private:
        /// Quantise mDisplay to RGBA8 and upload it.
        void upload()
        {
            // Held as a member so a fade frame does no heap allocation.
            mBytes.resize(mDisplay.size());
            for (size_t i = 0; i < mDisplay.size(); ++i)
            {
                mBytes[i] = ColorUtils::ToByte(mDisplay[i]);
            }

            // 1-row 2D texture (sampled at v = 0.5 in the shader). REPEAT on
            // the U axis lets the gradient sweep wrap naturally.
            Upload(mBytes.data(), mSize, /*height=*/1, GL_REPEAT);
        }

    private:
        // All three are float RGBA (mSize * 4). Kept in float, not bytes, so a
        // long fade does not accumulate quantisation error step by step - only
        // the upload rounds.
        std::vector<float> mTarget;        ///< Freshly baked destination ring.
        std::vector<float> mFrom;          ///< Ring shown when the current fade began.
        std::vector<float> mDisplay;       ///< Currently-uploaded (blended) ring.
        std::vector<unsigned char> mBytes; ///< Reused upload scratch.
        int mSize = 0;                     ///< Ring width in texels; 0 until the first bake.
        bool mFading = false;              ///< True while a cross-fade is in flight.
        float mElapsed = 0.0f;             ///< Seconds into the current fade.
        float mDuration = 0.0f;            ///< Snapshot of the duration for this fade.

        /// The (stops, blendSpace) behind mTarget - what @ref Bake guards on.
        std::vector<ColorStop> mBakedStops;
        BlendSpace mBakedSpace = BlendSpace::RGB;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_GRADIENT_RING_LUT_H_
