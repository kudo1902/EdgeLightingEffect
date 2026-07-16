#ifndef _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
#define _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_

#include "animation/animation.h"
#include "animation/modulator.h"
#include <utility>
#include <vector>

namespace EdgeLighting
{
    /// @brief Scalar Config leaves that a @ref FieldBoundAnimation can drive
    ///        through @ref FieldBoundAnimation::AddField.
    ///
    /// Enum values are numerically ABI-stable - the C API's @c EL_ConfigField
    /// mirrors them 1:1 and static_casts between the two. Append new fields at
    /// the end so both C and C++ callers stay forward-compatible.
    ///
    /// Segment struct fields (position / length / boost inside
    /// @c NeonConfig::segmentBoosts[i]) live in @ref SegmentField because
    /// they need an index; use @ref FieldBoundAnimation::AddSegmentField for
    /// those.
    typedef enum class AnimatableField
    {
        NEON_INTENSITY = 0,
        NEON_LINE_WIDTH = 1,
        NEON_GLOW_RADIUS = 2,
        NEON_BLOOM_STRENGTH = 3,
        NEON_FILAMENT_FALLOFF = 4,
        NEON_GLOW_SIDE_SOFTNESS = 5,
        NEON_HUE_ROTATION_RATE = 6,
        NEON_ARC_START = 7,
        NEON_ARC_LENGTH = 8,
    } AnimatableField;

    /// @brief Which scalar to drive inside a @c NeonConfig::segmentBoosts entry.
    ///
    /// Paired with an index at bind time via
    /// @ref FieldBoundAnimation::AddSegmentField. Numerically mirrored by
    /// @c EL_SegmentField in the C ABI.
    typedef enum class SegmentField
    {
        POSITION = 0,
        LENGTH = 1,
        BOOST = 2,
    } SegmentField;

    /// @brief Which scalar to drive inside a single stop of
    ///        @c NeonConfig::segmentBoosts[segIdx].colorStops[stopIdx].
    ///
    /// Paired with @c (segIdx, stopIdx) at bind time via
    /// @ref FieldBoundAnimation::AddStopField. Numerically mirrored by
    /// @c EL_ColorStopField in the C ABI when that binding is added.
    typedef enum class ColorStopField
    {
        POSITION = 0, ///< @c ColorStop::position - normalised offset within the segment span.
        R = 1,        ///< @c ColorStop::color.r
        G = 2,        ///< @c ColorStop::color.g
        B = 3,        ///< @c ColorStop::color.b
        A = 4,        ///< @c ColorStop::color.a
    } ColorStopField;

    /// @brief Animation whose per-frame behaviour is defined at runtime by
    ///        binding modulators to Config fields.
    ///
    /// While the neon-animations.h subclasses each hard-code a specific field
    /// (e.g. @c IntensityPulse writes @c neon.intensity), @c FieldBoundAnimation
    /// carries a list of bindings. Two kinds:
    ///
    /// - @ref AddField(AnimatableField, ModulatorPtr) - drive a scalar leaf.
    /// - @ref AddSegmentField(size_t, SegmentField, ModulatorPtr) - drive one
    ///   scalar inside @c NeonConfig::segmentBoosts[index], auto-growing the
    ///   vector to that slot.
    /// - @ref AddStopField(size_t, size_t, ColorStopField, ModulatorPtr) -
    ///   drive one scalar inside
    ///   @c NeonConfig::segmentBoosts[segIdx].colorStops[stopIdx],
    ///   auto-growing both the segments vector and that segment's stops
    ///   vector as needed.
    ///
    /// On each @ref ApplyAt every binding's modulator is evaluated with the
    /// same @c elapsed and the result is written to its bound target.
    ///
    /// The shared elapsed / duration / play state phase-locks all bindings -
    /// use this class for "one animation, several fields moving together"
    /// (e.g. a heartbeat that pulses intensity + glow + bloom in unison, or
    /// two segments travelling with a fixed phase offset). For independent
    /// parallel animations with different rates, put separate
    /// @c FieldBoundAnimation instances into an @ref AnimationGroup.
    ///
    /// @code
    ///     auto pulse = std::make_shared<Oscillator>(1.0f, 0.4f, 1.0f);
    ///     FieldBoundAnimation breathing(AnimatableField::NEON_INTENSITY, pulse);
    ///     breathing.AddField(AnimatableField::NEON_GLOW_RADIUS,
    ///                        std::make_shared<Remap>(pulse, 3.0f, 20.0f));
    ///
    ///     // Two travelling segments, phase-locked:
    ///     auto lead = std::make_shared<Oscillator>(1.0f / 3.0f, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH);
    ///     auto tail = std::make_shared<Oscillator>(1.0f / 3.0f, 0.0f, 1.0f, 0.5f, Waveform::SAWTOOTH);
    ///     breathing.AddSegmentField(0, SegmentField::POSITION, lead);
    ///     breathing.AddSegmentField(1, SegmentField::POSITION, tail);
    ///
    ///     // Pulse the red channel of segment 0's first stop:
    ///     breathing.AddStopField(0, 0, ColorStopField::R, pulse);
    ///     breathing.Play();
    /// @endcode
    class FieldBoundAnimation : public Animation
    {
    public:
        /// @brief A single (scalar field, modulator) binding.
        typedef struct ScalarBinding
        {
            AnimatableField field;
            ModulatorPtr modulator;
        } ScalarBinding;

        /// @brief A single (segmentBoosts[index].field, modulator) binding.
        typedef struct SegmentBinding
        {
            size_t index;
            SegmentField field;
            ModulatorPtr modulator;
        } SegmentBinding;

