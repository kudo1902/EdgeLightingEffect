#ifndef _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_
#define _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_

#include "animation/animation.h"
#include "animation/neon-animations.h"
#include "animation/field-bound-animation.h"
#include "animation/modulator.h"
#include <memory>

namespace EdgeLightingDemo
{
    /// Built-in animation showcase. Each preset is a self-contained composition
    /// of @ref EdgeLighting::Animation / @ref EdgeLighting::Modulator pieces;
    /// the debug UI flips between them to demonstrate what the system can do.
    typedef enum class AnimationPreset
    {
        NONE,                ///< No animation - Config is used verbatim.
        BREATHING,           ///< Slow sine pulse on intensity. Calm "alive" look.
        STROBE,              ///< Hard 6 Hz on/off square wave on intensity.
        HEARTBEAT,           ///< lub-DUB rhythm on intensity, rest, loop.
        SHIMMER,             ///< Intensity + glow radius pulse in phase, fast.
        AURORA,              ///< Very slow layered motion across multiple params.
        REVERSE_SWEEP,       ///< Hue ring sweeps forwards then backwards, smoothly.
        FADE_IN,             ///< One-shot ease-in of intensity from 0 to 1.
        SEGMENT_TRAVEL,      ///< Bright Gaussian spot revolves around the perimeter.
        SEGMENT_BOUNCE,      ///< Bright spot swings back and forth (triangle wave).
        COMET,               ///< Tight fast spot - single-revolution comet feel.
        OUTLINE_TRACER,      ///< One-shot: rect dark, then arc grows 0→1 to light it.
        OUTLINE_TRACER_WRAP, ///< One-shot: arc grows 0→1.5, past full, head wraps.
        OUTLINE_COLLAPSE,    ///< One-shot: arc shrinks 1→0 to erase it.
        ARC_WIPE,            ///< One-shot: 3-phase grow/chase/shrink wipe around perimeter.
        FADE_OUT,            ///< One-shot ease-out of intensity to 0.
        HUE_REVERSE,         ///< Hue direction flips abruptly every few seconds.
        LENS_SUN_ORBIT,      ///< Lens-flare sun rides all the way around the perimeter.
        LENS_SUN_BOUNCE,     ///< Lens-flare sun swings back and forth along the perimeter.
        LENS_FLARE_PULSE,    ///< Lens-flare intensity breathes in and out.
        LENS_SUN_SWEEP,      ///< Sun orbits while the flare pulses and its core breathes.
        COUNT
    } AnimationPreset;

    inline const char *PresetName(AnimationPreset p)
    {
        switch (p)
        {
        case AnimationPreset::NONE:
        {
            return "None";
        }
        case AnimationPreset::BREATHING:
        {
            return "Breathing";
        }
        case AnimationPreset::STROBE:
        {
            return "Strobe";
        }
        case AnimationPreset::HEARTBEAT:
        {
            return "Heartbeat";
        }
        case AnimationPreset::SHIMMER:
        {
            return "Shimmer";
        }
        case AnimationPreset::AURORA:
        {
            return "Aurora";
        }
        case AnimationPreset::REVERSE_SWEEP:
        {
            return "Reverse Sweep";
        }
        case AnimationPreset::FADE_IN:
        {
            return "Fade In";
        }
        case AnimationPreset::SEGMENT_TRAVEL:
        {
            return "Segment Travel";
        }
        case AnimationPreset::SEGMENT_BOUNCE:
        {
            return "Segment Bounce";
        }
        case AnimationPreset::COMET:
        {
            return "Comet";
        }
        case AnimationPreset::OUTLINE_TRACER:
        {
            return "Outline Tracer";
        }
        case AnimationPreset::OUTLINE_TRACER_WRAP:
        {
            return "Outline Tracer (Wrap)";
        }
        case AnimationPreset::OUTLINE_COLLAPSE:
        {
            return "Outline Collapse";
        }
        case AnimationPreset::ARC_WIPE:
        {
            return "Arc Wipe";
        }
        case AnimationPreset::FADE_OUT:
        {
            return "Fade Out";
        }
        case AnimationPreset::HUE_REVERSE:
        {
            return "Hue Reverse";
        }
        case AnimationPreset::LENS_SUN_ORBIT:
        {
            return "Lens Sun Orbit";
        }
        case AnimationPreset::LENS_SUN_BOUNCE:
        {
            return "Lens Sun Bounce";
        }
        case AnimationPreset::LENS_FLARE_PULSE:
        {
            return "Lens Flare Pulse";
        }
        case AnimationPreset::LENS_SUN_SWEEP:
        {
            return "Lens Sun Sweep";
        }
        default:
        {
            return "?";
        }
        }
    }

