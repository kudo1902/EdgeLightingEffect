#include "animation/field-bound-animation.h"

#include <algorithm>

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

        /// Read the current float value of the bound field. Mirrors writeField.
        /// For segment scalars, an empty segmentBoosts is treated as 0 - the
        /// snapshot captures "no segment yet", and RestoreBaseline puts that
        /// zero back into the same slot (writeField will grow the vector as
        /// needed, matching the pre-play state that only had the seed values).
        float readField(const Config &cfg, AnimatableField field)
        {
            switch (field)
            {
            case AnimatableField::NEON_INTENSITY:
            {
                return cfg.neon.intensity;
            }
            case AnimatableField::NEON_LINE_WIDTH:
            {
                return cfg.neon.lineWidth;
            }
            case AnimatableField::NEON_GLOW_RADIUS:
            {
                return cfg.neon.glowRadius;
            }
            case AnimatableField::NEON_BLOOM_STRENGTH:
            {
                return cfg.neon.bloomStrength;
            }
            case AnimatableField::NEON_FILAMENT_FALLOFF:
            {
                return cfg.neon.filamentFalloff;
            }
            case AnimatableField::NEON_GLOW_SIDE_SOFTNESS:
            {
                return cfg.neon.glowSideSoftness;
            }
            case AnimatableField::NEON_HUE_ROTATION_RATE:
            {
                return cfg.neon.hueRotationRate;
            }
            case AnimatableField::NEON_SEGMENT_POSITION:
            {
                return cfg.neon.segmentBoosts.empty() ? 0.0f : cfg.neon.segmentBoosts.front().position;
            }
            case AnimatableField::NEON_SEGMENT_LENGTH:
            {
                return cfg.neon.segmentBoosts.empty() ? 0.0f : cfg.neon.segmentBoosts.front().length;
            }
            case AnimatableField::NEON_SEGMENT_BOOST:
            {
                return cfg.neon.segmentBoosts.empty() ? 0.0f : cfg.neon.segmentBoosts.front().boost;
            }
            case AnimatableField::NEON_ARC_START:
            {
                return cfg.neon.arcStart;
            }
            case AnimatableField::NEON_ARC_LENGTH:
            {
                return cfg.neon.arcLength;
            }
            }
            return 0.0f;
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

    void FieldBoundAnimation::CaptureBaseline(const Config &cfg)
    {
        mSavedValues.clear();
        mSavedValues.reserve(mBindings.size());
        for (const FieldBinding &b : mBindings)
        {
            mSavedValues.push_back(readField(cfg, b.field));
        }
    }

    void FieldBoundAnimation::RestoreBaseline(Config &cfg) const
    {
        // Walk the pair-wise minimum: if bindings were added or removed since
        // CaptureBaseline was called, only the bindings that had a value
        // captured get restored.
        const size_t n = std::min(mBindings.size(), mSavedValues.size());
        for (size_t i = 0; i < n; ++i)
        {
            writeField(cfg, mBindings[i].field, mSavedValues[i]);
        }
    }

} // namespace EdgeLighting
