#ifndef _EDGE_LIGHTING_CONFIG_SNAPSHOT_H_
#define _EDGE_LIGHTING_CONFIG_SNAPSHOT_H_

#include "core/config.h"
#include <atomic>
#include <cstdint>

namespace EdgeLighting
{

    /// @brief One frame's worth of everything the renderers need, handed from
    ///        the data side of @ref EdgeLightingEffect to its render side.
    ///
    /// The three scalars beside the config each answer a question the render
    /// side cannot answer for itself:
    ///
    /// - @c clockTime is what renderers receive as their @c time argument. It
    ///   is @c Clock's own value, so every clock control moves it: @c Pause
    ///   freezes it, @c Stop and @c Reset send it back to zero, @c SetTime puts
    ///   it anywhere. That is what the shaders' @c uTime expects.
    /// - @c rawAccumulatedTime is the running sum of the deltas handed to
    ///   @ref EdgeLightingEffect::Update, and **no clock control touches it** -
    ///   not pause, not stop, not reset, not a scrub. It only advances, and only
    ///   when the host advances the effect. That is what
    ///   @c GradientRingLUT::Tick needs: it is documented to take the RAW frame
    ///   delta, because a colour change must keep fading while the animation is
    ///   paused or being scrubbed.
    ///
    ///   Stored as an ABSOLUTE accumulator rather than a delta so that a render
    ///   side which misses snapshots still advances the fade by the right
    ///   amount - the difference against the last one consumed is correct
    ///   whether none or five publishes were dropped in between.
    /// - @c configGeneration moves ONLY when the composited config actually
    ///   changed, so it is how the render side knows whether to call
    ///   @c OnConfigChanged. The data side already does that deep compare in
    ///   @c refreshActiveConfig; repeating it per frame on the render side
    ///   would be pure waste, and comparing against the previous snapshot is
    ///   not possible anyway - the render side hands that slot back the moment
    ///   it takes a new one.
    typedef struct ConfigSnapshot
    {
        /// Active config: base plus every attached animation's overlay.
        Config config;
        /// Seconds, straight from @c Clock - pause, stop, reset and scrub all
        /// move it.
        float clockTime = 0.0f;
        /// Seconds, summed from the deltas handed to @c Update - no clock
        /// control moves it. Absolute, not a delta.
        float rawAccumulatedTime = 0.0f;
        /// Bumped only when @c config actually changed. 0 means "as constructed".
        uint64_t configGeneration = 0;
    } ConfigSnapshot;

    /// @brief Three-slot handoff for @ref ConfigSnapshot: one writer, one
    ///        reader, no locks and no blocking in either direction.
    ///
    /// @c Config owns vectors, so it is neither trivially copyable nor safe to
    /// publish through a seqlock. Three slots and a single atomic cell give the
    /// same effect: the writer owns one slot outright, the reader owns another
    /// outright, and the cell holds the third plus a flag saying whether what is
    /// in it has been seen.
    ///
    /// ## What each side may touch
    ///
    /// @c mWrite is private to the writing thread and @c mRead to the reading
    /// thread; @c mReady is the only word both touch. A slot is therefore never
    /// reachable from two threads at once, which is the whole safety argument -
    /// there is nothing to tear.
    ///
    /// ## What the two directions of skid mean
    ///
    /// - **Reader ahead of writer**: @ref AcquireLatest returns false and the
    ///   reader keeps the slot it holds. Rendering faster than the data side
    ///   updates is a supported, silent case - it just redraws.
    /// - **Writer ahead of reader**: intermediate snapshots are dropped. That is
    ///   correct rather than merely tolerable here, because @c OnConfigChanged
    ///   is idempotent with respect to the final state: a skipped intermediate
    ///   animated config is precisely a skipped frame.
    ///
    /// ## The allocation invariant
    ///
    /// A slot's @c Config keeps the vector capacity it was last written with, so
    /// @c BeginWrite().config @c = @c active is a copy-ASSIGN into warm buffers
    /// and allocates nothing once each slot has been written once. This is the
    /// same property @c EdgeLightingEffect::mScratchConfig exists for and that
    /// @c AnimationManager::mTickScratch was later built on, and it is why
    /// nothing here may ever clear, move from, or reconstruct a slot.
    class ConfigSnapshotBuffer
    {
    public:
        ConfigSnapshotBuffer() = default;

        /// @brief The slot the writer owns. Fill it, then call @ref Publish.
        /// @note Writer thread only. The reference is valid until @ref Publish.
        ConfigSnapshot &BeginWrite() { return mSlots[mWrite]; }

        /// @brief Hand the written slot to the reader and take one back.
        ///
        /// The exchange is @c acq_rel in both directions on purpose: the
        /// release half is what makes this slot's contents visible to a reader
        /// that later acquires it, and the acquire half is what guarantees the
        /// reader had finished with the slot handed BACK before the next
        /// @ref BeginWrite starts overwriting it.
        ///
        /// @note Writer thread only.
        void Publish()
        {
            const uint32_t prev = mReady.exchange(static_cast<uint32_t>(mWrite) | DIRTY_BIT,
                                                  std::memory_order_acq_rel);
            mWrite = static_cast<int>(prev & INDEX_MASK);
        }

        /// @brief Take the newest published snapshot, if there is one.
        ///
        /// @return true if a NEW snapshot was taken. False means nothing has
        ///         been published since the last call and @ref Current still
        ///         returns the slot already held - which is a valid frame, not
        ///         an error. Callers that must do something exactly once per
        ///         published frame (advancing a cross-fade, say) gate on this;
        ///         callers that just need something to draw ignore it.
        ///
        /// @note Reader thread only.
        bool AcquireLatest()
        {
            if ((mReady.load(std::memory_order_acquire) & DIRTY_BIT) == 0)
            {
                return false;
            }
            // Only the reader ever clears DIRTY and only the writer ever sets
            // it, so having observed it above, this exchange cannot come back
            // clean: there is no ABA to guard against here.
            const uint32_t prev = mReady.exchange(static_cast<uint32_t>(mRead),
                                                  std::memory_order_acq_rel);
            mRead = static_cast<int>(prev & INDEX_MASK);
            return true;
        }

        /// @brief The snapshot the reader currently holds.
        ///
        /// Valid from construction: all three slots are default-constructed, so
        /// a read before anything has ever been published yields the default
        /// config at generation 0 rather than nothing to draw.
        ///
        /// @note Reader thread only.
        const ConfigSnapshot &Current() const { return mSlots[mRead]; }

    private:
        static constexpr uint32_t INDEX_MASK = 0x3u;
        static constexpr uint32_t DIRTY_BIT = 0x4u;

        ConfigSnapshot mSlots[3];

        /// Slot index in the low bits, plus @ref DIRTY_BIT when the slot named
        /// holds something the reader has not taken yet. Seeded with slot 2 and
        /// clean, so the three indices start out distinct and unclaimed.
        std::atomic<uint32_t> mReady{2u};

        /// Writer thread only.
        int mWrite = 0;
        /// Reader thread only.
        int mRead = 1;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_SNAPSHOT_H_
