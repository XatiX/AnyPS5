#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_EVENTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_EVENTS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Events {

inline constexpr std::uint64_t BucketBytes = 0x10000;
inline constexpr std::size_t BucketCount = std::size_t{1} << 18u;
inline constexpr std::uint64_t BucketsPerBlock = 16;
inline constexpr std::size_t BlockCount = BucketCount / BucketsPerBlock;
inline constexpr std::uint64_t RegionBytes = 0x400000;
inline constexpr std::size_t RegionCount = std::size_t{1} << 12u;
inline constexpr std::size_t WriterStripes = 256;

class StripedCount {
public:
    static constexpr std::size_t Stripes = 16;

    void Add() noexcept { slots[Stripe()].value.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t Sum() const noexcept;
    static std::size_t Stripe() noexcept;

private:
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> value{0};
    };
    std::array<Slot, Stripes> slots;
};

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
    [[nodiscard]] bool ChangedSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp, std::uintptr_t owner = 0, bool* exempted = nullptr) const;
    [[nodiscard]] ChangeLogCounts Counts() const;
    [[nodiscard]] std::uintptr_t ChangedOwner(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp) const;
    [[nodiscard]] bool RegionsChangedSince(std::span<const std::uint32_t> regionIndices, std::uint64_t stamp) const;
    [[nodiscard]] bool RegionChangedSince(std::uint32_t regionIndex, std::uint64_t stamp) const;
    static void AppendRegions(std::uint64_t address, std::uint64_t bytes, std::vector<std::uint32_t>& regionIndices);

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
    static std::size_t blockOf(std::uint64_t bucket) { return static_cast<std::size_t>(bucket / BucketsPerBlock) & (BlockCount - 1); }
    static std::size_t regionOf(std::uint64_t address) { return static_cast<std::size_t>(address / RegionBytes) & (RegionCount - 1); }
    static void noteBucket(Bucket& bucket, std::uint64_t epoch, std::uintptr_t owner);
    static bool moved(const Seen& seen, std::uint64_t stamp, std::uintptr_t owner, bool& exempted);
    Seen read(const Bucket& bucket) const;

    std::atomic<std::uint64_t> serial{0};
    std::unique_ptr<Bucket[]> buckets;
    std::unique_ptr<Bucket[]> blocks;
    std::unique_ptr<std::atomic<std::uint64_t>[]> regions;
    std::array<std::mutex, WriterStripes> stripes;
    std::array<std::mutex, WriterStripes> blockStripes;
    mutable StripedCount notes;
    mutable StripedCount queries;
    mutable StripedCount regionSkips;
    mutable StripedCount changed;
    mutable StripedCount ownerExempt;
};

inline constexpr std::uintptr_t ImageSetOwner = 2;
inline constexpr std::uintptr_t TargetOwner = 4;

inline std::uintptr_t ResourceOwner(const void* resources) {
    return reinterpret_cast<std::uintptr_t>(resources) | 1u;
}

ChangeLog& Shared();
ChangeLog& Images();
ChangeLog& GpuWrites();
void NoteImage(std::uint64_t address, std::uint64_t bytes, std::uintptr_t owner = ImageSetOwner);
std::uint64_t ThreadNotes();

class StoreOwner {
public:
    explicit StoreOwner(std::uintptr_t owner);
    ~StoreOwner();
    StoreOwner(const StoreOwner&) = delete;
    StoreOwner& operator=(const StoreOwner&) = delete;

private:
    std::uintptr_t previous;
};
void NoteRanges(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uintptr_t owner = 0);

}

#endif
