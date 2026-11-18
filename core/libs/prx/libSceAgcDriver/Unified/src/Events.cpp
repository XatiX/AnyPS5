#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>

namespace AgcDriver::Unified::Events {

ChangeLog::ChangeLog() : buckets(new Bucket[BucketCount]), regions(new std::atomic<std::uint64_t>[RegionCount]) {
    for (std::size_t region = 0; region < RegionCount; ++region) regions[region].store(0, std::memory_order_relaxed);
}

std::uint64_t ChangeLog::Note(std::uint64_t address, std::uint64_t bytes, std::uintptr_t owner) {
    if (bytes == 0) return Serial();
    const auto epoch = serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    notes.fetch_add(1, std::memory_order_relaxed);
    const auto first = address / BucketBytes;
    const auto last = (address + bytes - 1) / BucketBytes;
    const auto span = std::min<std::uint64_t>(last - first + 1, BucketCount);
    for (std::uint64_t step = 0; step < span; ++step) {
        const auto index = bucketOf((first + step) * BucketBytes);
        auto& bucket = buckets[index];
        std::lock_guard lock(stripes[index % WriterStripes]);
        bucket.version.fetch_add(1, std::memory_order_acq_rel);
        const auto previousOwner = bucket.owner.load(std::memory_order_relaxed);
        if (previousOwner != owner) {
            bucket.epochByOthers.store(bucket.epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
            bucket.owner.store(owner, std::memory_order_relaxed);
        }
        bucket.epoch.store(epoch, std::memory_order_relaxed);
        bucket.version.fetch_add(1, std::memory_order_release);
    }
    const auto firstRegion = address / RegionBytes;
    const auto lastRegion = (address + bytes - 1) / RegionBytes;
    const auto regionSpan = std::min<std::uint64_t>(lastRegion - firstRegion + 1, RegionCount);
    for (std::uint64_t step = 0; step < regionSpan; ++step) {
        auto& region = regions[regionOf((firstRegion + step) * RegionBytes)];
        auto seen = region.load(std::memory_order_relaxed);
        while (seen < epoch && !region.compare_exchange_weak(seen, epoch, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
    return epoch;
}

ChangeLog::Seen ChangeLog::read(const Bucket& bucket) const {
    for (;;) {
        const auto before = bucket.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        Seen seen{bucket.epoch.load(std::memory_order_relaxed), bucket.owner.load(std::memory_order_relaxed), bucket.epochByOthers.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (bucket.version.load(std::memory_order_relaxed) == before) return seen;
    }
}

bool ChangeLog::ChangedSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp, std::uintptr_t owner) const {
    if (bytes == 0) return false;
    queries.fetch_add(1, std::memory_order_relaxed);
    const auto firstRegion = address / RegionBytes;
    const auto lastRegion = (address + bytes - 1) / RegionBytes;
    bool regionMoved = lastRegion - firstRegion + 1 >= RegionCount;
    for (auto region = firstRegion; !regionMoved && region <= lastRegion; ++region) regionMoved = regions[regionOf(region * RegionBytes)].load(std::memory_order_acquire) > stamp;
    if (!regionMoved) {
        regionSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto first = address / BucketBytes;
    const auto last = (address + bytes - 1) / BucketBytes;
    if (last - first + 1 >= BucketCount) {
        changed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool exempted = false;
    for (auto index = first; index <= last; ++index) {
        const auto seen = read(buckets[bucketOf(index * BucketBytes)]);
        if (seen.epoch <= stamp) continue;
        if (owner != 0 && seen.owner == owner && seen.epochByOthers <= stamp) {
            exempted = true;
            continue;
        }
        changed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (exempted) ownerExempt.fetch_add(1, std::memory_order_relaxed);
    return false;
}

ChangeLogCounts ChangeLog::Counts() const {
    return {notes.load(std::memory_order_relaxed), queries.load(std::memory_order_relaxed), regionSkips.load(std::memory_order_relaxed), changed.load(std::memory_order_relaxed), ownerExempt.load(std::memory_order_relaxed)};
}

namespace {

void noteStore(std::uint64_t address, std::uint64_t bytes) {
    Shared().Note(address, bytes);
}

}

ChangeLog& Shared() {
    static ChangeLog& log = []() -> ChangeLog& {
        auto& created = *new ChangeLog();
        GuestMemory::SetStoreHook(noteStore);
        return created;
    }();
    return log;
}

void NoteRanges(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uintptr_t owner) {
    for (const auto& [begin, end] : ranges) {
        if (end > begin) Shared().Note(begin, end - begin, owner);
    }
}

}
