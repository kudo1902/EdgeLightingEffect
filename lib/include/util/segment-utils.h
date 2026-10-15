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
        /// @p out is overwritten IN PLACE: each segment is copy-assigned over
        /// the element already at its index, which reuses that element's
        /// @c colorStops storage, new elements are appended only past the old
        /// size, and the tail is erased. So a renderer that passes the same
        /// buffer every frame allocates nothing while the segment count holds
        /// and no segment gains stops - every frame of a segment animation.
        /// A @c clear() and @c push_back would copy every segment's stops into
        /// fresh storage instead (I75).
        inline void FillEffectiveSegments(const NeonConfig &neon,
                                          std::vector<SegmentBoost> &out)
        {
            const size_t cap = static_cast<size_t>(NeonConfig::MAX_SEGMENT_BOOSTS_CAP);
            size_t n = 0;
            auto put = [&out, &n](const SegmentBoost &s) {
                if (n < out.size())
                {
                    out[n] = s;
                }
                else
                {
                    out.push_back(s);
                }
                ++n;
            };
            for (const PreservedSegment &p : neon.preservedSegmentBoosts)
            {
                if (n >= cap)
                {
                    break;
                }
                put(p.segment);
            }
            for (const SegmentBoost &s : neon.segmentBoosts)
            {
                if (n >= cap)
                {
                    break;
                }
                put(s);
            }
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(n), out.end());
        }
    } // namespace SegmentUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SEGMENT_UTILS_H_
