#ifndef _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_
#define _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_

#include "animation/animation.h"
#include "animation/neon-animations.h"
#include "animation/modulator.h"
#include <memory>

namespace EdgeLightingDemo
{
    /// Built-in animation showcase. Each preset is a self-contained composition
    /// of @ref EdgeLighting::Animation / @ref EdgeLighting::Modulator pieces;
    /// the debug UI flips between them to demonstrate what the system can do.
    typedef enum class AnimationPreset
    {
        NONE,           ///< No animation — Config is used verbatim.
        BREATHING,      ///< Slow sine pulse on intensity. Calm "alive" look.
        STROBE,         ///< Hard 6 Hz on/off square wave on intensity.
        HEARTBEAT,      ///< lub-DUB rhythm on intensity, rest, loop.
        SHIMMER,        ///< Intensity + glow radius pulse in phase, fast.
        AURORA,         ///< Very slow layered motion across multiple params.
        FADE_IN,        ///< One-shot ease-in of intensity from 0 to 1.
        SEGMENT_TRAVEL, ///< Bright Gaussian spot revolves around the perimeter.
        SEGMENT_BOUNCE, ///< Bright spot swings back and forth (triangle wave).
        COMET,          ///< Tight fast spot — single-revolution comet feel.
        SIDE_EDGES,     ///< Only the two side edges lit; the pair breathes.
        SIDE_EDGES_DUO, ///< Two side edges, own colours + travelling white spots.
        OUTLINE_TRACER, ///< One-shot: rect dark, then arc grows 0→1 to light it.
        FADE_OUT,       ///< One-shot ease-out of intensity to 0.
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
        case AnimationPreset::SIDE_EDGES:
        {
            return "Side Edges";
        }
        case AnimationPreset::SIDE_EDGES_DUO:
        {
            return "Side Edges Duo";
        }
        case AnimationPreset::OUTLINE_TRACER:
        {
            return "Outline Tracer";
        }
        case AnimationPreset::FADE_OUT:
        {
            return "Fade Out";
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
            // Calm ~1.67 s sine — about 36 BPM.
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
            // Intensity + glow radius pulse together every 0.5 s — gives the
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
            // Tight, fast, bright spot — half a second per loop.
            return std::make_shared<SegmentTravel>(0.6f, 0.05f, 6.0f);
        }

        case AnimationPreset::SIDE_EDGES:
        {
            // Two bars on the left + right edges (rest dark), gently breathing
            // together. SideEdges sets the gate/geometry; IntensityPulse drives
            // the shared brightness.
            auto group = std::make_shared<AnimationGroup>();
            group->Add(std::make_shared<SideEdges>(0.95f, 1.0f));
            group->Add(std::make_shared<IntensityPulse>(2.0f, 0.5f, 1.0f));
            return group;
        }

        case AnimationPreset::SIDE_EDGES_DUO:
        {
            // Per-segment colour + nested travelling spots: left red->orange,
            // right blue->cyan, each with a white accent sliding along it.
            return std::make_shared<SideEdgesAccent>(2.5f);
        }

        case AnimationPreset::OUTLINE_TRACER:
        {
            // One-shot 2 s draw — rect goes from dark to fully lit.
            return std::make_shared<OutlineTracer>(2.0f, EdgeLighting::EasingFunction::OutCubic);
        }

        case AnimationPreset::FADE_OUT:
        {
            // One-shot fade-out from full intensity to 0 over 2 seconds.
            return std::make_shared<IntensityFadeOut>(1.0f, 2.0f, EdgeLighting::EasingFunction::InCubic);
        }

        default:
        {
            return nullptr;
        }
        }
    }

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_ANIMATION_PRESETS_H_
