#include "animation/field-access.h"
#include "util/segment-utils.h"
#include "util/log-util.h"

#include <vector>

namespace EdgeLighting
{
    // Each container gets ONE group below: its slot accessor in both
    // constnesses, then the write and the read of a scalar inside it. Grouped
    // by container rather than by direction so a pair sits together - the Read
    // is the exact mirror of the Write beside it, over the same slot accessor,
    // and the two cannot drift apart about what is in range without it being
    // visible in the same screenful.
    //
    // The pair differs only in how a miss is reported: writes log and skip (an
    // animation whose slot was released under it must not spam an error path it
    // cannot fix), reads return false silently (a caller probing a container it
    // has not sized yet is ordinary, not a defect).
    //
    // Nothing here auto-grows. Every Find* returns nullptr when the index is
    // past the end rather than creating what is missing - contrast
    // @c EnsureSegmentSlot in neon-animations.h, which is the growing one the
    // segment animations use.
    namespace
    {
        // --- Segment boosts ------------------------------------------------
        SegmentBoost *FindSegmentSlot(Config &cfg, size_t index)
        {
            if (index >= cfg.neon.segmentBoosts.size())
            {
                return nullptr;
            }
            return &cfg.neon.segmentBoosts[index];
        }

        const SegmentBoost *FindSegmentSlot(const Config &cfg, size_t index)
        {
            if (index >= cfg.neon.segmentBoosts.size())
            {
                return nullptr;
            }
            return &cfg.neon.segmentBoosts[index];
        }

        void WriteSegmentScalar(SegmentBoost &s, SegmentField field, float value)
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

        bool ReadSegmentScalar(const SegmentBoost &s, SegmentField field, float &out)
        {
            switch (field)
            {
            case SegmentField::POSITION:
            {
                out = s.position;
                return true;
            }
            case SegmentField::LENGTH:
            {
                out = s.length;
                return true;
            }
            case SegmentField::BOOST:
            {
                out = s.boost;
                return true;
            }
            }
            return false;
        }

        // --- Colour stops --------------------------------------------------
        // Shared: these sit inside both segment boosts and arcs, so the slot is
        // addressed by the owner's stop vector rather than by the Config.
        ColorStop *FindColorStopSlot(std::vector<ColorStop> &stops, size_t stopIdx)
        {
            if (stopIdx >= stops.size())
            {
                return nullptr;
            }
            return &stops[stopIdx];
        }

        const ColorStop *FindColorStopSlot(const std::vector<ColorStop> &stops, size_t stopIdx)
        {
            if (stopIdx >= stops.size())
            {
                return nullptr;
            }
            return &stops[stopIdx];
        }

        void WriteColorStopScalar(ColorStop &c, ColorStopField field, float value)
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

        bool ReadColorStopScalar(const ColorStop &c, ColorStopField field, float &out)
        {
            switch (field)
            {
            case ColorStopField::POSITION:
            {
                out = c.position;
                return true;
            }
            case ColorStopField::R:
            {
                out = c.color.r;
                return true;
            }
            case ColorStopField::G:
            {
                out = c.color.g;
                return true;
            }
            case ColorStopField::B:
            {
                out = c.color.b;
                return true;
            }
            case ColorStopField::A:
            {
                out = c.color.a;
                return true;
            }
            }
            return false;
        }

        // --- Arcs ----------------------------------------------------------
        Arc *FindArcSlot(Config &cfg, size_t index)
        {
            if (index >= cfg.neon.arcs.size())
            {
                return nullptr;
            }
            return &cfg.neon.arcs[index];
        }

        const Arc *FindArcSlot(const Config &cfg, size_t index)
        {
            if (index >= cfg.neon.arcs.size())
            {
                return nullptr;
            }
            return &cfg.neon.arcs[index];
        }

        void WriteArcScalar(Arc &a, ArcField field, float value)
        {
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

        bool ReadArcScalar(const Arc &a, ArcField field, float &out)
        {
            switch (field)
            {
            case ArcField::START:
            {
                out = a.start;
                return true;
            }
            case ArcField::LENGTH:
            {
                out = a.length;
                return true;
            }
            case ArcField::INTENSITY:
            {
                out = a.intensity;
                return true;
            }
            }
            return false;
        }
    } // namespace

    // --- Scalar Config leaves --------------------------------------------------

    void WriteField(Config &cfg, AnimatableField field, float value)
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

