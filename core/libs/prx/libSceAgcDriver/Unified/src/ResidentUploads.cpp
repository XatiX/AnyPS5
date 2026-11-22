#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace AgcDriver::Unified {

ResidentUploads::ResidentUploads(Residency& residency, Committed committed) : residency(residency), committed(committed) {}

void ResidentUploads::SetView(View next) {
    std::lock_guard lock(mutex);
    view = next;
}

const std::byte* ResidentUploads::source(std::uint64_t address, std::size_t bytes) const {
    if (view != nullptr) {
        if (const auto* alias = view(address, bytes); alias != nullptr) return alias;
    }
    return reinterpret_cast<const std::byte*>(address);
}

void ResidentUploads::snapshot(std::uint64_t address, std::uint64_t size, bool full) {
    std::uint64_t covered = 0;
    for (const auto& piece : residency.ResidentPieces(address, size)) {
        const auto cursor = piece.guest;
        const auto stop = piece.guest + piece.device.bytes;
        if (full) {
            covered += piece.device.bytes;
            PendingUpload upload{piece.device.handle, piece.device.offset, cursor, std::vector<std::byte>(static_cast<std::size_t>(stop - cursor))};
            if (committed != nullptr) {
                for (const auto& [from, to] : committed(cursor, upload.bytes.size(), false)) {
                    std::memcpy(upload.bytes.data() + (from - cursor), source(from, static_cast<std::size_t>(to - from)), static_cast<std::size_t>(to - from));
                }
            } else {
                std::memcpy(upload.bytes.data(), source(cursor, upload.bytes.size()), upload.bytes.size());
            }
            ++counts.fullUploads;
            counts.fullBytes += stop - cursor;
            pending.push_back(std::move(upload));
            continue;
        }
        const auto readable = committed != nullptr ? committed(cursor, static_cast<std::size_t>(stop - cursor), false) : std::vector<std::pair<std::uint64_t, std::uint64_t>>{{cursor, stop}};
        for (const auto& [from, to] : readable) {
            covered += to - from;
            PendingUpload upload{piece.device.handle, piece.device.offset + (from - cursor), from, std::vector<std::byte>(static_cast<std::size_t>(to - from))};
            std::memcpy(upload.bytes.data(), source(from, upload.bytes.size()), upload.bytes.size());
            ++counts.dirtyUploads;
            counts.dirtyBytes += to - from;
            pending.push_back(std::move(upload));
        }
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
    const auto pieces = residency.Covering(address, size);
    if (pieces.empty()) return false;
    for (const auto& piece : pieces) {
        for (const auto& span : residency.MakeResident(piece.address, piece.size, frame)) snapshot(span.address, span.size, true);
    }
    return residency.Translate(address, size).has_value();
}

bool ResidentUploads::PrepareFilled(std::uint64_t address, std::uint64_t size, std::uint64_t frame) {
    if (size == 0) return true;
    std::lock_guard lock(mutex);
    const auto pieces = residency.Covering(address, size);
    if (pieces.empty()) return false;
    for (const auto& piece : pieces) static_cast<void>(residency.MakeResident(piece.address, piece.size, frame));
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
