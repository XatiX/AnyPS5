#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace AgcDriver::Unified {

ResidentUploads::ResidentUploads(Residency& residency, Committed committed) : residency(residency), committed(committed) {}

void ResidentUploads::snapshot(std::uint64_t address, std::uint64_t size, bool full) {
    std::uint64_t covered = 0;
    for (const auto& piece : residency.ResidentPieces(address, size)) {
        const auto cursor = piece.guest;
        const auto stop = piece.guest + piece.device.bytes;
        covered += piece.device.bytes;
        PendingUpload upload{piece.device.handle, piece.device.offset, cursor, std::vector<std::byte>(static_cast<std::size_t>(stop - cursor))};
        if (full && committed != nullptr) {
            for (const auto& [from, to] : committed(cursor, upload.bytes.size(), false)) {
                std::memcpy(upload.bytes.data() + (from - cursor), reinterpret_cast<const void*>(from), static_cast<std::size_t>(to - from));
            }
        } else {
            std::memcpy(upload.bytes.data(), reinterpret_cast<const void*>(cursor), upload.bytes.size());
        }
        if (full) {
            ++counts.fullUploads;
            counts.fullBytes += stop - cursor;
        } else {
            ++counts.dirtyUploads;
            counts.dirtyBytes += stop - cursor;
        }
        pending.push_back(std::move(upload));
    }
    counts.skippedBytes += size - covered;
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

bool ResidentUploads::Pending() const {
    std::lock_guard lock(mutex);
    return !pending.empty();
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