    bool ReadField(const Config &cfg, AnimatableField field, float &out)
    {
        switch (field)
        {
        case AnimatableField::NEON_INTENSITY:
        {
            out = cfg.neon.intensity;
            return true;
        }
        case AnimatableField::NEON_LINE_WIDTH:
        {
            out = cfg.neon.lineWidth;
            return true;
        }
        case AnimatableField::NEON_GLOW_RADIUS:
        {
            out = cfg.neon.glowRadius;
            return true;
        }
        case AnimatableField::NEON_BLOOM_STRENGTH:
        {
            out = cfg.neon.bloomStrength;
            return true;
        }
        case AnimatableField::NEON_FILAMENT_FALLOFF:
        {
            out = cfg.neon.filamentFalloff;
            return true;
        }
        case AnimatableField::NEON_GLOW_SIDE_SOFTNESS:
        {
            out = cfg.neon.glowSideSoftness;
            return true;
        }
        case AnimatableField::NEON_HUE_ROTATION_RATE:
        {
            out = cfg.neon.hueRotationRate;
            return true;
        }
        case AnimatableField::LENS_FLARE_PERIMETER_POSITION:
        {
            out = cfg.lensFlare.perimeterPosition;
            return true;
        }
        case AnimatableField::LENS_FLARE_PERIMETER_OFFSET:
        {
            out = cfg.lensFlare.perimeterOffset;
            return true;
        }
        case AnimatableField::LENS_FLARE_SIZE:
        {
            out = cfg.lensFlare.size;
            return true;
        }
        case AnimatableField::LENS_FLARE_INTENSITY:
        {
            out = cfg.lensFlare.intensity;
            return true;
        }
        case AnimatableField::LENS_FLARE_SPREAD:
        {
            out = cfg.lensFlare.spread;
            return true;
        }
        case AnimatableField::LENS_FLARE_GHOST_SPACING:
        {
            out = cfg.lensFlare.ghostSpacing;
            return true;
        }
        case AnimatableField::LENS_FLARE_GHOST_SIZE:
        {
            out = cfg.lensFlare.ghostSize;
            return true;
        }
        case AnimatableField::LENS_FLARE_GHOST_OFFSET:
        {
            out = cfg.lensFlare.ghostOffset;
            return true;
        }
        case AnimatableField::LENS_FLARE_GHOST_TINT:
        {
            out = cfg.lensFlare.ghostTint;
            return true;
        }
        case AnimatableField::LENS_FLARE_RAY_DENSITY:
        {
            out = cfg.lensFlare.rayDensity;
            return true;
        }
        case AnimatableField::LENS_FLARE_ROTATION_RATE:
        {
            out = cfg.lensFlare.rotationRate;
            return true;
        }
        }
        return false;
    }

    // --- Segment boosts (transient pool, by index) -----------------------------

    void WriteSegmentField(Config &cfg, size_t index, SegmentField field, float value)
    {
        SegmentBoost *s = FindSegmentSlot(cfg, index);
        if (!s)
        {
            LOG_E("WriteSegmentField: index %zu out of range (size=%zu); skipping",
                  index, cfg.neon.segmentBoosts.size());
            return;
        }
        WriteSegmentScalar(*s, field, value);
    }

    bool ReadSegmentField(const Config &cfg, size_t index, SegmentField field, float &out)
    {
        const SegmentBoost *s = FindSegmentSlot(cfg, index);
        if (!s)
        {
            return false;
        }
        return ReadSegmentScalar(*s, field, out);
    }

    // --- Preserved segments (by stable id) -------------------------------------

    void WritePreservedSegmentField(Config &cfg, uint32_t id, SegmentField field, float value)
    {
        // No auto-grow: the entry must have been acquired already. A binding
        // to an id that no longer exists (released elsewhere) is a no-op.
        int idx = SegmentUtils::FindPreservedSegment(cfg.neon, id);
        if (idx < 0)
        {
            return;
        }
        WriteSegmentScalar(cfg.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment, field, value);
    }

    bool ReadPreservedSegmentField(const Config &cfg, uint32_t id, SegmentField field, float &out)
    {
        int idx = SegmentUtils::FindPreservedSegment(cfg.neon, id);
        if (idx < 0)
        {
            return false;
        }
        return ReadSegmentScalar(cfg.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment,
                                 field, out);
    }

    // --- Colour stops inside a segment boost -----------------------------------

