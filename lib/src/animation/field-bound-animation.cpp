#include "animation/field-bound-animation.h"
#include "util/segment-utils.h"
#include "util/log-util.h"

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
            case AnimatableField::LENS_FLARE_PERIMETER_POSITION:
            {
                cfg.lensFlare.perimeterPosition = value;
                break;
            }
            case AnimatableField::LENS_FLARE_PERIMETER_OFFSET:
            {
                cfg.lensFlare.perimeterOffset = value;
                break;
            }
            case AnimatableField::LENS_FLARE_SIZE:
            {
                cfg.lensFlare.size = value;
                break;
            }
            case AnimatableField::LENS_FLARE_INTENSITY:
            {
                cfg.lensFlare.intensity = value;
                break;
            }
            case AnimatableField::LENS_FLARE_SPREAD:
            {
                cfg.lensFlare.spread = value;
                break;
            }
            case AnimatableField::LENS_FLARE_GHOST_SPACING:
            {
                cfg.lensFlare.ghostSpacing = value;
                break;
            }
            case AnimatableField::LENS_FLARE_GHOST_SIZE:
            {
                cfg.lensFlare.ghostSize = value;
                break;
            }
            case AnimatableField::LENS_FLARE_GHOST_OFFSET:
            {
                cfg.lensFlare.ghostOffset = value;
                break;
            }
            case AnimatableField::LENS_FLARE_GHOST_TINT:
            {
                cfg.lensFlare.ghostTint = value;
                break;
            }
            case AnimatableField::LENS_FLARE_RAY_DENSITY:
            {
                cfg.lensFlare.rayDensity = value;
                break;
            }
            case AnimatableField::LENS_FLARE_ROTATION_RATE:
            {
                cfg.lensFlare.rotationRate = value;
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
            case AnimatableField::LENS_FLARE_PERIMETER_POSITION:
            {
                return cfg.lensFlare.perimeterPosition;
            }
            case AnimatableField::LENS_FLARE_PERIMETER_OFFSET:
            {
                return cfg.lensFlare.perimeterOffset;
            }
            case AnimatableField::LENS_FLARE_SIZE:
            {
                return cfg.lensFlare.size;
            }
            case AnimatableField::LENS_FLARE_INTENSITY:
            {
                return cfg.lensFlare.intensity;
            }
            case AnimatableField::LENS_FLARE_SPREAD:
            {
                return cfg.lensFlare.spread;
            }
            case AnimatableField::LENS_FLARE_GHOST_SPACING:
            {
                return cfg.lensFlare.ghostSpacing;
            }
            case AnimatableField::LENS_FLARE_GHOST_SIZE:
            {
                return cfg.lensFlare.ghostSize;
            }
            case AnimatableField::LENS_FLARE_GHOST_OFFSET:
            {
                return cfg.lensFlare.ghostOffset;
            }
            case AnimatableField::LENS_FLARE_GHOST_TINT:
            {
                return cfg.lensFlare.ghostTint;
            }
            case AnimatableField::LENS_FLARE_RAY_DENSITY:
            {
                return cfg.lensFlare.rayDensity;
            }
            case AnimatableField::LENS_FLARE_ROTATION_RATE:
            {
                return cfg.lensFlare.rotationRate;
            }
            }
            return 0.0f;
        }

        // No auto-grow: the slot must already exist (size the pool via the C
        // API first). Returns nullptr when @p index is out of range; the write
        // helpers log and skip in that case.
        SegmentBoost *segmentSlot(Config &cfg, size_t index)
        {
            if (index >= cfg.neon.segmentBoosts.size())
            {
                return nullptr;
            }
            return &cfg.neon.segmentBoosts[index];
        }

        void writeSegmentScalar(SegmentBoost &s, SegmentField field, float value)
        {
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

        void writeSegment(Config &cfg, size_t index, SegmentField field, float value)
        {
            SegmentBoost *s = segmentSlot(cfg, index);
            if (!s)
            {
                LOG_E("writeSegment: index %zu out of range (size=%zu); skipping",
                      index, cfg.neon.segmentBoosts.size());
                return;
            }
            writeSegmentScalar(*s, field, value);
        }

        void writePreservedSegment(Config &cfg, uint32_t id, SegmentField field, float value)
        {
            // No auto-grow: the entry must have been acquired already. A binding
            // to an id that no longer exists (released elsewhere) is a no-op.
            int idx = SegmentUtils::FindPreservedSegment(cfg.neon, id);
            if (idx < 0)
            {
                return;
            }
            writeSegmentScalar(cfg.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment, field, value);
        }

        // No auto-grow: the stop must already exist within its segment/arc.
        // Returns nullptr when @p stopIdx is out of range; callers log and skip.
        ColorStop *colorStopSlot(std::vector<ColorStop> &stops, size_t stopIdx)
        {
            if (stopIdx >= stops.size())
            {
                return nullptr;
            }
            return &stops[stopIdx];
        }

        void writeColorStopScalar(ColorStop &c, ColorStopField field, float value)
        {
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

        void writeStop(Config &cfg, size_t segIdx, size_t stopIdx,
                       ColorStopField field, float value)
        {
            SegmentBoost *s = segmentSlot(cfg, segIdx);
            if (!s)
            {
                LOG_E("writeStop: segment index %zu out of range (size=%zu); skipping",
                      segIdx, cfg.neon.segmentBoosts.size());
                return;
            }
            ColorStop *c = colorStopSlot(s->colorStops, stopIdx);
            if (!c)
            {
                LOG_E("writeStop: stop index %zu out of range (size=%zu) for segment %zu; skipping",
                      stopIdx, s->colorStops.size(), segIdx);
                return;
            }
            writeColorStopScalar(*c, field, value);
        }

        void writePreservedStop(Config &cfg, uint32_t id, size_t stopIdx,
                                ColorStopField field, float value)
        {
            // Nothing is auto-grown here: the entry must have been acquired and
            // its stops sized already (via el_effect_set_preserved_segment_color_stop_count).
            // A binding to a released id, or to a stop past the current count, is
            // a skipped no-op - out-of-range just logs and moves on.
            int idx = SegmentUtils::FindPreservedSegment(cfg.neon, id);
            if (idx < 0)
            {
                return;
            }
            SegmentBoost &s = cfg.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
            ColorStop *c = colorStopSlot(s.colorStops, stopIdx);
            if (!c)
            {
                LOG_E("writePreservedStop: stopIdx %zu out of range (size=%zu) for preserved id %u; skipping",
                      stopIdx, s.colorStops.size(), id);
                return;
            }
            writeColorStopScalar(*c, field, value);
        }

        // No auto-grow, mirroring segmentSlot: nullptr when @p index is out of
        // range, and the write helpers log and skip.
        Arc *arcSlot(Config &cfg, size_t index)
        {
            if (index >= cfg.neon.arcs.size())
            {
                return nullptr;
            }
            return &cfg.neon.arcs[index];
        }

        void writeArc(Config &cfg, size_t index, ArcField field, float value)
        {
            Arc *ap = arcSlot(cfg, index);
            if (!ap)
            {
                LOG_E("writeArc: index %zu out of range (size=%zu); skipping",
                      index, cfg.neon.arcs.size());
                return;
            }
            Arc &a = *ap;
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

        void writeArcStop(Config &cfg, size_t arcIdx, size_t stopIdx,
                          ColorStopField field, float value)
        {
            Arc *a = arcSlot(cfg, arcIdx);
            if (!a)
            {
                LOG_E("writeArcStop: arc index %zu out of range (size=%zu); skipping",
                      arcIdx, cfg.neon.arcs.size());
                return;
            }
            ColorStop *c = colorStopSlot(a->colorStops, stopIdx);
            if (!c)
            {
                LOG_E("writeArcStop: stop index %zu out of range (size=%zu) for arc %zu; skipping",
                      stopIdx, a->colorStops.size(), arcIdx);
                return;
            }
            writeColorStopScalar(*c, field, value);
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
        for (const PreservedSegmentBinding &b : mPreservedSegmentBindings)
        {
            if (b.modulator)
            {
                writePreservedSegment(cfg, b.id, b.field, b.modulator->Evaluate(elapsed));
            }
        }
        for (const PreservedSegmentStopBinding &b : mPreservedSegmentStopBindings)
        {
            if (b.modulator)
            {
                writePreservedStop(cfg, b.id, b.stopIndex, b.field,
                                   b.modulator->Evaluate(elapsed));
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
            writeScalar(cfg, mScalarBindings[i].field, mSavedScalarValues[i]);
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
