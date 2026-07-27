#ifndef _EDGE_LIGHTING_SEGMENT_UTILS_H_
#define _EDGE_LIGHTING_SEGMENT_UTILS_H_

#include "core/config.h"
#include <vector>

namespace EdgeLighting
{
    // Operations over the two segment pools. NeonConfig only holds the data
    // (the two vectors); the pool-management logic and the render-time merge
    // live here as free functions so the config struct stays a plain data holder.
    namespace SegmentUtils
    {
        /// Append a default entry to @c neon.preservedSegmentBoosts and hand back
        /// its id, or @c 0 if the preserved pool is already at
        /// @c NeonConfig::MAX_SEGMENT_BOOSTS_CAP. The id lets the caller mutate
        /// exactly that entry via @ref FindPreservedSegment regardless of what
        /// other owners do to either pool.
        ///
        /// The id is derived from the pool - one past the highest live id (ids
        /// 1-9 stay reserved for well-known / sentinel entries), so it is always
        /// unique among the entries that exist and needs no stored counter: it
        /// travels with the config for free and survives both a host rebuilding
        /// @c NeonConfig from a base and (de)serialization. Caveat: releasing the
        /// highest id frees that value for reuse, so do not keep using an id
        /// after releasing it.
        inline uint32_t AcquireSegment(NeonConfig &neon)
        {
            if (static_cast<int>(neon.preservedSegmentBoosts.size()) >= NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
            {
                return 0;
            }
            uint32_t id = 10;
            for (const PreservedSegment &p : neon.preservedSegmentBoosts)
            {
                if (p.id >= id)
                {
                    id = p.id + 1;
                }
            }
            PreservedSegment ps;
            ps.id = id;
            neon.preservedSegmentBoosts.push_back(ps);
            return ps.id;
        }

        /// Index into @c neon.preservedSegmentBoosts of the entry owning @p id,
        /// or @c -1 if none. @c id 0 never matches.
        inline int FindPreservedSegment(const NeonConfig &neon, uint32_t id)
        {
            if (id == 0)
            {
                return -1;
            }
            for (size_t i = 0; i < neon.preservedSegmentBoosts.size(); ++i)
            {
                if (neon.preservedSegmentBoosts[i].id == id)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        /// Drop the preserved entry owning @p id; others keep their ids. Returns
        /// true if an entry was removed.
        inline bool ReleaseSegment(NeonConfig &neon, uint32_t id)
        {
            int idx = FindPreservedSegment(neon, id);
            if (idx < 0)
            {
                return false;
            }
            neon.preservedSegmentBoosts.erase(neon.preservedSegmentBoosts.begin() + idx);
            return true;
        }

        /// Fill @p out with the segment list the neon shaders bake: the preserved
        /// pool's segments first (so id-addressed hotspots keep their shader
        /// slots even when the transient pool is full), then the transient
        /// @c segmentBoosts, truncated to @c NeonConfig::MAX_SEGMENT_BOOSTS_CAP.
        /// With no preserved entries this is exactly @c segmentBoosts, so the
        /// pre-preserved behaviour is unchanged.
        ///
        /// @p out is @c clear()ed and its capacity reused, so a renderer that
        /// passes the same scratch buffer every frame does no heap allocation
        /// after warmup.
        inline void FillEffectiveSegments(const NeonConfig &neon,
                                          std::vector<SegmentBoost> &out)
        {
            out.clear();
            for (const PreservedSegment &p : neon.preservedSegmentBoosts)
            {
                if (static_cast<int>(out.size()) >= NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
                {
                    return;
                }
                out.push_back(p.segment);
            }
            for (const SegmentBoost &s : neon.segmentBoosts)
            {
                if (static_cast<int>(out.size()) >= NeonConfig::MAX_SEGMENT_BOOSTS_CAP)
                {
                    return;
                }
                out.push_back(s);
            }
        }
    } // namespace SegmentUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SEGMENT_UTILS_H_
