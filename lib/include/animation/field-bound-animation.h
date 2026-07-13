#ifndef _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
#define _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_

#include "animation/animation.h"
#include "animation/modulator.h"
#include <utility>
#include <vector>

namespace EdgeLighting
{
    /// @brief Single-float Config leaves that a @ref FieldBoundAnimation can
    ///        drive.
    ///
    /// Enum values are numerically ABI-stable — the C API's @c EL_ConfigField
    /// mirrors them 1:1 and static_casts between the two. Append new fields at
    /// the end so both C and C++ callers stay forward-compatible.
    ///
    /// Segment fields target entry 0 of @c NeonConfig::segmentBoosts and
    /// auto-grow the vector to include it. For a specific index other than 0,
    /// write your own subclass or drive the vector directly from the host.
    typedef enum class AnimatableField
    {
        NEON_INTENSITY = 0,
        NEON_LINE_WIDTH = 1,
        NEON_GLOW_RADIUS = 2,
        NEON_BLOOM_STRENGTH = 3,
        NEON_FILAMENT_FALLOFF = 4,
        NEON_GLOW_SIDE_SOFTNESS = 5,
        NEON_HUE_ROTATION_RATE = 6,
        NEON_SEGMENT_POSITION = 7,
        NEON_SEGMENT_LENGTH = 8,
        NEON_SEGMENT_BOOST = 9,
        NEON_ARC_START = 10,
        NEON_ARC_LENGTH = 11,
    } AnimatableField;

    /// @brief Animation whose per-frame behaviour is defined at runtime by
    ///        binding modulators to @ref AnimatableField values.
    ///
    /// While the neon-animations.h subclasses each hard-code a specific field
    /// (e.g. @c IntensityPulse writes @c neon.intensity), @c FieldBoundAnimation
    /// carries a list of @ref FieldBinding entries. On each @ref ApplyAt every
    /// binding's modulator is evaluated with the same @c elapsed and the
    /// result is written to its bound field.
    ///
    /// The shared elapsed / duration / play state phase-locks all bindings —
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
        typedef struct FieldBinding
        {
            AnimatableField field;
            ModulatorPtr modulator;
        } FieldBinding;

        /// @brief Zero-binding animation. Extend via @ref AddField.
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
        ///       shared across bindings — pass the same @c ModulatorPtr to
        ///       drive several fields off one signal (or wrap in @ref Remap
        ///       per binding for per-field ranges).
        void AddField(AnimatableField field, ModulatorPtr modulator)
        {
            mBindings.push_back({field, std::move(modulator)});
        }

        /// @brief Read-only view of the current binding list.
        const std::vector<FieldBinding> &GetBindings() const { return mBindings; }

        /// @brief Number of bindings currently attached.
        size_t GetBindingCount() const { return mBindings.size(); }

        /// @brief Drop all bindings (leaves state / duration alone).
        void ClearBindings() { mBindings.clear(); }

        /// @brief Evaluate every binding at @p elapsed and write into @p cfg.
        void ApplyAt(Config &cfg, float elapsed) const override;

    private:
        std::vector<FieldBinding> mBindings;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FIELD_BOUND_ANIMATION_H_
