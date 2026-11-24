#include "prx/libSceAgcDriver/Unified/include/GpuWritten.hpp"
#include <algorithm>
#include <limits>

namespace AgcDriver::Unified {

namespace {

constexpr std::uint64_t Never = std::numeric_limits<std::uint64_t>::max();

std::uint64_t endOf(std::uint64_t address, std::uint64_t size) {
    return size > Never - address ? Never : address + size;
}

}

GpuWritten::Pieces::iterator GpuWritten::firstOverlapping(std::uint64_t begin) {
    auto it = pieces.upper_bound(begin);
    if (it != pieces.begin() && std::prev(it)->second.end > begin) --it;
    return it;
}

GpuWritten::Pieces::const_iterator GpuWritten::firstOverlapping(std::uint64_t begin) const {
    auto it = pieces.upper_bound(begin);
    if (it != pieces.begin() && std::prev(it)->second.end > begin) --it;
    return it;
}

std::uint64_t GpuWritten::carve(std::uint64_t begin, std::uint64_t end, std::uint64_t completed, std::vector<WrittenRange>* removed, std::uint64_t onlySequence) {
    std::uint64_t bytes = 0;
    for (auto it = firstOverlapping(begin); it != pieces.end() && it->first < end;) {
        const auto pieceBegin = it->first;
        const auto piece = it->second;
        if (piece.serial > completed || (onlySequence != 0 && piece.sequence != onlySequence)) {
            ++it;
            continue;
        }
        const auto from = std::max(pieceBegin, begin);
        const auto to = std::min(piece.end, end);
        it = pieces.erase(it);
        if (pieceBegin < from) pieces.emplace(pieceBegin, Piece{from, piece.serial, piece.sequence});
        if (to < piece.end) it = pieces.emplace(to, Piece{piece.end, piece.serial, piece.sequence}).first;
        if (removed != nullptr) removed->push_back({from, to, piece.serial});
        bytes += to - from;
    }
    heldBytes -= bytes;
    return bytes;
}

std::uint64_t GpuWritten::Note(std::uint64_t address, std::uint64_t size, std::uint64_t serial) {
    if (size == 0) return 0;
    const auto end = endOf(address, size);
    std::lock_guard lock(mutex);
    auto sequence = ++nextSequence;
    carve(address, end, Never, nullptr);
    auto begin = address;
    auto last = end;
    auto next = pieces.lower_bound(begin);
    if (next != pieces.begin()) {
        const auto previous = std::prev(next);
        if (previous->second.end == begin && previous->second.serial == serial) {
            begin = previous->first;
            pieces.erase(previous);
            sequence = 0;
        }
    }
    if (next != pieces.end() && next->first == last && next->second.serial == serial) {
        last = next->second.end;
        pieces.erase(next);
        sequence = 0;
    }
    pieces.emplace(begin, Piece{last, serial, sequence});
    heldBytes += end - address;
    ++counts.noted;
    counts.notedBytes += end - address;
    return sequence;
}

std::uint64_t GpuWritten::TakeNoted(std::uint64_t address, std::uint64_t size, std::uint64_t sequence) {
    if (size == 0 || sequence == 0) return 0;
    std::lock_guard lock(mutex);
    const auto bytes = carve(address, endOf(address, size), Never, nullptr, sequence);
    ++counts.taken;
    counts.takenBytes += bytes;
    return bytes;
}

std::uint64_t GpuWritten::Pending(std::uint64_t address, std::uint64_t size) const {
    if (size == 0) return 0;
    const auto end = endOf(address, size);
    std::lock_guard lock(mutex);
    std::uint64_t serial = 0;
    for (auto it = firstOverlapping(address); it != pieces.end() && it->first < end; ++it) serial = std::max(serial, it->second.serial);
    return serial;
}

bool GpuWritten::Any(std::uint64_t address, std::uint64_t size) const {
    if (size == 0) return false;
    const auto end = endOf(address, size);
    std::lock_guard lock(mutex);
    const auto it = firstOverlapping(address);
    return it != pieces.end() && it->first < end;
}

std::vector<WrittenRange> GpuWritten::In(std::uint64_t address, std::uint64_t size) const {
    std::vector<WrittenRange> ranges;
    if (size == 0) return ranges;
    const auto end = endOf(address, size);
    std::lock_guard lock(mutex);
    for (auto it = firstOverlapping(address); it != pieces.end() && it->first < end; ++it) ranges.push_back({std::max(it->first, address), std::min(it->second.end, end), it->second.serial});
    return ranges;
}

std::vector<WrittenRange> GpuWritten::Take(std::uint64_t address, std::uint64_t size, std::uint64_t completed) {
    std::vector<WrittenRange> taken;
    if (size == 0) return taken;
    std::lock_guard lock(mutex);
    const auto bytes = carve(address, endOf(address, size), completed, &taken);
    counts.taken += taken.size();
    counts.takenBytes += bytes;
    return taken;
}

std::uint64_t GpuWritten::Discard(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return 0;
    std::lock_guard lock(mutex);
    const auto bytes = carve(address, endOf(address, size), Never, nullptr);
    counts.discardedBytes += bytes;
    return bytes;
}

GpuWrittenCounts GpuWritten::Counts() const {
    std::lock_guard lock(mutex);
    auto result = counts;
    result.pieces = pieces.size();
    result.bytes = heldBytes;
    return result;
}

}
