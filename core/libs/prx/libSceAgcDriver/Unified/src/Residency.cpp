#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include "prx/libSceAgcDriver/Unified/include/GpuPageTable.hpp"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>

namespace AgcDriver::Unified {

namespace {

constexpr std::uint64_t segmentBase(std::uint64_t address) {
    return address & ~(MirrorSegmentBytes - 1);
}

constexpr std::uint64_t chunkBase(std::uint64_t address) {
    return address & ~(ChunkBytes - 1);
}

}

Residency::Residency(ResidencyDevice& device, std::uint64_t budget) : device(device), budget(budget) {}

Residency::~Residency() {
    std::lock_guard lock(mutex);
    for (auto& [base, range] : ranges) release(range);
    ranges.clear();
    for (const auto& [base, buffer] : segments) device.DestroyBuffer(buffer.handle);
    segments.clear();
    for (std::uint32_t pool = 0; pool < pools; ++pool) device.DestroyPool(pool);
}

Residency::Ranges::iterator Residency::rangeOf(std::uint64_t address) {
    auto it = ranges.upper_bound(address);
    if (it == ranges.begin()) return ranges.end();
    --it;
    return address < it->second.end ? it : ranges.end();
}

Residency::Ranges::const_iterator Residency::rangeOf(std::uint64_t address) const {
    auto it = ranges.upper_bound(address);
    if (it == ranges.begin()) return ranges.end();
    --it;
    return address < it->second.end ? it : ranges.end();
}

const DeviceBuffer* Residency::segmentOf(std::uint64_t address) const {
    const auto found = segments.find(segmentBase(address));
    return found != segments.end() ? &found->second : nullptr;
}

const DeviceBuffer* Residency::segmentFor(std::uint64_t address) {
    const auto base = segmentBase(address);
    if (const auto found = segments.find(base); found != segments.end()) return &found->second;
    const auto buffer = device.CreateBuffer(MirrorSegmentBytes);
    if (!buffer) return nullptr;
    std::fprintf(stderr, "[recode-residency] mirror segment for guest 0x%llx+0x%llx at device 0x%llx\n", static_cast<unsigned long long>(base), static_cast<unsigned long long>(MirrorSegmentBytes), static_cast<unsigned long long>(buffer->address));
    return &segments.emplace(base, *buffer).first->second;
}

bool Residency::Add(std::uint64_t address, std::uint64_t size) {
    if (size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
    std::lock_guard lock(mutex);
    return add(address, size);
}

bool Residency::Cover(std::uint64_t address, std::uint64_t size, std::vector<GuestSpan>* gaps) {
    if (size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
    std::lock_guard lock(mutex);
    const auto end = address + size;
    auto it = rangeOf(address);
    if (it == ranges.end()) it = ranges.lower_bound(address);
    for (auto cursor = address; cursor < end;) {
        if (it != ranges.end() && it->first <= cursor) {
            cursor = it->second.end;
            ++it;
            continue;
        }
        const auto stop = it != ranges.end() ? std::min(end, it->first) : end;
        if (!add(cursor, stop - cursor)) return false;
        if (gaps != nullptr) gaps->push_back({cursor, stop - cursor});
        ++gapRanges;
        cursor = stop;
    }
    return true;
}

std::vector<GuestSpan> Residency::Covering(std::uint64_t address, std::uint64_t size) const {
    std::vector<GuestSpan> pieces;
    if (size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return pieces;
    std::lock_guard lock(mutex);
    const auto end = address + size;
    auto it = rangeOf(address);
    for (auto cursor = address; cursor < end; ++it) {
        if (it == ranges.end() || it->first > cursor) return {};
        const auto stop = std::min(end, it->second.end);
        pieces.push_back({cursor, stop - cursor});
        cursor = stop;
    }
    return pieces;
}

bool Residency::add(std::uint64_t address, std::uint64_t size) {
    const auto end = address + size;
    const auto next = ranges.lower_bound(address);
    if (next != ranges.end() && next->first < end) return false;
    if (next != ranges.begin() && std::prev(next)->second.end > address) return false;
    const auto origin = chunkBase(address);
    if (end - 1 > std::numeric_limits<std::uint64_t>::max() - ChunkBytes) return false;
    const auto bytes = chunkBase(end + ChunkBytes - 1) - origin;
    if (segmentBase(origin) != segmentBase(origin + bytes - 1) || segmentFor(origin) == nullptr) {
        ++refusedCount;
        return false;
    }
    Range range;
    range.end = end;
    range.origin = origin;
    range.held.resize(static_cast<std::size_t>(bytes / ChunkBytes));
    ranges.emplace(address, std::move(range));
    return true;
}

void Residency::Remove(std::uint64_t address) {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end()) return;
    release(it->second);
    ++removals;
    ranges.erase(it);
}

std::uint64_t Residency::Removals() const {
    std::lock_guard lock(mutex);
    return removals;
}

bool Residency::Owns(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    return it != ranges.end() && size <= it->second.end - address;
}

std::optional<Chunk> Residency::take() {
    if (free.empty()) {
        if (pools == std::numeric_limits<std::uint32_t>::max() || !device.CreatePool(pools)) return std::nullopt;
        free.reserve(free.size() + PoolChunks);
        for (auto index = PoolChunks; index-- > 0;) free.push_back({pools, index});
        ++pools;
    }
    const auto chunk = free.back();
    free.pop_back();
    return chunk;
}

void Residency::put(Chunk chunk) {
    free.push_back(chunk);
}

std::uint64_t Residency::release(Range& range) {
    if (range.heldChunks == 0) return 0;
    const auto* segment = segmentOf(range.origin);
    std::vector<ChunkBind> binds;
    for (std::size_t index = 0; index < range.held.size(); ++index) {
        if (!range.held[index]) continue;
        range.held[index] = false;
        const auto chunk = range.origin + index * ChunkBytes;
        const auto found = bound.find(chunk);
        if (found == bound.end()) continue;
        if (--found->second.holders != 0) continue;
        binds.push_back({chunk - segmentBase(chunk), found->second.chunk, false});
        if (segment != nullptr) unmapChunk(chunk, segment->address + (chunk - segmentBase(chunk)));
        put(found->second.chunk);
        bound.erase(found);
    }
    if (segment != nullptr && !binds.empty()) device.Bind(segment->handle, binds);
    const auto released = binds.size() * ChunkBytes;
    boundBytes -= released;
    range.heldChunks = 0;
    return released;
}

std::vector<GuestSpan> Residency::MakeResident(std::uint64_t address, std::uint64_t size, std::uint64_t frame) {
    std::vector<GuestSpan> made;
    if (size == 0) return made;
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end()) return made;
    const auto base = it->first;
    auto& range = it->second;
    range.lastUse = std::max(range.lastUse, frame);
    const auto* segment = segmentFor(range.origin);
    if (segment == nullptr) {
        ++refusedCount;
        return made;
    }
    const auto end = std::min(range.end, size > std::numeric_limits<std::uint64_t>::max() - address ? range.end : address + size);
    const auto first = static_cast<std::size_t>((address - range.origin) / ChunkBytes);
    const auto last = static_cast<std::size_t>((end - 1 - range.origin) / ChunkBytes);
    std::vector<ChunkBind> binds;
    std::vector<std::size_t> fresh;
    std::vector<std::size_t> joined;
    for (auto index = first; index <= last; ++index) {
        if (range.held[index]) continue;
        const auto chunk = range.origin + index * ChunkBytes;
        if (bound.contains(chunk)) {
            joined.push_back(index);
            continue;
        }
        const auto taken = take();
        if (!taken) {
            ++refusedCount;
            break;
        }
        binds.push_back({chunk - segmentBase(chunk), *taken, true});
        fresh.push_back(index);
    }
    if (binds.empty() && joined.empty()) return made;
    if (!binds.empty() && !device.Bind(segment->handle, binds)) {
        for (const auto& bind : binds) put(bind.chunk);
        ++refusedCount;
        return made;
    }
    for (std::size_t at = 0; at < fresh.size(); ++at) {
        const auto chunk = range.origin + fresh[at] * ChunkBytes;
        bound.emplace(chunk, Held{binds[at].chunk, 1u});
        range.held[fresh[at]] = true;
        mapChunk(chunk, segment->address + (chunk - segmentBase(chunk)));
    }
    for (const auto index : joined) {
        ++bound.find(range.origin + index * ChunkBytes)->second.holders;
        range.held[index] = true;
        ++sharedChunks;
    }
    std::vector<std::size_t> taken;
    taken.reserve(fresh.size() + joined.size());
    std::merge(fresh.begin(), fresh.end(), joined.begin(), joined.end(), std::back_inserter(taken));
    for (const auto index : taken) {
        const auto from = std::max(base, range.origin + index * ChunkBytes);
        const auto to = std::min(range.end, range.origin + (index + 1) * ChunkBytes);
        if (!made.empty() && made.back().address + made.back().size == from) made.back().size += to - from;
        else made.push_back({from, to - from});
    }
    range.heldChunks += taken.size();
    boundBytes += fresh.size() * ChunkBytes;
    boundCount += fresh.size();
    return made;
}

std::optional<DeviceSpan> Residency::Translate(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    auto it = rangeOf(address);
    if (it == ranges.end() || size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return std::nullopt;
    const auto end = address + size;
    if (segmentBase(address) != segmentBase(end - 1)) return std::nullopt;
    const auto* segment = segmentOf(address);
    if (segment == nullptr) return std::nullopt;
    for (auto cursor = address;;) {
        const auto& range = it->second;
        const auto stop = std::min(end, range.end);
        const auto first = static_cast<std::size_t>((cursor - range.origin) / ChunkBytes);
        const auto last = static_cast<std::size_t>((stop - 1 - range.origin) / ChunkBytes);
        for (auto index = first; index <= last; ++index) {
            if (!range.held[index]) return std::nullopt;
        }
        if (stop == end) break;
        const auto next = std::next(it);
        if (next == ranges.end() || next->first != range.end) return std::nullopt;
        it = next;
        cursor = stop;
    }
    const auto offset = address - segmentBase(address);
    return DeviceSpan{segment->handle, offset, segment->address + offset, size};
}

std::vector<ResidentPiece> Residency::ResidentPieces(std::uint64_t address, std::uint64_t size) const {
    std::vector<ResidentPiece> pieces;
    if (size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return pieces;
    const auto end = address + size;
    std::lock_guard lock(mutex);
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin() && std::prev(it)->second.end > address) --it;
    for (; it != ranges.end() && it->first < end; ++it) {
        const auto& range = it->second;
        const auto* segment = segmentOf(range.origin);
        if (segment == nullptr) continue;
        const auto stop = std::min(end, range.end);
        for (auto cursor = std::max(address, it->first); cursor < stop;) {
            const auto index = static_cast<std::size_t>((cursor - range.origin) / ChunkBytes);
            const auto chunkEnd = std::min(stop, range.origin + (index + 1) * ChunkBytes);
            if (range.held[index]) {
                const bool extends = !pieces.empty() && pieces.back().device.handle == segment->handle && pieces.back().guest + pieces.back().device.bytes == cursor;
                if (extends) {
                    pieces.back().device.bytes += chunkEnd - cursor;
                } else {
                    const auto offset = cursor - segmentBase(cursor);
                    pieces.push_back({cursor, {segment->handle, offset, segment->address + offset, chunkEnd - cursor}});
                }
            }
            cursor = chunkEnd;
        }
    }
    return pieces;
}

std::optional<DeviceSpan> Residency::Base(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end()) return std::nullopt;
    const auto* segment = segmentOf(address);
    if (segment == nullptr) return std::nullopt;
    const auto offset = address - segmentBase(address);
    return DeviceSpan{segment->handle, offset, segment->address + offset, it->second.end - address};
}

std::uint64_t Residency::MirrorAddress(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto* segment = segmentOf(address);
    return segment != nullptr ? segment->address + (address - segmentBase(address)) : 0u;
}

void Residency::Touch(std::uint64_t address, std::uint64_t frame) {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it != ranges.end()) it->second.lastUse = std::max(it->second.lastUse, frame);
}

std::size_t Residency::RemoveUnboundInside(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    const auto end = size > std::numeric_limits<std::uint64_t>::max() - address ? std::numeric_limits<std::uint64_t>::max() : address + size;
    std::size_t removed = 0;
    for (auto it = ranges.lower_bound(address); it != ranges.end() && it->first < end;) {
        if (it->second.end > end || it->second.heldChunks != 0) {
            ++it;
            continue;
        }
        ++removals;
        it = ranges.erase(it);
        ++removed;
    }
    return removed;
}

std::vector<GuestSpan> Residency::Evict(std::uint64_t frame) {
    std::vector<GuestSpan> evicted;
    std::lock_guard lock(mutex);
    while (budget != 0 && boundBytes > budget) {
        auto oldest = ranges.end();
        for (auto it = ranges.begin(); it != ranges.end(); ++it) {
            if (it->second.heldChunks == 0 || it->second.lastUse >= frame) continue;
            if (keep != nullptr && keep(it->first, it->second.end - it->first)) continue;
            if (oldest == ranges.end() || it->second.lastUse < oldest->second.lastUse) oldest = it;
        }
        if (oldest == ranges.end()) break;
        release(oldest->second);
        ++evictedCount;
        evicted.push_back({oldest->first, oldest->second.end - oldest->first});
    }
    return evicted;
}

void Residency::SetBudget(std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    budget = bytes;
}

void Residency::SetKeep(Keep next) {
    std::lock_guard lock(mutex);
    keep = next;
}

void Residency::mapChunk(std::uint64_t chunk, std::uint64_t deviceAddress) {
    if (pageTable == nullptr) return;
    if (const auto mapped = pageTable->Lookup(chunk); mapped != 0 && mapped != deviceAddress) {
        ++sharedPages;
        return;
    }
    pageTable->Map(chunk, ChunkBytes, deviceAddress, GpuPagePermissions);
}

void Residency::unmapChunk(std::uint64_t chunk, std::uint64_t deviceAddress) {
    if (pageTable == nullptr) return;
    if (pageTable->Lookup(chunk) == deviceAddress) pageTable->Unmap(chunk, ChunkBytes);
}

void Residency::SetPageTable(GpuPageTable* table) {
    std::lock_guard lock(mutex);
    pageTable = table;
    if (pageTable == nullptr) return;
    for (const auto& [chunk, held] : bound) {
        if (const auto* segment = segmentOf(chunk)) mapChunk(chunk, segment->address + (chunk - segmentBase(chunk)));
    }
}

ResidencyCounts Residency::Counts() const {
    std::lock_guard lock(mutex);
    return {ranges.size(), pools, boundBytes, budget, boundCount, evictedCount, refusedCount, sharedPages, segments.size(), removals, sharedChunks, gapRanges};
}

}
