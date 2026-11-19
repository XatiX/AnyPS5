#include "prx/libSceAgcDriver/Unified/include/DataRing.hpp"
#include <algorithm>

namespace AgcDriver::Unified {

RingSpace::RingSpace(std::uint64_t capacity, std::uint64_t alignment) : capacity(capacity), alignment(std::max<std::uint64_t>(alignment, 1u)) {}

std::uint64_t RingSpace::aligned(std::uint64_t offset) const {
    return (offset + alignment - 1u) / alignment * alignment;
}

void RingSpace::retire(std::uint64_t completedThrough) {
    while (!regions.empty() && regions.front().serial <= completedThrough) {
        regions.pop_front();
        counts.retired++;
    }
}

void RingSpace::place(std::uint64_t offset, std::uint64_t bytes, std::uint64_t serial) {
    if (!regions.empty() && regions.back().serial == serial && aligned(regions.back().end) == offset) regions.back().end = offset + bytes;
    else regions.push_back({offset, offset + bytes, serial});
    counts.allocations++;
    counts.bytes += bytes;
}

std::optional<std::uint64_t> RingSpace::Allocate(std::uint64_t bytes, std::uint64_t serial, std::uint64_t completedThrough) {
    retire(completedThrough);
    if (bytes == 0u || bytes > capacity) {
        counts.full++;
        return std::nullopt;
    }
    if (regions.empty()) {
        place(0, bytes, serial);
        return 0u;
    }
    const auto& front = regions.front();
    const auto& back = regions.back();
    const bool wrapped = back.begin < front.begin;
    const auto head = aligned(back.end);
    if (wrapped) {
        if (head + bytes <= front.begin) {
            place(head, bytes, serial);
            return head;
        }
        counts.full++;
        return std::nullopt;
    }
    if (head + bytes <= capacity) {
        place(head, bytes, serial);
        return head;
    }
    if (bytes <= front.begin) {
        counts.wraps++;
        place(0, bytes, serial);
        return 0u;
    }
    counts.full++;
    return std::nullopt;
}

std::uint64_t RingSpace::Used() const {
    std::uint64_t used = 0;
    for (const auto& region : regions) used += region.end - region.begin;
    return used;
}

RingCounts RingSpace::TakeCounts() {
    const auto taken = counts;
    counts = {};
    return taken;
}

}
