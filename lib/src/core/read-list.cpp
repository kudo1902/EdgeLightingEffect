#include "core/read-list.h"
#include "renderer/base-renderer.h"
#include "util/log-util.h"
#include <algorithm>
#include <cstring>

namespace EdgeLighting
{
    bool BufferRead::operator==(const BufferRead &o) const
    {
        return reader == o.reader && phase == o.phase && complete == o.complete && std::strcmp(key, o.key) == 0;
    }

    void ReadList::Add(const BaseRenderer *reader, std::string_view key, ReadPhase phase)
    {
        if (key.empty())
        {
            return;
        }
        BufferRead read;
        read.reader = reader;
        read.phase = phase;
        const size_t length = std::min(key.size(), static_cast<size_t>(BUFFER_KEY_CAPACITY - 1));
        std::memcpy(read.key, key.data(), length);
        read.key[length] = '\0';
        read.complete = key.size() < static_cast<size_t>(BUFFER_KEY_CAPACITY);
        mReads.push_back(read);
    }

    bool ReadList::IsWanted(std::string_view key) const
    {
        for (const BufferRead &read : mReads)
        {
            if (read.complete && key == read.key)
            {
                return true;
            }
        }
        return false;
    }

    const std::vector<BaseRenderer *> &PrepareSchedule::Build(
        const std::vector<std::shared_ptr<BaseRenderer>> &renderers, const ReadList &reads,
        const BufferRegistry &registry)
    {
        const bool check = changedSinceChecked(renderers, reads, registry);

        mOrder.clear();
        for (const std::shared_ptr<BaseRenderer> &renderer : renderers)
        {
            mOrder.push_back(renderer.get());
        }
        const std::vector<BufferRead> &list = reads.GetReads();
        const bool anyPrepareRead = std::any_of(list.begin(), list.end(), [](const BufferRead &read)
                                                { return read.phase == ReadPhase::PREPARE && read.complete; });
        if (!anyPrepareRead)
        {
            return mOrder;
        }

        // The PREPARE reads as edges, producer -> reader. A read of a buffer
        // nothing in this effect produces, or of the reader's own, orders
        // nothing.
        const size_t n = renderers.size();
        mEdges.assign(n * n, 0);
        mInDegree.assign(n, 0);
        mPlaced.assign(n, 0);
        for (const BufferRead &read : list)
        {
            if (read.phase != ReadPhase::PREPARE || !read.complete)
            {
                continue;
            }
            const BufferRecord *record = registry.Find(read.key);
            const int producer = record ? indexOf(renderers, record->producer) : -1;
            const int reader = indexOf(renderers, read.reader);
            if (producer < 0 || reader < 0 || producer == reader)
            {
                continue;
            }
            uint8_t &edge = mEdges[static_cast<size_t>(producer) * n + static_cast<size_t>(reader)];
            if (edge == 0)
            {
                edge = 1;
                ++mInDegree[static_cast<size_t>(reader)];
            }
        }

        // Kahn's algorithm, taking the FIRST ready renderer in compositing
        // order at each step: the order changes only where an edge forces it.
        mOrder.clear();
        for (size_t step = 0; step < n; ++step)
        {
            size_t next = n;
            for (size_t i = 0; i < n; ++i)
            {
                if (mPlaced[i] == 0 && mInDegree[i] == 0)
                {
                    next = i;
                    break;
                }
            }
            if (next == n)
            {
                if (check)
                {
                    LOG_E("PrepareSchedule: the Prepare reads of %zu renderers form a cycle - every Prepare "
                          "runs in compositing order instead, so one of them reads a product before it is "
                          "made this frame.",
                          n - step);
                }
                mOrder.clear();
                for (const std::shared_ptr<BaseRenderer> &renderer : renderers)
                {
                    mOrder.push_back(renderer.get());
                }
                return mOrder;
            }
            mPlaced[next] = 1;
            mOrder.push_back(renderers[next].get());
            for (size_t j = 0; j < n; ++j)
            {
                if (mEdges[next * n + j] != 0)
                {
                    --mInDegree[j];
                }
            }
        }
        return mOrder;
    }

    bool PrepareSchedule::changedSinceChecked(const std::vector<std::shared_ptr<BaseRenderer>> &renderers,
                                              const ReadList &reads, const BufferRegistry &registry)
    {
        const std::vector<BufferRead> &list = reads.GetReads();
        bool context = mCheckedRecordCount != registry.GetRecordCount() || mCheckedRenderers.size() != renderers.size();
        for (size_t i = 0; !context && i < renderers.size(); ++i)
        {
            context = mCheckedRenderers[i] != renderers[i].get();
        }
        bool readsChanged = mCheckedReads.size() != list.size();
        for (size_t i = 0; !readsChanged && i < list.size(); ++i)
        {
            readsChanged = mCheckedReads[i] != list[i];
        }
        if (!context && !readsChanged)
        {
            return false;
        }
        // A new renderer or buffer can answer - or stop answering - any read,
        // so every read is checked again; otherwise only the reads that are
        // new since the last check, so a problem that stayed is not logged
        // again because another read changed.
        diagnose(reads, registry, context ? nullptr : &mCheckedReads);
        mCheckedRecordCount = registry.GetRecordCount();
        mCheckedRenderers.clear();
        for (const std::shared_ptr<BaseRenderer> &renderer : renderers)
        {
            mCheckedRenderers.push_back(renderer.get());
        }
        mCheckedReads = list;
        return true;
    }

    void PrepareSchedule::diagnose(const ReadList &reads, const BufferRegistry &registry,
                                   const std::vector<BufferRead> *checked) const
    {
        const std::vector<BufferRead> &list = reads.GetReads();
        for (size_t i = 0; i < list.size(); ++i)
        {
            const BufferRead &read = list[i];
            // One line per key, however many layers read it; none for a read
            // the last check already saw.
            const bool repeated = std::any_of(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(i),
                                              [&](const BufferRead &earlier)
                                              { return std::strcmp(earlier.key, read.key) == 0; });
            const bool seen = checked && std::find(checked->begin(), checked->end(), read) != checked->end();
            if (repeated || seen)
            {
                continue;
            }
            const unsigned int layer = read.reader ? static_cast<unsigned int>(read.reader->GetLayer()) : 0u;
            if (!read.complete)
            {
                LOG_W("ReadList: layer 0x%x reads '%s...', longer than any buffer key can be (%d chars) - "
                      "nothing produces it.",
                      layer, read.key, BUFFER_KEY_CAPACITY - 1);
                continue;
            }
            const BufferRecord *record = registry.Find(read.key);
            if (!record)
            {
                LOG_W("ReadList: layer 0x%x reads '%s', and no registered buffer has that key - it binds "
                      "nothing.",
                      layer, read.key);
                continue;
            }
            if (record->visibility == BufferVisibility::DEBUG && read.reader &&
                read.reader->GetLayer() != RendererLayer::DEBUG)
            {
                LOG_W("ReadList: layer 0x%x reads '%s', a DEBUG buffer - an intermediate its producer may "
                      "change with any optimisation. Read a PUBLIC product instead.",
                      layer, read.key);
            }
        }
    }

    int PrepareSchedule::indexOf(const std::vector<std::shared_ptr<BaseRenderer>> &renderers,
                                 const BaseRenderer *renderer)
    {
        for (size_t i = 0; i < renderers.size(); ++i)
        {
            if (renderers[i].get() == renderer)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

} // namespace EdgeLighting