        /// @brief A single (segmentBoosts[segIdx].colorStops[stopIdx].field,
        ///        modulator) binding.
        typedef struct SegmentStopBinding
        {
            size_t segIndex;
            size_t stopIndex;
            ColorStopField field;
            ModulatorPtr modulator;
        } SegmentStopBinding;

        // --- Construction ------------------------------------------------

        /// @brief Zero-binding animation. Extend via @ref AddField /
        ///        @ref AddSegmentField.
        /// @param duration Length of one cycle in seconds; 0 = modulator owns
        ///                 its own periodicity (matches @ref Animation base).
        /// @param mode     LOOP (default) or ONE_SHOT.
        explicit FieldBoundAnimation(float duration = 0.0f,
                                     PlaybackMode mode = PlaybackMode::LOOP)
        {
            SetDuration(duration);
            SetPlaybackMode(mode);
        }

        /// @brief One-binding convenience constructor for a scalar field.
        FieldBoundAnimation(AnimatableField field,
                            ModulatorPtr modulator,
                            float duration = 0.0f,
                            PlaybackMode mode = PlaybackMode::LOOP)
            : FieldBoundAnimation(duration, mode)
        {
            AddField(field, std::move(modulator));
        }

        // --- Bindings ----------------------------------------------------

        /// @brief Bind a scalar field to a modulator.
        /// @note Bindings are evaluated in insertion order. Modulators can be
        ///       shared across bindings - pass the same @c ModulatorPtr to
        ///       drive several fields off one signal (or wrap in @ref Remap
        ///       per binding for per-field ranges).
        void AddField(AnimatableField field, ModulatorPtr modulator)
        {
            mScalarBindings.push_back({field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside @c segmentBoosts[index] to a modulator.
        /// @details @p index auto-grows the vector at write time. Multiple
        ///          segment bindings can target different indices for
        ///          multi-segment animations phase-locked to one clock.
        void AddSegmentField(size_t index, SegmentField field, ModulatorPtr modulator)
        {
            mSegmentBindings.push_back({index, field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside
        ///        @c segmentBoosts[segIdx].colorStops[stopIdx] to a modulator.
        /// @details Both @p segIdx (segments vector) and @p stopIdx (that
        ///          segment's stops vector) auto-grow at write time. New
        ///          stops are seeded to a visible default (mid-position,
        ///          opaque white) so binding only a single channel still
        ///          shows something.
        void AddStopField(size_t segIdx, size_t stopIdx,
                          ColorStopField field, ModulatorPtr modulator)
        {
            mSegmentStopBindings.push_back({segIdx, stopIdx, field, std::move(modulator)});
        }

        // --- Introspection -----------------------------------------------

        /// @brief Read-only view of the scalar-field bindings.
        const std::vector<ScalarBinding> &GetScalarBindings() const { return mScalarBindings; }

        /// @brief Read-only view of the segment-field bindings.
        const std::vector<SegmentBinding> &GetSegmentBindings() const { return mSegmentBindings; }

        /// @brief Read-only view of the segment-stop-field bindings.
        const std::vector<SegmentStopBinding> &GetSegmentStopBindings() const
        {
            return mSegmentStopBindings;
        }

        /// @brief Total number of bindings (scalar + segment + segment-stop).
        size_t GetBindingCount() const
        {
            return mScalarBindings.size() + mSegmentBindings.size() + mSegmentStopBindings.size();
        }

        /// @brief Drop every binding (leaves state / duration alone).
        void ClearBindings()
        {
            mScalarBindings.clear();
            mSegmentBindings.clear();
            mSegmentStopBindings.clear();
        }

        // --- Drive -------------------------------------------------------

        /// @brief Evaluate every binding at @p elapsed and write into @p cfg.
        void ApplyAt(Config &cfg, float elapsed) const override;

        // --- RESTORE support ------------------------------------------
        // Generic implementation. For scalar bindings we snapshot per-binding
        // float values; for segment / segment-stop bindings we snapshot the
        // whole @c segmentBoosts vector (same reason SegmentTravel /
        // SegmentBounce do: bindings auto-grow the vector, so per-slot
        // snapshot loses the "vector was empty" case). The whole-struct copy
        // also covers the non-scalar segment fields (@c colorStops,
        // @c blendSpace) automatically. Call @ref CaptureBaseline BEFORE
        // @ref Play so the snapshot reflects the pre-animation value.
        void CaptureBaseline(const Config &cfg) override;

    protected:
        void RestoreBaseline(Config &cfg) const override;

    private:
        std::vector<ScalarBinding> mScalarBindings;
        std::vector<SegmentBinding> mSegmentBindings;
        std::vector<SegmentStopBinding> mSegmentStopBindings;
        /// One saved value per scalar binding, index-aligned with
        /// @c mScalarBindings at the moment @ref CaptureBaseline was called.
        std::vector<float> mSavedScalarValues;
        /// Snapshot of the whole @c segmentBoosts vector - covers every
        /// @c SegmentBoost field (position/length/boost + colorStops +
        /// blendSpace). Captured only when there is at least one segment or
        /// segment-stop binding at snapshot time; otherwise left empty and
        /// skipped on restore.
        std::vector<SegmentBoost> mSavedSegmentBoosts;
        /// Whether @ref CaptureBaseline captured @c mSavedSegmentBoosts (so
        /// we can distinguish "no segment bindings then" from "empty vector
        /// then").
        bool mSegmentBoostsCaptured = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
