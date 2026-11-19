#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PACKETTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PACKETTABLE_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace AgcDriver::Unified {

struct PacketTableCounts {
    std::uint64_t offered = 0;
    std::uint64_t refusedOffers = 0;
    std::uint64_t found = 0;
    std::uint64_t refused = 0;
    std::uint64_t waited = 0;
    std::uint64_t missed = 0;
    std::uint64_t released = 0;
    std::uint64_t dropped = 0;
    std::uint64_t pending = 0;
};

template <typename TValue>
class PacketTable {
public:
    static constexpr std::size_t MaxPending = 1u << 14u;

    struct Found {
        bool offered = false;
        std::shared_ptr<const TValue> value;
    };

    static std::uint64_t KeyOf(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset) {
        return (static_cast<std::uint64_t>(queueId & 0xffu) << 56u) ^ (serial << 28u) ^ static_cast<std::uint64_t>(packetOffset);
    }

    void Offer(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, std::shared_ptr<const TValue> value) {
        {
            std::lock_guard lock(mutex);
            if (entries.size() >= MaxPending) {
                counts.dropped += entries.size();
                entries.clear();
            }
            if (value == nullptr) ++counts.refusedOffers;
            entries.insert_or_assign(KeyOf(queueId, serial, packetOffset), std::move(value));
            ++counts.offered;
        }
        offeredSignal.notify_all();
    }

    Found Find(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, std::chrono::milliseconds wait) {
        const auto key = KeyOf(queueId, serial, packetOffset);
        std::unique_lock lock(mutex);
        auto found = entries.find(key);
        if (found == entries.end()) {
            ++counts.waited;
            offeredSignal.wait_for(lock, wait, [&] { return (found = entries.find(key)) != entries.end(); });
        }
        if (found == entries.end()) {
            ++counts.missed;
            return {};
        }
        if (found->second == nullptr) ++counts.refused;
        else ++counts.found;
        return {true, found->second};
    }

    void Release(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset) {
        std::lock_guard lock(mutex);
        if (entries.erase(KeyOf(queueId, serial, packetOffset)) != 0u) ++counts.released;
    }

    PacketTableCounts TakeCounts() {
        std::lock_guard lock(mutex);
        auto result = counts;
        result.pending = entries.size();
        counts = {};
        return result;
    }

private:
    std::mutex mutex;
    std::condition_variable offeredSignal;
    std::unordered_map<std::uint64_t, std::shared_ptr<const TValue>> entries;
    PacketTableCounts counts;
};

}

#endif
