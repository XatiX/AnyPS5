#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_EVENTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_EVENTS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace AgcDriver::Unified::Events {

inline constexpr std::uint64_t BucketBytes = 0x10000;
inline constexpr std::size_t BucketCount = std::size_t{1} << 16u;
inline constexpr std::uint64_t RegionBytes = 0x400000;
inline constexpr std::size_t RegionCount = std::size_t{1} << 12u;
inline constexpr std::size_t WriterStripes = 256;

struct ChangeLogCounts {
    std::uint64_t notes = 0;
    std::uint64_t queries = 0;
    std::uint64_t regionSkips = 0;
    std::uint64_t changed = 0;
    std::uint64_t ownerExempt = 0;
};

class ChangeLog {
public:
    ChangeLog();

    [[nodiscard]] std::uint64_t Serial() const { return serial.load(std::memory_order_acquire); }
    std::uint64_t Note(std::uint64_t address, std::uint64_t bytes, std::uintptr_t owner = 0);
    [[nodiscard]] bool ChangedSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp, std::uintptr_t owner = 0) const;
    [[nodiscard]] ChangeLogCounts Counts() const;

private:
    struct Bucket {
        std::atomic<std::uint32_t> version{0};
        std::atomic<std::uint64_t> epoch{0};
        std::atomic<std::uintptr_t> owner{0};
        std::atomic<std::uint64_t> epochByOthers{0};
    };
    struct Seen {
        std::uint64_t epoch = 0;
        std::uintptr_t owner = 0;
        std::uint64_t epochByOthers = 0;
    };
    static std::size_t bucketOf(std::uint64_t address) { return static_cast<std::size_t>(address / BucketBytes) & (BucketCount - 1); }
    static std::size_t regionOf(std::uint64_t address) { return static_cast<std::size_t>(address / RegionBytes) & (RegionCount - 1); }
    Seen read(const Bucket& bucket) const;

    std::atomic<std::uint64_t> serial{0};
    std::unique_ptr<Bucket[]> buckets;
    std::unique_ptr<std::atomic<std::uint64_t>[]> regions;
    std::array<std::mutex, WriterStripes> stripes;
    mutable std::atomic<std::uint64_t> notes{0};
    mutable std::atomic<std::uint64_t> queries{0};
    mutable std::atomic<std::uint64_t> regionSkips{0};
    mutable std::atomic<std::uint64_t> changed{0};
    mutable std::atomic<std::uint64_t> ownerExempt{0};
};

ChangeLog& Shared();

}

#endif
