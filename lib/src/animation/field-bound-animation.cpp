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
            }
            return 0.0f;
        }

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

        ColorStop &ensureStopSlot(SegmentBoost &seg, size_t stopIdx)
        {
            if (seg.colorStops.size() <= stopIdx)
            {
                seg.colorStops.resize(stopIdx + 1,
                                      ColorStop{0.5f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
            }
            return seg.colorStops[stopIdx];
        }

        void writeStop(Config &cfg, size_t segIdx, size_t stopIdx,
                       ColorStopField field, float value)
        {
            SegmentBoost &s = ensureSegmentSlot(cfg, segIdx);
            ColorStop &c = ensureStopSlot(s, stopIdx);
            switch (field)
            {
            case ColorStopField::POSITION:
            {
                c.position = value;
                break;
            }
            case ColorStopField::R:
            {
                c.color.r = value;
                break;
            }
            case ColorStopField::G:
            {
                c.color.g = value;
                break;
            }
            case ColorStopField::B:
            {
                c.color.b = value;
                break;
            }
            case ColorStopField::A:
            {
                c.color.a = value;
                break;
            }
            }
        }

        Arc &ensureArcSlot(Config &cfg, size_t index)
        {
            if (cfg.neon.arcs.size() <= index)
            {
                cfg.neon.arcs.resize(index + 1, Arc{});
            }
            return cfg.neon.arcs[index];
        }

        void writeArc(Config &cfg, size_t index, ArcField field, float value)
        {
            Arc &a = ensureArcSlot(cfg, index);
            switch (field)
            {
            case ArcField::START:
            {
                a.start = value;
                break;
            }
            case ArcField::LENGTH:
            {
                a.length = value;
                break;
            }
            case ArcField::INTENSITY:
            {
                a.intensity = value;
                break;
            }
            }
        }

        ColorStop &ensureArcStopSlot(Arc &arc, size_t stopIdx)
        {
            if (arc.colorStops.size() <= stopIdx)
            {
                arc.colorStops.resize(stopIdx + 1,
                                      ColorStop{0.5f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
            }
            return arc.colorStops[stopIdx];
        }

        void writeArcStop(Config &cfg, size_t arcIdx, size_t stopIdx,
                          ColorStopField field, float value)
        {
            Arc &a = ensureArcSlot(cfg, arcIdx);
            ColorStop &c = ensureArcStopSlot(a, stopIdx);
            switch (field)
            {
            case ColorStopField::POSITION:
            {
                c.position = value;
                break;
            }
            case ColorStopField::R:
            {
                c.color.r = value;
                break;
            }
            case ColorStopField::G:
            {
                c.color.g = value;
                break;
            }
            case ColorStopField::B:
            {
                c.color.b = value;
                break;
            }
            case ColorStopField::A:
            {
                c.color.a = value;
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
        for (const SegmentStopBinding &b : mSegmentStopBindings)
        {
            if (b.modulator)
            {
                writeStop(cfg, b.segIndex, b.stopIndex, b.field,
                          b.modulator->Evaluate(elapsed));
            }
        }
        for (const ArcBinding &b : mArcBindings)
        {
            if (b.modulator)
            {
                writeArc(cfg, b.index, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const ArcStopBinding &b : mArcStopBindings)
        {
            if (b.modulator)
            {
                writeArcStop(cfg, b.arcIndex, b.stopIndex, b.field,
                             b.modulator->Evaluate(elapsed));
            }
        }
    }

    void FieldBoundAnimation::CaptureBaseline(const Config &cfg)
    {
        mSavedScalarValues.clear();
        mSavedScalarValues.reserve(mScalarBindings.size());
        for (const ScalarBinding &b : mScalarBindings)
        {
            mSavedScalarValues.push_back(readScalar(cfg, b.field));
        }

        if (!mSegmentBindings.empty() || !mSegmentStopBindings.empty())
        {
            mSavedSegmentBoosts = cfg.neon.segmentBoosts;
            mSegmentBoostsCaptured = true;
        }
        else
        {
            mSavedSegmentBoosts.clear();
            mSegmentBoostsCaptured = false;
        }

        if (!mArcBindings.empty() || !mArcStopBindings.empty())
        {
            mSavedArcs = cfg.neon.arcs;
            mArcsCaptured = true;
        }
        else
        {
            mSavedArcs.clear();
            mArcsCaptured = false;
        }
    }

    void FieldBoundAnimation::RestoreBaseline(Config &cfg) const
    {
        const size_t n = std::min(mScalarBindings.size(), mSavedScalarValues.size());
        for (size_t i = 0; i < n; ++i)
        {
            writeScalar(cfg, mScalarBindings[i].field, mSavedScalarValues[i]);
        }
        if (mSegmentBoostsCaptured)
        {
            cfg.neon.segmentBoosts = mSavedSegmentBoosts;
        }
        if (mArcsCaptured)
        {
            cfg.neon.arcs = mSavedArcs;
        }
    }

} // namespace EdgeLighting
