#ifndef _EDGE_LIGHTING_READ_LIST_H_
#define _EDGE_LIGHTING_READ_LIST_H_

#include "core/buffer-registry.h"
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace EdgeLighting
{
    class BaseRenderer;

    /// When a renderer binds a buffer it reads (docs/plans/resource-sharing-plan.md
    /// 3.4).
    typedef enum class ReadPhase
    {
        /// In its @c Prepare - its own offscreen passes read the buffer, so the
        /// producer's @c Prepare has to run first (@ref PrepareSchedule).
        PREPARE,
        /// In its @c Render, after every renderer's @c Prepare: any product is
        /// made by then, wherever the reader sits in the compositing order.
        RENDER
    } ReadPhase;

    /// One declared read: who binds which buffer, and when.
    typedef struct BufferRead
    {
        /// The buffer's key (@ref BufferRecord::key), cut to fit when a reader
        /// named a longer one - see @c complete.
        char key[BUFFER_KEY_CAPACITY] = {};
        /// Compared by identity; dereferenced only by the schedule's
        /// diagnostics, within the frame that declared it.
        const BaseRenderer *reader = nullptr;
        ReadPhase phase = ReadPhase::RENDER;
        /// False when the key the reader named did not fit in
        /// @ref BUFFER_KEY_CAPACITY: no buffer can have it, so nothing produces
        /// it, and @ref ReadList::IsWanted never matches it.
        bool complete = true;

        bool operator==(const BufferRead &o) const;
        bool operator!=(const BufferRead &o) const { return !(*this == o); }
    } BufferRead;

    /// Every buffer the renderers of one effect will bind this frame, by key -
    /// what each renderer declared in @c BaseRenderer::CollectReads, at the top
    /// of the frame, before any @c Prepare. Held by the effect's
    /// @ref ResourceContext and rebuilt every frame into a vector that keeps
    /// its capacity, so steady state allocates nothing.
    ///
    /// Read three ways: by a producer, which makes an on-demand product only
    /// while it is wanted (@ref IsWanted); by @ref PrepareSchedule, which runs
    /// a producer's @c Prepare before a @c Prepare that reads it; and by the
    /// same schedule's diagnostics, which log a read nothing produces.
    class ReadList
    {
    public:
        /// Forgets every read; the capacity stays. @c EdgeLightingEffect::Render,
        /// at the top of the frame.
        void Clear() { mReads.clear(); }

        /// Declares that @p reader binds the buffer under @p key in @p phase
        /// this frame. Called from @c BaseRenderer::CollectReads, with the
        /// renderer itself as @p reader. An empty key is ignored; one too long
        /// to be any buffer's is kept, cut and marked incomplete, so the
        /// diagnostics can name it.
        void Add(const BaseRenderer *reader, std::string_view key, ReadPhase phase);

        /// Whether any renderer reads the buffer under @p key this frame, in
        /// either phase. Valid from the top of the frame on - every
        /// @c CollectReads runs before any @c Prepare.
        bool IsWanted(std::string_view key) const;

        /// This frame's reads, in the order the renderers declared them.
        const std::vector<BufferRead> &GetReads() const { return mReads; }

    private:
        std::vector<BufferRead> mReads;
    };

    /// The order an effect calls @c Prepare in, each frame: the compositing
    /// order, except that a renderer whose @c Prepare reads another renderer's
    /// buffer (@ref ReadPhase::PREPARE) runs after that renderer - a stable
    /// topological sort, so renderers no read relates keep their order. A
    /// cycle is refused: logged, and drawn in compositing order. With no
    /// @c PREPARE read - every frame today - it is the compositing order, at
    /// the cost of one pass over the reads.
    ///
    /// Also the reads' diagnostics, logged when a read first appears - or,
    /// for every read, when the renderers or the registered buffers change,
    /// since either can answer a read or stop answering it - so a problem is
    /// logged once, not every frame, not again for every frame of an
    /// animation that leaves the reads alone, and not again because another
    /// read changed:
    ///   - a read of a key no registered buffer has, once per key;
    ///   - a read of a @c DEBUG buffer by any layer but the debug layer: an
    ///     intermediate whose contents its producer may change with any
    ///     optimisation (@ref BufferVisibility) - once per reader, whoever
    ///     else reads the key;
    ///   - a cycle.
    ///
    /// Owned by the effect: the order is of the effect's renderers. Steady
    /// state allocates nothing - every vector keeps its capacity.
    class PrepareSchedule
    {
    public:
        /// This frame's order. @p renderers is the effect's list, in
        /// compositing order, and @p rendererGeneration moves whenever a
        /// renderer joins or leaves it; @p reads this frame's; @p registry who
        /// produces each key. The result is valid until the next call.
        const std::vector<BaseRenderer *> &Build(const std::vector<std::shared_ptr<BaseRenderer>> &renderers,
                                                 uint64_t rendererGeneration, const ReadList &reads,
                                                 const BufferRegistry &registry);

    private:
        /// Whether the reads, the renderers or the registry differ from what
        /// the diagnostics last ran on; if so, runs them (@ref diagnose) and
        /// records what they ran on.
        bool changedSinceChecked(uint64_t rendererGeneration, const ReadList &reads, const BufferRegistry &registry);
        /// Logs the reads no registered buffer answers, and the DEBUG buffers
        /// a layer other than the debug layer reads - skipping any read in
        /// @p checked, the list the last check saw (nullptr: check them all).
        void diagnose(const ReadList &reads, const BufferRegistry &registry,
                      const std::vector<BufferRead> *checked) const;
        /// The index of @p renderer in @p renderers, or -1.
        static int indexOf(const std::vector<std::shared_ptr<BaseRenderer>> &renderers,
                           const BaseRenderer *renderer);

    private:
        std::vector<BaseRenderer *> mOrder;
        /// The PREPARE reads as edges, producer -> reader, n x n.
        std::vector<uint8_t> mEdges;
        std::vector<int> mInDegree;
        std::vector<uint8_t> mPlaced;
        /// What the diagnostics last ran on. Generations, not the renderers'
        /// addresses or the record count: a renderer destroyed and another
        /// allocated at its address, or a record removed and another
        /// registered, would leave those as they were.
        std::vector<BufferRead> mCheckedReads;
        uint64_t mCheckedRendererGeneration = 0;
        uint64_t mCheckedRecordGeneration = 0;
        bool mChecked = false;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_READ_LIST_H_