    /// Build a fresh animation instance for @p preset.
    /// Returns nullptr for AnimationPreset::NONE.
    inline EdgeLighting::AnimationPtr CreateAnimation(AnimationPreset preset)
    {
        using namespace EdgeLighting;

        switch (preset)
        {
        case AnimationPreset::NONE:
        {
            return nullptr;
        }

        case AnimationPreset::BREATHING:
        {
            // Calm ~1.67 s sine - about 36 BPM.
            return std::make_shared<IntensityPulse>(1.0f / 0.6f, 0.4f, 1.0f);
        }

        case AnimationPreset::STROBE:
        {
            // 6 on+off cycles per second.
            return std::make_shared<IntensityStrobe>(1.0f / 6.0f, 0.0f, 1.0f);
        }

        case AnimationPreset::HEARTBEAT:
        {
            // Two quick beats ("lub-DUB") followed by a rest, ~1.0 s per cycle.
            auto seq = std::make_shared<Sequence>();
            seq->Append(std::make_shared<Ease>(0.30f, 1.00f, 0.08f, EasingFunction::OutCubic), 0.08f); // lub rise
            seq->Append(std::make_shared<Ease>(1.00f, 0.45f, 0.10f, EasingFunction::InCubic), 0.10f);  // lub fall
            seq->Append(std::make_shared<Ease>(0.45f, 1.00f, 0.08f, EasingFunction::OutCubic), 0.08f); // DUB rise
            seq->Append(std::make_shared<Ease>(1.00f, 0.30f, 0.20f, EasingFunction::InCubic), 0.20f);  // DUB fall
            seq->Append(std::make_shared<Constant>(0.30f), 0.54f);                                     // rest
            seq->SetLoop(true);
            return std::make_shared<IntensityCurve>(seq);
        }

        case AnimationPreset::SHIMMER:
        {
            // Intensity + glow radius pulse together every 0.5 s - gives the
            // line a fast "twinkle". Use AnimationGroup to stack.
            auto group = std::make_shared<AnimationGroup>();
            group->Add(std::make_shared<IntensityPulse>(0.5f, 0.65f, 1.0f));
            group->Add(std::make_shared<GlowRadiusBreath>(0.5f, 5.0f, 10.0f));
            return group;
        }

        case AnimationPreset::AURORA:
        {
            // Slow, atmospheric motion across three params at different
            // periods so they drift in and out of phase.
            auto group = std::make_shared<AnimationGroup>();
            group->Add(std::make_shared<IntensityPulse>(10.0f, 0.75f, 1.00f));
            group->Add(std::make_shared<GlowRadiusBreath>(1.0f / 0.15f, 8.0f, 24.0f));
            group->Add(std::make_shared<BloomPulse>(5.0f, 0.20f, 0.70f));
            return group;
        }

        case AnimationPreset::REVERSE_SWEEP:
        {
            // Triangle wave between -0.8 and +0.8 over 6 s - smooth direction flip.
            return std::make_shared<HueRotationEaseReverse>(0.8f, 6.0f);
        }

        case AnimationPreset::FADE_IN:
        {
            return std::make_shared<IntensityFadeIn>(1.0f, 1.5f, EdgeLighting::EasingFunction::OutCubic);
        }

        case AnimationPreset::SEGMENT_TRAVEL:
        {
            // Comfortable spot, one revolution every 3 s.
            return std::make_shared<SegmentTravel>(3.0f, 0.15f, 4.0f);
        }

        case AnimationPreset::SEGMENT_BOUNCE:
        {
            // Wider spot swinging back and forth.
            return std::make_shared<SegmentBounce>(4.0f, 0.20f, 3.5f);
        }

        case AnimationPreset::COMET:
        {
            // Tight, fast, bright spot - half a second per loop.
            return std::make_shared<SegmentTravel>(0.6f, 0.05f, 6.0f);
        }

        case AnimationPreset::OUTLINE_TRACER:
        {
            // One-shot 2 s draw - rect goes from dark to fully lit.
            return std::make_shared<OutlineTracer>(2.0f, EdgeLighting::EasingFunction::OutCubic);
        }

        case AnimationPreset::OUTLINE_TRACER_WRAP:
        {
            // One-shot 2 s draw past full - length sweeps 0 -> 1.5 so the head
            // keeps wrapping past the tail once the whole outline is lit.
            return std::make_shared<OutlineTracer>(2.0f, EdgeLighting::EasingFunction::OutCubic, 1.1f);
        }

        case AnimationPreset::OUTLINE_COLLAPSE:
        {
            // One-shot 2 s erase - rect goes from fully lit to dark.
            return std::make_shared<OutlineCollapse>(2.0f, EdgeLighting::EasingFunction::InCubic);
        }

        case AnimationPreset::ARC_WIPE:
        {
            // Full-loop wipe (startPos == endPos): race a 0.25-long arc all
            // the way around the perimeter, back to the origin over 3 s.
            // Phase timings (with default Linear ease) work out to grow≈0.6s /
            // chase≈1.8s / shrink≈0.6s (grow and shrink equal because the
            // head and tail move at the same speed throughout).
            return std::make_shared<ArcWipe>(
                /*duration=*/3.0f,
                /*startPos=*/0.1f,
                /*endPos=*/0.1f,
                /*maxLength=*/0.5f,
                EdgeLighting::EasingFunction::Linear);
        }

        case AnimationPreset::FADE_OUT:
        {
            // One-shot fade-out from full intensity to 0 over 2 seconds.
            return std::make_shared<IntensityFadeOut>(1.0f, 2.0f, EdgeLighting::EasingFunction::InCubic);
        }

        case AnimationPreset::HUE_REVERSE:
        {
            // Abrupt direction flip every 3 seconds (6s full cycle).
            return std::make_shared<HueRotationReverse>(0.4f, 6.0f);
        }

        case AnimationPreset::LENS_SUN_ORBIT:
        {
            // Field-bound: a sawtooth drives lensFlare.perimeterPosition 0->1,
            // so the sun rides once around the perimeter every 6 s. This is the
            // template for a custom lens-flare animation - bind any field to any
            // modulator; swap the field or the waveform to taste.
            auto anim = std::make_shared<FieldBoundAnimation>();
            anim->AddField(AnimatableField::LENS_FLARE_PERIMETER_POSITION,
                           std::make_shared<Oscillator>(1.0f / 6.0f, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH));
            return anim;
        }

        case AnimationPreset::LENS_SUN_BOUNCE:
        {
            // Triangle wave: the sun swings across the perimeter and back over 5 s.
            auto anim = std::make_shared<FieldBoundAnimation>();
            anim->AddField(AnimatableField::LENS_FLARE_PERIMETER_POSITION,
                           std::make_shared<Oscillator>(1.0f / 5.0f, 0.0f, 1.0f, 0.0f, Waveform::TRIANGLE));
            return anim;
        }

        case AnimationPreset::LENS_FLARE_PULSE:
        {
            // Sine on intensity: the whole flare breathes 0.5x..1.4x every 2 s.
            auto anim = std::make_shared<FieldBoundAnimation>();
            anim->AddField(AnimatableField::LENS_FLARE_INTENSITY,
                           std::make_shared<Oscillator>(1.0f / 2.0f, 0.5f, 1.4f));
            return anim;
        }

        case AnimationPreset::LENS_SUN_SWEEP:
        {
            // Signature combo, all in ONE FieldBoundAnimation: three fields,
            // three independent modulators on their own periods so they drift
            // out of phase. Shows how a user composes a multi-field lens-flare
            // animation without any dedicated subclass.
            auto anim = std::make_shared<FieldBoundAnimation>();
            anim->AddField(AnimatableField::LENS_FLARE_PERIMETER_POSITION,
                           std::make_shared<Oscillator>(1.0f / 8.0f, 0.0f, 1.0f, 0.0f, Waveform::SAWTOOTH));
            anim->AddField(AnimatableField::LENS_FLARE_INTENSITY,
                           std::make_shared<Oscillator>(1.0f / 2.5f, 0.6f, 1.3f));
            anim->AddField(AnimatableField::LENS_FLARE_SIZE,
                           std::make_shared<Oscillator>(1.0f / 3.5f, 0.85f, 1.4f));
            return anim;
        }

        default:
        {
            return nullptr;
        }
        }
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_
