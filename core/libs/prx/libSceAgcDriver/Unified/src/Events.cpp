#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include <algorithm>

namespace AgcDriver::Unified::Events {

namespace {

thread_local std::uint64_t threadNotes = 0;

}

std::uint64_t ThreadNotes() {
    return threadNotes;
}

std::size_t StripedCount::Stripe() noexcept {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t stripe = next.fetch_add(1, std::memory_order_relaxed) % Stripes;
    return stripe;
}

std::uint64_t StripedCount::Sum() const noexcept {
    std::uint64_t total = 0;
    for (const auto& slot : slots) total += slot.value.load(std::memory_order_relaxed);
    return total;
}

ChangeLog::ChangeLog() : buckets(new Bucket[BucketCount]), blocks(new Bucket[BlockCount]), regions(new std::atomic<std::uint64_t>[RegionCount]) {
    for (std::size_t region = 0; region < RegionCount; ++region) regions[region].store(0, std::memory_order_relaxed);
}

void ChangeLog::noteBucket(Bucket& bucket, std::uint64_t epoch, std::uintptr_t owner) {
    bucket.version.fetch_add(1, std::memory_order_acq_rel);
    const auto previousOwner = bucket.owner.load(std::memory_order_relaxed);
    if (previousOwner != owner) {
        bucket.epochByOthers.store(bucket.epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        bucket.owner.store(owner, std::memory_order_relaxed);
    }
    bucket.epoch.store(epoch, std::memory_order_relaxed);
    bucket.version.fetch_add(1, std::memory_order_release);
}

bool ChangeLog::moved(const Seen& seen, std::uint64_t stamp, std::uintptr_t owner, bool& exempted) {
    if (seen.epoch <= stamp) return false;
    if (owner != 0 && seen.owner == owner && seen.epochByOthers <= stamp) {
        exempted = true;
        return false;
    }
    return true;
}

std::uint64_t ChangeLog::Note(std::uint64_t address, std::uint64_t bytes, std::uintptr_t owner) {
    if (bytes == 0) return Serial();
    const auto epoch = serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    notes.Add();
    ++threadNotes;
    const auto first = address / BucketBytes;
    const auto last = (address + bytes - 1) / BucketBytes;
    const auto end = first + std::min<std::uint64_t>(last - first + 1, BucketCount);
    for (auto index = first; index < end;) {
        if (index % BucketsPerBlock == 0 && end - index >= BucketsPerBlock) {
            const auto block = blockOf(index);
            std::lock_guard lock(blockStripes[block % WriterStripes]);
            noteBucket(blocks[block], epoch, owner);
            index += BucketsPerBlock;
            continue;
        }
        const auto bucket = bucketOf(index * BucketBytes);
        std::lock_guard lock(stripes[bucket % WriterStripes]);
        noteBucket(buckets[bucket], epoch, owner);
        ++index;
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

bool ChangeLog::ChangedSince(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp, std::uintptr_t owner, bool* exemptedOut) const {
    if (bytes == 0) return false;
    queries.Add();
    const auto firstRegion = address / RegionBytes;
    const auto lastRegion = (address + bytes - 1) / RegionBytes;
    bool regionMoved = lastRegion - firstRegion + 1 >= RegionCount;
    for (auto region = firstRegion; !regionMoved && region <= lastRegion; ++region) regionMoved = regions[regionOf(region * RegionBytes)].load(std::memory_order_acquire) > stamp;
    if (!regionMoved) {
        regionSkips.Add();
        return false;
    }
    const auto first = address / BucketBytes;
    const auto last = (address + bytes - 1) / BucketBytes;
    if (last - first + 1 >= BucketCount) {
        changed.Add();
        return true;
    }
    bool exempted = false;
    for (auto index = first; index <= last; ++index) {
        if (index == first || index % BucketsPerBlock == 0) {
            if (moved(read(blocks[blockOf(index)]), stamp, owner, exempted)) {
                changed.Add();
                return true;
            }
        }
        if (!moved(read(buckets[bucketOf(index * BucketBytes)]), stamp, owner, exempted)) continue;
        changed.Add();
        return true;
    }
    if (exempted) {
        ownerExempt.Add();
        if (exemptedOut != nullptr) *exemptedOut = true;
    }
    return false;
}

std::uintptr_t ChangeLog::ChangedOwner(std::uint64_t address, std::uint64_t bytes, std::uint64_t stamp) const {
    if (bytes == 0) return 0;
    const auto first = address / BucketBytes;
    const auto last = std::min<std::uint64_t>((address + bytes - 1) / BucketBytes, first + BucketCount - 1);
    for (auto index = first; index <= last; ++index) {
        const auto seen = read(buckets[bucketOf(index * BucketBytes)]);
        const auto block = read(blocks[blockOf(index)]);
        const auto& latest = block.epoch > seen.epoch ? block : seen;
        if (latest.epoch > stamp) return latest.owner;
    }
    return 0;
}

bool ChangeLog::RegionsChangedSince(std::span<const std::uint32_t> regionIndices, std::uint64_t stamp) const {
    return std::ranges::any_of(regionIndices, [&](std::uint32_t region) { return regions[region].load(std::memory_order_acquire) > stamp; });
}

bool ChangeLog::RegionChangedSince(std::uint32_t regionIndex, std::uint64_t stamp) const {
    return regions[regionIndex & (RegionCount - 1)].load(std::memory_order_acquire) > stamp;
}

void ChangeLog::AppendRegions(std::uint64_t address, std::uint64_t bytes, std::vector<std::uint32_t>& regionIndices) {
    if (bytes == 0) return;
    const auto firstRegion = address / RegionBytes;
    const auto lastRegion = (address + bytes - 1) / RegionBytes;
    const auto span = std::min<std::uint64_t>(lastRegion - firstRegion + 1, RegionCount);
    for (std::uint64_t step = 0; step < span; ++step) regionIndices.push_back(static_cast<std::uint32_t>(regionOf((firstRegion + step) * RegionBytes)));
}

ChangeLogCounts ChangeLog::Counts() const {
    return {notes.Sum(), queries.Sum(), regionSkips.Sum(), changed.Sum(), ownerExempt.Sum()};
}

namespace {

thread_local std::uintptr_t storeOwner = 0;

void noteStore(std::uint64_t address, std::uint64_t bytes) {
    if (storeOwner == 0u) Downgrades::WatchTouch("a guest store", address, bytes);
    Shared().Note(address, bytes, storeOwner);
}

}

StoreOwner::StoreOwner(std::uintptr_t owner) : previous(storeOwner) {
    storeOwner = owner;
}

StoreOwner::~StoreOwner() {
    storeOwner = previous;
}

ChangeLog& Shared() {
    static ChangeLog& log = []() -> ChangeLog& {
        auto& created = *new ChangeLog();
        GuestMemory::SetStoreHook(noteStore);
        return created;
    }();
    return log;
}

ChangeLog& Images() {
    static ChangeLog& log = *new ChangeLog();
    return log;
}

ChangeLog& GpuWrites() {
    static ChangeLog& log = *new ChangeLog();
    return log;
}

void NoteImage(std::uint64_t address, std::uint64_t bytes, std::uintptr_t owner) {
    if (owner == 0u) Downgrades::WatchTouch("an existing-cache image note", address, bytes);
    Shared().Note(address, bytes, owner);
    Images().Note(address, bytes, owner);
    if (owner != ImageSetOwner) GpuWrites().Note(address, bytes, owner);
}

void NoteRanges(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, std::uintptr_t owner) {
    for (const auto& [begin, end] : ranges) {
        if (end > begin) Shared().Note(begin, end - begin, owner);
    }
    Shadow::NoteGpuWrites(ranges);
}

}
