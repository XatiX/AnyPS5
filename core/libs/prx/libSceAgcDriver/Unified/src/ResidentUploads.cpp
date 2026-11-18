#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace AgcDriver::Unified {

ResidentUploads::ResidentUploads(Residency& residency) : residency(residency) {}

void ResidentUploads::snapshot(std::uint64_t address, std::uint64_t size, bool full) {
    const auto end = address + size;
    auto cursor = address;
    while (cursor < end) {
        auto stop = std::min(end, (cursor & ~(ChunkBytes - 1)) + ChunkBytes);
        const auto first = residency.Translate(cursor, stop - cursor);
        if (!first) {
            counts.skippedBytes += stop - cursor;
            cursor = stop;
            continue;
        }
        while (stop < end) {
            const auto next = std::min(end, stop + ChunkBytes);
            const auto more = residency.Translate(stop, next - stop);
            if (!more || more->handle != first->handle || more->offset != first->offset + (stop - cursor)) break;
            stop = next;
        }
        PendingUpload upload{first->handle, first->offset, cursor, std::vector<std::byte>(static_cast<std::size_t>(stop - cursor))};
        std::memcpy(upload.bytes.data(), reinterpret_cast<const void*>(cursor), upload.bytes.size());
        if (full) {
            ++counts.fullUploads;
            counts.fullBytes += stop - cursor;
        } else {
            ++counts.dirtyUploads;
            counts.dirtyBytes += stop - cursor;
        }
        pending.push_back(std::move(upload));
        cursor = stop;
    }
}

void ResidentUploads::NoteDirty(std::span<const DirtyRange> ranges) {
    std::lock_guard lock(mutex);
    for (const auto& range : ranges) {
        if (range.size != 0) snapshot(range.address, range.size, false);
    }
}

bool ResidentUploads::Prepare(std::uint64_t address, std::uint64_t size, std::uint64_t frame) {
    if (size == 0) return true;
    std::lock_guard lock(mutex);
    if (!residency.Owns(address, size)) return false;
    for (const auto& span : residency.MakeResident(address, size, frame)) snapshot(span.address, span.size, true);
    return residency.Translate(address, size).has_value();
}

std::vector<PendingUpload> ResidentUploads::Take() {
    std::lock_guard lock(mutex);
    std::vector<PendingUpload> taken;
    taken.swap(pending);
    return taken;
}

std::vector<GuestSpan> ResidentUploads::Evict(std::uint64_t frame) {
    std::lock_guard lock(mutex);
    auto evicted = residency.Evict(frame);
    counts.evictedRanges += evicted.size();
    if (!evicted.empty()) {
        std::erase_if(pending, [&](const PendingUpload& upload) {
            return std::any_of(evicted.begin(), evicted.end(), [&](const GuestSpan& span) { return upload.guest >= span.address && upload.guest < span.address + span.size; });
        });
    }
    return evicted;
}

UploadCounts ResidentUploads::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

}
