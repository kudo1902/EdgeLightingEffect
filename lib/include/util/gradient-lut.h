#ifndef _EDGE_LIGHTING_GRADIENT_LUT_H_
#define _EDGE_LIGHTING_GRADIENT_LUT_H_

#include "core/config.h"
#include "gl/texture-2d.h"
#include "util/color-utils.h"
#include <algorithm>
#include <vector>

namespace EdgeLighting
{
    /// The baked colour ring: a set of @ref ColorStop resolved into a 1 x N
    /// RGBA8 texture (sampled with v = 0.5, REPEAT on U so the hue sweep wraps).
    /// Every sample in a shader becomes one texture lookup instead of an
    /// in-shader stops loop plus an HSV/HSL blend.
    ///
    /// Owns the cross-fade too. When the stops change we don't snap: the new
    /// ring is baked into @c mTarget, the currently-shown ring is snapshotted
    /// into @c mFrom, and @ref Update blends From->Target into @c mDisplay
    /// over @c transitionDuration seconds - @c mDisplay is what gets quantised
    /// and uploaded. Cross-fading in LUT space handles stop sets that differ in
    /// count or position (there is no per-stop pairing to worry about).
    ///
    /// Shared by every layer that needs the ring - both neon renderers and the
    /// debug overlay's LUT strip - so they cannot drift out of sync.
    class GradientLUT
    {
    public:
        /// Default ring width. 256 texels is more than any gradient the human
        /// eye can resolve; @ref Rebuild takes the size per call so the
        /// optimized renderer can trade it down.
        static constexpr int DEFAULT_SIZE = 256;

        GradientLUT() = default;

        /// Bake @p stops into the ring and start (or skip) a cross-fade.
        ///
        /// Safe to call every frame: a call whose (stops, blendSpace, size)
        /// match the last bake returns without touching GL, so callers don't
        /// have to gate it themselves.
        ///
        /// @param stops              Colour stops to resolve.
        /// @param blendSpace         Interpolation space between stops.
        /// @param transitionDuration Cross-fade length in seconds; <= 0 snaps.
        /// @param size               Ring width in texels (clamped to >= 4).
        void Rebuild(const std::vector<ColorStop> &stops, BlendSpace blendSpace,
                     float transitionDuration, int size = DEFAULT_SIZE)
        {
            int lutSize = std::max(size, 4);

            // Nothing that shapes the ring changed - a re-entry from a
            // per-frame config push, not an actual edit.
            if (mHasBaked && lutSize == mSize && blendSpace == mBakedBlendSpace &&
                stops == mBakedStops)
            {
                return;
            }

            mTarget.resize(lutSize * 4);
            for (int i = 0; i < lutSize; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(lutSize);
                glm::vec3 c = ColorUtils::SampleStops(t, stops, blendSpace);
                mTarget[i * 4 + 0] = c.r;
                mTarget[i * 4 + 1] = c.g;
                mTarget[i * 4 + 2] = c.b;
                mTarget[i * 4 + 3] = 1.0f;
            }

            const bool sizeChanged = lutSize != mSize;
            mSize = lutSize;
            mBakedStops = stops;
            mBakedBlendSpace = blendSpace;

            // Snap paths: the first bake (nothing to fade from), no fade
            // requested, or a width change (the buffers differ in length, so
            // there is nothing to lerp element-wise). Re-seed everything to
            // the target so the next same-width change can fade from here.
            if (!mHasBaked || sizeChanged || transitionDuration <= 0.0f)
            {
                mFrom = mTarget;
                mDisplay = mTarget;
                upload(mDisplay);
                mHasBaked = true;
                mFading = false;
                return;
            }

            // Fade from whatever is currently on screen (mid-fade or settled)
            // toward the new target. The owning renderer's Update() does the
            // first blended upload the same frame.
            mFrom = mDisplay;
            mFadeElapsed = 0.0f;
            mFadeDuration = transitionDuration;
            mFading = true;
        }

        /// Advance an in-flight cross-fade and re-upload the blended ring.
        /// Pass the raw frame delta, not clock time, so a colour change still
        /// fades smoothly while the animation clock is paused. No-op when no
        /// fade is running.
        void Update(float deltaTime)
        {
            if (!mFading)
            {
                return;
            }

            mFadeElapsed += deltaTime;
            float u = (mFadeDuration > 0.0f) ? (mFadeElapsed / mFadeDuration) : 1.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            float s = u * u * (3.0f - 2.0f * u); // smoothstep ease-in-out

            const int n = mSize * 4;
            mDisplay.resize(n);
            for (int i = 0; i < n; ++i)
            {
                mDisplay[i] = mFrom[i] + (mTarget[i] - mFrom[i]) * s;
            }
            upload(mDisplay);

            if (u >= 1.0f)
            {
                mDisplay = mTarget; // land exactly on the target
                mFading = false;
            }
        }

        /// Bind the ring texture to @p unit for sampling.
        void Bind(int unit = 0) const { mTexture.Bind(unit); }

        /// Ring width in texels (0 until the first @ref Rebuild).
        int GetSize() const { return mSize; }

        /// True while a cross-fade is in flight.
        bool IsFading() const { return mFading; }

    private:
        /// Quantise the float ring to RGBA8 and upload it. Edge devices often
        /// lack float-texture support, so the GPU copy is always ubyte.
        void upload(const std::vector<float> &lut) const
        {
            std::vector<unsigned char> bytes(static_cast<size_t>(mSize) * 4);
            for (int i = 0; i < mSize * 4; ++i)
            {
                bytes[i] = static_cast<unsigned char>(
                    std::clamp(lut[i] * 255.0f, 0.0f, 255.0f));
            }

            // 1-row 2D texture (sampled with v = 0.5). REPEAT on U lets the
            // gradient sweep wrap naturally; V is a single row, so CLAMP.
            mTexture.SetData(bytes.data(), mSize, /*height=*/1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            mTexture.SetParams(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE);
        }

    private:
        Texture2D mTexture;

        std::vector<float> mTarget;  ///< Freshly baked destination ring.
        std::vector<float> mFrom;    ///< Ring shown when the current fade began.
        std::vector<float> mDisplay; ///< Currently-uploaded (blended) ring.

        int mSize = 0;             ///< Ring width in texels.
        bool mHasBaked = false;    ///< False until the first bake seeds the buffers.
        bool mFading = false;      ///< True while a cross-fade is in flight.
        float mFadeElapsed = 0.0f; ///< Seconds into the current fade.
        float mFadeDuration = 0.0f;

        /// (stops, blendSpace) behind mTarget - a re-bake with these unchanged
        /// is a no-op, so callers can push their config every frame.
        std::vector<ColorStop> mBakedStops;
        BlendSpace mBakedBlendSpace = BlendSpace::RGB;
    };
}

#endif // _EDGE_LIGHTING_GRADIENT_LUT_H_
