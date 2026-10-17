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
        const std::vector<std::shared_ptr<BaseRenderer>> &renderers, uint64_t rendererGeneration,
        const ReadList &reads, const BufferRegistry &registry)
    {
        const bool check = changedSinceChecked(rendererGeneration, reads, registry);

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

    bool PrepareSchedule::changedSinceChecked(uint64_t rendererGeneration, const ReadList &reads,
                                              const BufferRegistry &registry)
    {
        const std::vector<BufferRead> &list = reads.GetReads();
        const bool context = !mChecked || mCheckedRendererGeneration != rendererGeneration ||
                             mCheckedRecordGeneration != registry.GetGeneration();
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
        mChecked = true;
        mCheckedRendererGeneration = rendererGeneration;
        mCheckedRecordGeneration = registry.GetGeneration();
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
            // None for a read the last check already saw. Of the rest, a key
            // nothing answers is logged once however many layers read it, and
            // a DEBUG read once per reader: that one is the READER's problem,
            // so another layer reading the same key first - the debug view,
            // which may - must not hide it.
            const bool seen = checked && std::find(checked->begin(), checked->end(), read) != checked->end();
            if (seen)
            {
                continue;
            }
            const auto earlier = list.begin() + static_cast<std::ptrdiff_t>(i);
            const bool keyRepeated = std::any_of(list.begin(), earlier, [&](const BufferRead &other)
                                                 { return std::strcmp(other.key, read.key) == 0; });
            const bool readerRepeated =
                std::any_of(list.begin(), earlier, [&](const BufferRead &other)
                            { return other.reader == read.reader && std::strcmp(other.key, read.key) == 0; });
            const unsigned int layer = read.reader ? static_cast<unsigned int>(read.reader->GetLayer()) : 0u;
            if (!read.complete)
            {
                if (keyRepeated)
                {
                    continue;
                }
                LOG_W("ReadList: layer 0x%x reads '%s...', longer than any buffer key can be (%d chars) - "
                      "nothing produces it.",
                      layer, read.key, BUFFER_KEY_CAPACITY - 1);
                continue;
            }
            const BufferRecord *record = registry.Find(read.key);
            if (!record)
            {
                if (keyRepeated)
                {
                    continue;
                }
                LOG_W("ReadList: layer 0x%x reads '%s', and no registered buffer has that key - it binds "
                      "nothing.",
                      layer, read.key);
                continue;
            }
            // The host's reads (EdgeLightingEffect::AddHostRead) have no
            // reader, and are held to the same rule: a host is never the
            // debug layer.
            if (!readerRepeated && record->visibility == BufferVisibility::DEBUG &&
                (!read.reader || read.reader->GetLayer() != RendererLayer::DEBUG))
            {
                if (read.reader)
                {
                    LOG_W("ReadList: layer 0x%x reads '%s', a DEBUG buffer - an intermediate its producer may "
                          "change with any optimisation. Read a PUBLIC product instead.",
                          layer, read.key);
                }
                else
                {
                    LOG_W("ReadList: the host reads '%s', a DEBUG buffer - an intermediate its producer may "
                          "change with any optimisation. Read a PUBLIC product instead.",
                          read.key);
                }
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
