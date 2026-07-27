#ifndef _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
#define _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_

#include "animation/animation.h"
#include "animation/modulator.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace EdgeLighting
{
    /// @brief Scalar Config leaves that a @ref FieldBoundAnimation can drive
    ///        through @ref FieldBoundAnimation::AddField.
    ///
    /// Enum values are numerically ABI-stable - the C API's @c el_config_field_e
    /// mirrors them 1:1 (parity enforced by @c static_assert at the top of
    /// @c edge-lighting-capi.cpp). Append new fields at the end so both C
    /// and C++ callers stay forward-compatible.
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
    } AnimatableField;

    /// @brief Which scalar to drive inside a @c NeonConfig::segmentBoosts entry.
    ///
    /// Paired with an index at bind time via
    /// @ref FieldBoundAnimation::AddSegmentField. Numerically mirrored by
    /// @c el_segment_field_e in the C ABI.
    typedef enum class SegmentField
    {
        POSITION = 0,
        LENGTH = 1,
        BOOST = 2,
    } SegmentField;

    /// @brief Which scalar to drive inside a @c NeonConfig::arcs entry.
    ///
    /// Paired with an index at bind time via
    /// @ref FieldBoundAnimation::AddArcField. Numerically mirrored by
    /// @c el_arc_field_e in the C ABI.
    typedef enum class ArcField
    {
        START = 0,
        LENGTH = 1,
        INTENSITY = 2,
    } ArcField;

    /// @brief Which scalar to drive inside a single stop of
    ///        @c NeonConfig::segmentBoosts[segIdx].colorStops[stopIdx].
    ///
    /// Paired with @c (segIdx, stopIdx) at bind time via
    /// @ref FieldBoundAnimation::AddStopField (or @ref FieldBoundAnimation::AddArcStopField
    /// for arc-stop bindings). Numerically mirrored by
    /// @c el_color_stop_field_e in the C ABI.
    typedef enum class ColorStopField
    {
        POSITION = 0, ///< @c ColorStop::position - normalised offset within the segment span.
        R = 1,        ///< @c ColorStop::color.r
        G = 2,        ///< @c ColorStop::color.g
        B = 3,        ///< @c ColorStop::color.b
        A = 4,        ///< @c ColorStop::color.a
    } ColorStopField;

    /// @brief Animation whose per-frame behaviour is defined at runtime by
    ///        binding modulators to @ref AnimatableField values.
    ///
    /// While the neon-animations.h subclasses each hard-code a specific field
    /// (e.g. @c IntensityPulse writes @c neon.intensity), @c FieldBoundAnimation
    /// carries a list of bindings. Two kinds:
    ///
    /// - @ref AddField(AnimatableField, ModulatorPtr) - drive a scalar leaf.
    /// - @ref AddSegmentField(size_t, SegmentField, ModulatorPtr) - drive one
    ///   scalar inside @c NeonConfig::segmentBoosts[index]. The slot must
    ///   already exist; an out-of-range index is a logged no-op.
    /// - @ref AddStopField(size_t, size_t, ColorStopField, ModulatorPtr) -
    ///   drive one scalar inside
    ///   @c NeonConfig::segmentBoosts[segIdx].colorStops[stopIdx]. Both the
    ///   segment and the stop must already exist; out-of-range is a logged
    ///   no-op.
    ///
    /// On each @ref ApplyAt every binding's modulator is evaluated with the
    /// same @c elapsed and the result is written to its bound target.
    ///
    /// The shared elapsed / duration / play state phase-locks all bindings -
    /// use this class for "one animation, several fields moving together"
    /// (e.g. a heartbeat that pulses intensity + glow + bloom in unison). For
    /// independent parallel animations with different rates, put separate
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
    ///
    ///     // per frame:
    ///     breathing.Update(dt);
    ///     Config cfg = effect.GetConfig();
    ///     breathing.Apply(cfg);
    ///     effect.SetConfig(cfg);
    /// @endcode
    class FieldBoundAnimation : public Animation
    {
    public:
        /// @brief A single (field, modulator) pair driven by this animation.
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

        /// @brief A single (preservedSegmentBoosts{id}.field, modulator) binding.
        /// @details Addresses the preserved pool by stable id, so - unlike
        ///          @ref SegmentBinding - it never auto-grows anything: the
        ///          entry must already exist (acquired via
        ///          @c SegmentUtils::AcquireSegment). If no preserved entry has
        ///          @c id at apply time the binding is a no-op.
        typedef struct PreservedSegmentBinding
        {
            uint32_t id;
            SegmentField field;
            ModulatorPtr modulator;
        } PreservedSegmentBinding;

        /// @brief A single (preservedSegmentBoosts{id}.colorStops[stopIdx].field,
        ///        modulator) binding.
        /// @details Like @ref PreservedSegmentBinding, addresses the preserved
        ///          entry by stable id and grows nothing: neither a missing id
        ///          nor a @c stopIndex past the entry's current stop count is
        ///          created - both are logged no-ops at apply time.
        typedef struct PreservedSegmentStopBinding
        {
            uint32_t id;
            size_t stopIndex;
            ColorStopField field;
            ModulatorPtr modulator;
        } PreservedSegmentStopBinding;

        /// @brief A single (segmentBoosts[segIdx].colorStops[stopIdx].field,
        ///        modulator) binding.
        typedef struct SegmentStopBinding
        {
            size_t segIndex;
            size_t stopIndex;
            ColorStopField field;
            ModulatorPtr modulator;
        } SegmentStopBinding;

        /// @brief A single (arcs[index].field, modulator) binding.
        typedef struct ArcBinding
        {
            size_t index;
            ArcField field;
            ModulatorPtr modulator;
        } ArcBinding;

        /// @brief A single (arcs[arcIdx].colorStops[stopIdx].field,
        ///        modulator) binding.
        typedef struct ArcStopBinding
        {
            size_t arcIndex;
            size_t stopIndex;
            ColorStopField field;
            ModulatorPtr modulator;
        } ArcStopBinding;

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

        /// @brief One-binding convenience constructor.
        FieldBoundAnimation(AnimatableField field,
                            ModulatorPtr modulator,
                            float duration = 0.0f,
                            PlaybackMode mode = PlaybackMode::LOOP)
            : FieldBoundAnimation(duration, mode)
        {
            AddField(field, std::move(modulator));
        }

        /// @brief Bind an additional field to a modulator.
        /// @note Bindings are evaluated in insertion order. Modulators can be
        ///       shared across bindings - pass the same @c ModulatorPtr to
        ///       drive several fields off one signal (or wrap in @ref Remap
        ///       per binding for per-field ranges).
        void AddField(AnimatableField field, ModulatorPtr modulator)
        {
            mScalarBindings.push_back({field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside @c segmentBoosts[index] to a modulator.
        /// @details No auto-grow: @c segmentBoosts must already hold @p index
        ///          (size it via the C API first). A binding whose @p index is
        ///          out of range at apply time is a logged no-op.
        void AddSegmentField(size_t index, SegmentField field, ModulatorPtr modulator)
        {
            mSegmentBindings.push_back({index, field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside the preserved-pool entry owning @p id.
        /// @details Unlike @ref AddSegmentField this never creates the entry -
        ///          acquire it first via @c SegmentUtils::AcquireSegment (or the
        ///          C API's @c el_effect_acquire_preserved_segment). At apply
        ///          time the entry is looked up by id, so it is immune to
        ///          reindexing and to overrides of the transient pool. A binding
        ///          whose id is not found is silently skipped.
        void AddPreservedSegmentField(uint32_t id, SegmentField field, ModulatorPtr modulator)
        {
            mPreservedSegmentBindings.push_back({id, field, std::move(modulator)});
        }

        /// @brief Bind a colour-stop channel of the preserved entry owning @p id.
        /// @details Nothing auto-grows: acquire the entry and size its stops
        ///          first. A binding to a released id, or to a @p stopIdx past
        ///          the current stop count, is a skipped no-op (unlike
        ///          @ref AddStopField, which grows the slot).
        void AddPreservedSegmentStopField(uint32_t id, size_t stopIdx,
                                          ColorStopField field, ModulatorPtr modulator)
        {
            mPreservedSegmentStopBindings.push_back({id, stopIdx, field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside
        ///        @c segmentBoosts[segIdx].colorStops[stopIdx] to a modulator.
        /// @details No auto-grow: both @p segIdx and, within that segment,
        ///          @p stopIdx must already exist (size the segment's stops via
        ///          the C API first). An out-of-range segment or stop is a
        ///          logged no-op at apply time.
        void AddStopField(size_t segIdx, size_t stopIdx,
                          ColorStopField field, ModulatorPtr modulator)
        {
            mSegmentStopBindings.push_back({segIdx, stopIdx, field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside @c arcs[index] to a modulator.
        /// @details No auto-grow: @c cfg.neon.arcs must already hold @p index. A
        ///          binding whose @p index is out of range at apply time is a
        ///          logged no-op.
        void AddArcField(size_t index, ArcField field, ModulatorPtr modulator)
        {
            mArcBindings.push_back({index, field, std::move(modulator)});
        }

        /// @brief Bind a scalar inside
        ///        @c arcs[arcIdx].colorStops[stopIdx] to a modulator.
        /// @details No auto-grow: both @p arcIdx and, within that arc,
        ///          @p stopIdx must already exist. An out-of-range arc or stop
        ///          is a logged no-op at apply time.
        void AddArcStopField(size_t arcIdx, size_t stopIdx,
                             ColorStopField field, ModulatorPtr modulator)
        {
            mArcStopBindings.push_back({arcIdx, stopIdx, field, std::move(modulator)});
        }

        // --- Introspection -----------------------------------------------

        /// @brief Read-only view of the scalar-field bindings.
        const std::vector<ScalarBinding> &GetScalarBindings() const { return mScalarBindings; }

        /// @brief Read-only view of the segment-field bindings.
        const std::vector<SegmentBinding> &GetSegmentBindings() const { return mSegmentBindings; }

        /// @brief Read-only view of the preserved-segment-field bindings.
        const std::vector<PreservedSegmentBinding> &GetPreservedSegmentBindings() const
        {
            return mPreservedSegmentBindings;
        }

        /// @brief Read-only view of the preserved-segment-stop-field bindings.
        const std::vector<PreservedSegmentStopBinding> &GetPreservedSegmentStopBindings() const
        {
            return mPreservedSegmentStopBindings;
        }

        /// @brief Read-only view of the segment-stop-field bindings.
        const std::vector<SegmentStopBinding> &GetSegmentStopBindings() const
        {
            return mSegmentStopBindings;
        }

        /// @brief Read-only view of the arc-field bindings.
        const std::vector<ArcBinding> &GetArcBindings() const { return mArcBindings; }

        /// @brief Read-only view of the arc-stop-field bindings.
        const std::vector<ArcStopBinding> &GetArcStopBindings() const
        {
            return mArcStopBindings;
        }

        /// @brief Total number of bindings across every kind.
        size_t GetBindingCount() const
        {
            return mScalarBindings.size() + mSegmentBindings.size() +
                   mPreservedSegmentBindings.size() + mPreservedSegmentStopBindings.size() +
                   mSegmentStopBindings.size() + mArcBindings.size() +
                   mArcStopBindings.size();
        }

        /// @brief Drop every binding (leaves state / duration alone).
        void ClearBindings()
        {
            mScalarBindings.clear();
            mSegmentBindings.clear();
            mPreservedSegmentBindings.clear();
            mPreservedSegmentStopBindings.clear();
            mSegmentStopBindings.clear();
            mArcBindings.clear();
            mArcStopBindings.clear();
        }

        // --- Drive -------------------------------------------------------

        /// @brief Evaluate every binding at @p elapsed and write into @p cfg.
        void ApplyAt(Config &cfg, float elapsed) const override;

        // --- RESTORE support ------------------------------------------
        // Generic implementation. For scalar bindings we snapshot per-binding
        // float values; for segment / segment-stop bindings we snapshot the
        // whole @c segmentBoosts vector. The whole-vector copy sidesteps per-slot
        // bookkeeping and also covers the non-scalar segment fields
        // (@c colorStops, @c blendSpace) automatically. Call @ref CaptureBaseline
        // BEFORE @ref Play so the snapshot reflects the pre-animation value.
        void CaptureBaseline(const Config &cfg) override;

    protected:
        void RestoreBaseline(Config &cfg) const override;

    private:
        std::vector<ScalarBinding> mScalarBindings;
        std::vector<SegmentBinding> mSegmentBindings;
        std::vector<PreservedSegmentBinding> mPreservedSegmentBindings;
        std::vector<PreservedSegmentStopBinding> mPreservedSegmentStopBindings;
        std::vector<SegmentStopBinding> mSegmentStopBindings;
        std::vector<ArcBinding> mArcBindings;
        std::vector<ArcStopBinding> mArcStopBindings;
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
        /// Snapshot of the whole @c preservedSegmentBoosts vector - same
        /// rationale as @c mSavedSegmentBoosts, captured only when there is at
        /// least one preserved-segment binding.
        std::vector<PreservedSegment> mSavedPreservedSegmentBoosts;
        bool mPreservedSegmentBoostsCaptured = false;
        /// Snapshot of the whole @c arcs vector - same rationale as
        /// mSavedSegmentBoosts.
        std::vector<Arc> mSavedArcs;
        bool mArcsCaptured = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
