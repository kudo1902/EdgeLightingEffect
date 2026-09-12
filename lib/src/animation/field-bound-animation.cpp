#include "animation/field-bound-animation.h"
#include "animation/field-access.h"

#include <algorithm>

namespace EdgeLighting
{
    void FieldBoundAnimation::ApplyAt(Config &cfg, float elapsed) const
    {
        for (const ScalarBinding &b : mScalarBindings)
        {
            if (b.modulator)
            {
                WriteField(cfg, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const SegmentBinding &b : mSegmentBindings)
        {
            if (b.modulator)
            {
                WriteSegmentField(cfg, b.index, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const PreservedSegmentBinding &b : mPreservedSegmentBindings)
        {
            if (b.modulator)
            {
                WritePreservedSegmentField(cfg, b.id, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const PreservedSegmentStopBinding &b : mPreservedSegmentStopBindings)
        {
            if (b.modulator)
            {
                WritePreservedSegmentStopField(cfg, b.id, b.stopIndex, b.field,
                                               b.modulator->Evaluate(elapsed));
            }
        }
        for (const SegmentStopBinding &b : mSegmentStopBindings)
        {
            if (b.modulator)
            {
                WriteSegmentStopField(cfg, b.segIndex, b.stopIndex, b.field,
                                      b.modulator->Evaluate(elapsed));
            }
        }
        for (const ArcBinding &b : mArcBindings)
        {
            if (b.modulator)
            {
                WriteArcField(cfg, b.index, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const ArcStopBinding &b : mArcStopBindings)
        {
            if (b.modulator)
            {
                WriteArcStopField(cfg, b.arcIndex, b.stopIndex, b.field,
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
            float value = 0.0f;
            ReadField(cfg, b.field, value);
            mSavedScalarValues.push_back(value);
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

        if (!mPreservedSegmentBindings.empty() || !mPreservedSegmentStopBindings.empty())
        {
            mSavedPreservedSegmentBoosts = cfg.neon.preservedSegmentBoosts;
            mPreservedSegmentBoostsCaptured = true;
        }
        else
        {
            mSavedPreservedSegmentBoosts.clear();
            mPreservedSegmentBoostsCaptured = false;
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
            WriteField(cfg, mScalarBindings[i].field, mSavedScalarValues[i]);
        }
        if (mSegmentBoostsCaptured)
        {
            cfg.neon.segmentBoosts = mSavedSegmentBoosts;
        }
        if (mPreservedSegmentBoostsCaptured)
        {
            cfg.neon.preservedSegmentBoosts = mSavedPreservedSegmentBoosts;
        }
        if (mArcsCaptured)
        {
            cfg.neon.arcs = mSavedArcs;
        }
    }

} // namespace EdgeLighting
