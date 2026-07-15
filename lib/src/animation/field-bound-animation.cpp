#include "animation/field-bound-animation.h"

namespace EdgeLighting
{
    namespace
    {
        /// Ensure segmentBoosts has at least one entry so segment-scalar
        /// bindings have something to write to. Seeded with a visible boost
        /// so binding only `NEON_SEGMENT_POSITION` still shows a moving spot.
        SegmentBoost &ensureFirstSegment(Config &cfg)
        {
            if (cfg.neon.segmentBoosts.empty())
            {
                cfg.neon.segmentBoosts.push_back({0.0f, 0.15f, 4.0f});
            }
            return cfg.neon.segmentBoosts.front();
        }

        void writeField(Config &cfg, AnimatableField field, float value)
        {
            switch (field)
            {
            case AnimatableField::NEON_INTENSITY:
            {
                cfg.neon.intensity = value;
                break;
            }
            case AnimatableField::NEON_LINE_WIDTH:
            {
                cfg.neon.lineWidth = value;
                break;
            }
            case AnimatableField::NEON_GLOW_RADIUS:
            {
                cfg.neon.glowRadius = value;
                break;
            }
            case AnimatableField::NEON_BLOOM_STRENGTH:
            {
                cfg.neon.bloomStrength = value;
                break;
            }
            case AnimatableField::NEON_FILAMENT_FALLOFF:
            {
                cfg.neon.filamentFalloff = value;
                break;
            }
            case AnimatableField::NEON_GLOW_SIDE_SOFTNESS:
            {
                cfg.neon.glowSideSoftness = value;
                break;
            }
            case AnimatableField::NEON_HUE_ROTATION_RATE:
            {
                cfg.neon.hueRotationRate = value;
                break;
            }
            case AnimatableField::NEON_SEGMENT_POSITION:
            {
                ensureFirstSegment(cfg).position = value;
                break;
            }
            case AnimatableField::NEON_SEGMENT_LENGTH:
            {
                ensureFirstSegment(cfg).length = value;
                break;
            }
            case AnimatableField::NEON_SEGMENT_BOOST:
            {
                ensureFirstSegment(cfg).boost = value;
                break;
            }
            case AnimatableField::NEON_ARC_START:
            {
                cfg.neon.arcStart = value;
                break;
            }
            case AnimatableField::NEON_ARC_LENGTH:
            {
                cfg.neon.arcLength = value;
                break;
            }
            }
        }
    } // namespace

    void FieldBoundAnimation::ApplyAt(Config &cfg, float elapsed) const
    {
        for (const FieldBinding &b : mBindings)
        {
            if (b.modulator)
            {
                writeField(cfg, b.field, b.modulator->Evaluate(elapsed));
            }
        }
    }

} // namespace EdgeLighting
