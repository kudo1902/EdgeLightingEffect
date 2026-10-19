#include "core/shared-buffer-registry.h"
#include "core/shared-buffer.h"
#include "util/log-util.h"

namespace EdgeLighting
{

    SharedBufferRegistry::SharedBufferRegistry() = default;
    SharedBufferRegistry::~SharedBufferRegistry() = default;

    bool SharedBufferRegistry::Register(SharedBufferId id, const std::shared_ptr<SharedBuffer> &buffer)
    {
        if (!isValidId(id) || !buffer)
        {
            return false;
        }
        std::shared_ptr<SharedBuffer> &slot = mBuffers[static_cast<size_t>(id)];
        if (slot && slot != buffer)
        {
            LOG_E("SharedBufferRegistry: buffer %u already has a producer - a second one is not shared.",
                  static_cast<unsigned>(id));
            return false;
        }
        slot = buffer;
        return true;
    }

    void SharedBufferRegistry::Unregister(SharedBufferId id, const SharedBuffer *buffer)
    {
        if (!isValidId(id))
        {
            return;
        }
        std::shared_ptr<SharedBuffer> &slot = mBuffers[static_cast<size_t>(id)];
        if (slot.get() == buffer)
        {
            slot.reset();
        }
    }

    std::shared_ptr<const SharedBuffer> SharedBufferRegistry::Find(SharedBufferId id) const
    {
        return isValidId(id) ? mBuffers[static_cast<size_t>(id)] : nullptr;
    }

    bool SharedBufferRegistry::isValidId(SharedBufferId id)
    {
        if (static_cast<uint32_t>(id) < static_cast<uint32_t>(SharedBufferId::COUNT))
        {
            return true;
        }
        LOG_E("SharedBufferRegistry: %u names no buffer.", static_cast<unsigned>(id));
        return false;
    }

} // namespace EdgeLighting
