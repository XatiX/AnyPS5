#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PACKETTABLE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PACKETTABLE_HPP

#include "prx/libSceAgcDriver/Unified/include/StageCosts.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace AgcDriver::Unified {

struct PacketTableCounts {
    std::uint64_t offered = 0;
    std::uint64_t refusedOffers = 0;
    std::uint64_t found = 0;
    std::uint64_t refused = 0;
    std::uint64_t waited = 0;
    std::uint64_t missed = 0;
    std::uint64_t passed = 0;
    std::uint64_t released = 0;
    std::uint64_t stale = 0;
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
            if (entries.size() >= MaxPending) makeRoom();
            if (value == nullptr) ++counts.refusedOffers;
            entries.insert_or_assign(KeyOf(queueId, serial, packetOffset), Entry{queueId, {serial, packetOffset}, std::move(value)});
            auto& frontier = queues[queueId].offered;
            frontier = std::max(frontier, Position{serial, packetOffset});
            ++counts.offered;
        }
        offeredSignal.notify_all();
    }

    Found Find(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, std::chrono::milliseconds wait) {
        const auto key = KeyOf(queueId, serial, packetOffset);
        const Position position{serial, packetOffset};
        std::unique_lock lock(mutex);
        auto& queue = queues[queueId];
        queue.requested = std::max(queue.requested, position);
        auto found = entries.find(key);
        if (found == entries.end()) {
            if (queue.offered > position) {
                ++counts.passed;
                return {};
            }
            ++counts.waited;
            const auto waitStarted = std::chrono::steady_clock::now();
            offeredSignal.wait_for(lock, wait, [&] { return (found = entries.find(key)) != entries.end() || queues[queueId].offered > position; });
            StageCosts::Note(StageCosts::Stage::Order, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted).count()));
        }
        if (found == entries.end()) {
            ++counts.missed;
            return {};
        }
        if (found->second.value == nullptr) ++counts.refused;
        else ++counts.found;
        return {true, found->second.value};
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
    using Position = std::pair<std::uint64_t, std::size_t>;

    struct Entry {
        std::uint32_t queue = 0;
        Position position;
        std::shared_ptr<const TValue> value;
    };

    struct QueueMarks {
        Position offered{};
        Position requested{};
    };

    void makeRoom() {
        for (auto it = entries.begin(); it != entries.end();) {
            const auto marks = queues.find(it->second.queue);
            if (marks != queues.end() && it->second.position < marks->second.requested) {
                it = entries.erase(it);
                ++counts.stale;
            } else {
                ++it;
            }
        }
        if (entries.size() < MaxPending) return;
        counts.dropped += entries.size();
        entries.clear();
    }

    std::mutex mutex;
    std::condition_variable offeredSignal;
    std::unordered_map<std::uint64_t, Entry> entries;
    std::unordered_map<std::uint32_t, QueueMarks> queues;
    PacketTableCounts counts;
};

}

#endif