    void WriteSegmentStopField(Config &cfg, size_t segIdx, size_t stopIdx,
                               ColorStopField field, float value)
    {
        SegmentBoost *s = FindSegmentSlot(cfg, segIdx);
        if (!s)
        {
            LOG_E("WriteSegmentStopField: segment index %zu out of range (size=%zu); skipping",
                  segIdx, cfg.neon.segmentBoosts.size());
            return;
        }
        ColorStop *c = FindColorStopSlot(s->colorStops, stopIdx);
        if (!c)
        {
            LOG_E("WriteSegmentStopField: stop index %zu out of range (size=%zu) for segment %zu; skipping",
                  stopIdx, s->colorStops.size(), segIdx);
            return;
        }
        WriteColorStopScalar(*c, field, value);
    }

    bool ReadSegmentStopField(const Config &cfg, size_t segIdx, size_t stopIdx,
                              ColorStopField field, float &out)
    {
        const SegmentBoost *s = FindSegmentSlot(cfg, segIdx);
        if (!s)
        {
            return false;
        }
        const ColorStop *c = FindColorStopSlot(s->colorStops, stopIdx);
        if (!c)
        {
            return false;
        }
        return ReadColorStopScalar(*c, field, out);
    }

    // --- Colour stops inside a preserved entry ---------------------------------

    void WritePreservedSegmentStopField(Config &cfg, uint32_t id, size_t stopIdx,
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
        ColorStop *c = FindColorStopSlot(s.colorStops, stopIdx);
        if (!c)
        {
            LOG_E("WritePreservedSegmentStopField: stopIdx %zu out of range (size=%zu) for preserved id %u; skipping",
                  stopIdx, s.colorStops.size(), id);
            return;
        }
        WriteColorStopScalar(*c, field, value);
    }

    bool ReadPreservedSegmentStopField(const Config &cfg, uint32_t id, size_t stopIdx,
                                       ColorStopField field, float &out)
    {
        int idx = SegmentUtils::FindPreservedSegment(cfg.neon, id);
        if (idx < 0)
        {
            return false;
        }
        const SegmentBoost &s = cfg.neon.preservedSegmentBoosts[static_cast<size_t>(idx)].segment;
        const ColorStop *c = FindColorStopSlot(s.colorStops, stopIdx);
        if (!c)
        {
            return false;
        }
        return ReadColorStopScalar(*c, field, out);
    }

    // --- Arcs (by index) -------------------------------------------------------

    void WriteArcField(Config &cfg, size_t index, ArcField field, float value)
    {
        Arc *a = FindArcSlot(cfg, index);
        if (!a)
        {
            LOG_E("WriteArcField: index %zu out of range (size=%zu); skipping",
                  index, cfg.neon.arcs.size());
            return;
        }
        WriteArcScalar(*a, field, value);
    }

    bool ReadArcField(const Config &cfg, size_t index, ArcField field, float &out)
    {
        const Arc *a = FindArcSlot(cfg, index);
        if (!a)
        {
            return false;
        }
        return ReadArcScalar(*a, field, out);
    }

    // --- Colour stops inside an arc --------------------------------------------

    void WriteArcStopField(Config &cfg, size_t arcIdx, size_t stopIdx,
                           ColorStopField field, float value)
    {
        Arc *a = FindArcSlot(cfg, arcIdx);
        if (!a)
        {
            LOG_E("WriteArcStopField: arc index %zu out of range (size=%zu); skipping",
                  arcIdx, cfg.neon.arcs.size());
            return;
        }
        ColorStop *c = FindColorStopSlot(a->colorStops, stopIdx);
        if (!c)
        {
            LOG_E("WriteArcStopField: stop index %zu out of range (size=%zu) for arc %zu; skipping",
                  stopIdx, a->colorStops.size(), arcIdx);
            return;
        }
        WriteColorStopScalar(*c, field, value);
    }

    bool ReadArcStopField(const Config &cfg, size_t arcIdx, size_t stopIdx,
                          ColorStopField field, float &out)
    {
        const Arc *a = FindArcSlot(cfg, arcIdx);
        if (!a)
        {
            return false;
        }
        const ColorStop *c = FindColorStopSlot(a->colorStops, stopIdx);
        if (!c)
        {
            return false;
        }
        return ReadColorStopScalar(*c, field, out);
    }

} // namespace EdgeLighting
