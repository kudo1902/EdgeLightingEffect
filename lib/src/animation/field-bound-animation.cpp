#include "animation/field-bound-animation.h"

#include <algorithm>

namespace EdgeLighting
{
    namespace
    {
        void writeScalar(Config &cfg, AnimatableField field, float value)
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

        float readScalar(const Config &cfg, AnimatableField field)
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

        /// Grow @c cfg.neon.segmentBoosts up to @p index inclusive so a
        /// segment binding can safely write to @c segmentBoosts[index]. New
        /// entries are seeded with a visible length/boost so binding only
        /// @c POSITION still produces a visible moving spot.
        SegmentBoost &ensureSegmentSlot(Config &cfg, size_t index)
        {
            if (cfg.neon.segmentBoosts.size() <= index)
            {
                cfg.neon.segmentBoosts.resize(index + 1,
                                              SegmentBoost{0.0f, 0.15f, 4.0f});
            }
            return cfg.neon.segmentBoosts[index];
        }

        void writeSegment(Config &cfg, size_t index, SegmentField field, float value)
        {
            SegmentBoost &s = ensureSegmentSlot(cfg, index);
            switch (field)
            {
            case SegmentField::POSITION:
            {
                s.position = value;
                break;
            }
            case SegmentField::LENGTH:
            {
                s.length = value;
                break;
            }
            case SegmentField::BOOST:
            {
                s.boost = value;
                break;
            }
            }
        }
    } // namespace

    void FieldBoundAnimation::ApplyAt(Config &cfg, float elapsed) const
    {
        for (const ScalarBinding &b : mScalarBindings)
        {
            if (b.modulator)
            {
                writeScalar(cfg, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const SegmentBinding &b : mSegmentBindings)
        {
            if (b.modulator)
            {
                writeSegment(cfg, b.index, b.field, b.modulator->Evaluate(elapsed));
            }
        }
    }

    void FieldBoundAnimation::CaptureBaseline(const Config &cfg)
    {
        // Scalar bindings: index-aligned per-binding snapshot.
        mSavedScalarValues.clear();
        mSavedScalarValues.reserve(mScalarBindings.size());
        for (const ScalarBinding &b : mScalarBindings)
        {
            mSavedScalarValues.push_back(readScalar(cfg, b.field));
        }

        // Segment bindings: whole-vector snapshot when any segment binding
        // is present. Auto-grow means a per-slot save would drop the "vector
        // was empty at snapshot" case.
        if (!mSegmentBindings.empty())
        {
            mSavedSegmentBoosts = cfg.neon.segmentBoosts;
            mSegmentBoostsCaptured = true;
        }
        else
        {
            mSavedSegmentBoosts.clear();
            mSegmentBoostsCaptured = false;
        }
    }

    void FieldBoundAnimation::RestoreBaseline(Config &cfg) const
    {
        // Walk the pair-wise minimum: if scalar bindings were added or
        // removed since CaptureBaseline was called, only the bindings that
        // had a value captured get restored.
        const size_t n = std::min(mScalarBindings.size(), mSavedScalarValues.size());
        for (size_t i = 0; i < n; ++i)
        {
            writeScalar(cfg, mScalarBindings[i].field, mSavedScalarValues[i]);
        }
        if (mSegmentBoostsCaptured)
        {
            cfg.neon.segmentBoosts = mSavedSegmentBoosts;
        }
    }

} // namespace EdgeLighting
