#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DATARING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DATARING_HPP

#include <cstdint>
#include <deque>
#include <optional>

namespace AgcDriver::Unified {

struct RingCounts {
    std::uint64_t allocations = 0;
    std::uint64_t bytes = 0;
    std::uint64_t full = 0;
    std::uint64_t wraps = 0;
    std::uint64_t retired = 0;
};

class RingSpace {
public:
    RingSpace(std::uint64_t capacity, std::uint64_t alignment);

    std::optional<std::uint64_t> Allocate(std::uint64_t bytes, std::uint64_t serial, std::uint64_t completedThrough);
    [[nodiscard]] std::uint64_t Capacity() const { return capacity; }
    [[nodiscard]] std::uint64_t Used() const;
    RingCounts TakeCounts();

private:
    struct Region {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t serial = 0;
    };

    void retire(std::uint64_t completedThrough);
    std::uint64_t aligned(std::uint64_t offset) const;
    void place(std::uint64_t offset, std::uint64_t bytes, std::uint64_t serial);

    std::uint64_t capacity = 0;
    std::uint64_t alignment = 1;
    std::deque<Region> regions;
    RingCounts counts;
};

}

#endif
