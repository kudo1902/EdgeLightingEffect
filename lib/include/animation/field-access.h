#ifndef _EDGE_LIGHTING_FIELD_ACCESS_H_
#define _EDGE_LIGHTING_FIELD_ACCESS_H_

#include "animation/field-bound-animation.h"
#include "core/config.h"
#include <cstddef>
#include <cstdint>

namespace EdgeLighting
{
    /// @file
    /// @brief Field-addressed read/write access to a @ref Config, keyed by the
    ///        same enums @ref FieldBoundAnimation binds modulators to.
    ///
    /// One switch per field family, shared by everything that needs to resolve
    /// a field enum to a config leaf: @ref FieldBoundAnimation's apply and
    /// baseline paths, and the C ABI's source-parameterised read family.
    ///
    /// @par Layout
    /// Grouped by TARGET - the thing being addressed - with each group's write
    /// and read declared as a pair, rather than split into a write family and a
    /// read family. The two halves of a pair resolve the same field enum over
    /// the same slot accessor, so they have to agree about what is in range; a
    /// reader that disagrees with its writer is a bug waiting for an animation
    /// to grow a vector. Keeping them adjacent is what makes a drift visible.
    /// The .cpp follows this order, and so does its file-local helper block.
    ///
    /// @par Bounds
    /// NOTHING here auto-grows. Every indexed entry must already exist - size
    /// the pools through @ref Config directly or the C API's count setters
    /// first. Contrast @c EnsureSegmentSlot in neon-animations.h, which is the
    /// growing one the segment animations use.
    ///
    /// @par Reporting a miss
    /// This is the ONLY thing the two halves of a pair do differently.
    /// A @c Write* logs and skips, which is what an animation bound to a slot
    /// released out from under it needs - it cannot fix the binding, and must
    /// not spam a path it cannot fix. A @c Read* returns @c false and leaves
    /// @p out untouched, without logging, which is what a caller that turns the
    /// miss into an error code needs; probing a container it has not sized yet
    /// is ordinary, not a defect.
    ///
    /// A @c Read* also returns @c false for a field enum value the library does
    /// not define, which is how an unknown value arriving over the C ABI is
    /// caught. The matching @c Write* is a no-op in that case.

    // --- Scalar Config leaves ----------------------------------------------

    /// @brief One scalar leaf of @ref Config, addressed by @ref AnimatableField.
    void WriteField(Config &cfg, AnimatableField field, float value);
    bool ReadField(const Config &cfg, AnimatableField field, float &out);

    // --- Segment boosts (transient pool, by index) -------------------------

    /// @brief A scalar inside @c neon.segmentBoosts[index].
    /// @note Under a travelling-segment animation this pool is longer in the
    ///       active config than in the base - the animation grows it - so size
    ///       any loop from the config being read, never from the authored one.
    void WriteSegmentField(Config &cfg, size_t index, SegmentField field, float value);
    bool ReadSegmentField(const Config &cfg, size_t index, SegmentField field, float &out);

    // --- Preserved segments (override-proof pool, by stable id) ------------

    /// @brief A scalar inside the preserved entry owning @p id.
    /// @note Addressed by id rather than index because the whole point of the
    ///       preserved pool is that an id outlives what an index does. An id
    ///       that is no longer live is a miss, handled as above.
    void WritePreservedSegmentField(Config &cfg, uint32_t id, SegmentField field, float value);
    bool ReadPreservedSegmentField(const Config &cfg, uint32_t id, SegmentField field, float &out);

    // --- Colour stops inside a segment boost -------------------------------

    /// @brief One channel of one colour stop inside @c neon.segmentBoosts[segIdx].
    void WriteSegmentStopField(Config &cfg, size_t segIdx, size_t stopIdx,
                               ColorStopField field, float value);
    bool ReadSegmentStopField(const Config &cfg, size_t segIdx, size_t stopIdx,
                              ColorStopField field, float &out);

    // --- Colour stops inside a preserved entry -----------------------------

    /// @brief One channel of one colour stop inside the preserved entry owning @p id.
    void WritePreservedSegmentStopField(Config &cfg, uint32_t id, size_t stopIdx,
                                        ColorStopField field, float value);
    bool ReadPreservedSegmentStopField(const Config &cfg, uint32_t id, size_t stopIdx,
                                       ColorStopField field, float &out);

    // --- Arcs (by index) ---------------------------------------------------

    /// @brief A scalar inside @c neon.arcs[index].
    void WriteArcField(Config &cfg, size_t index, ArcField field, float value);
    bool ReadArcField(const Config &cfg, size_t index, ArcField field, float &out);

    // --- Colour stops inside an arc ----------------------------------------

    /// @brief One channel of one colour stop inside @c neon.arcs[arcIdx].
    void WriteArcStopField(Config &cfg, size_t arcIdx, size_t stopIdx,
                           ColorStopField field, float value);
    bool ReadArcStopField(const Config &cfg, size_t arcIdx, size_t stopIdx,
                          ColorStopField field, float &out);

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FIELD_ACCESS_H_
