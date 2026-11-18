#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include "prx/libSceAgcDriver/Unified/include/GpuPageTable.hpp"
#include <algorithm>
#include <limits>

namespace AgcDriver::Unified {

Residency::Residency(ResidencyDevice& device, std::uint64_t budget) : device(device), budget(budget) {}

Residency::~Residency() {
    std::lock_guard lock(mutex);
    for (auto& [base, range] : ranges) {
        unbind(range);
        device.DestroyBuffer(range.buffer.handle);
    }
    ranges.clear();
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

bool Residency::Add(std::uint64_t address, std::uint64_t size) {
    if (size == 0 || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
    std::lock_guard lock(mutex);
    const auto end = address + size;
    const auto next = ranges.lower_bound(address);
    if (next != ranges.end() && next->first < end) return false;
    if (next != ranges.begin() && std::prev(next)->second.end > address) return false;
    const auto origin = address & ~(ChunkBytes - 1);
    const auto bytes = ((end + ChunkBytes - 1) & ~(ChunkBytes - 1)) - origin;
    const auto buffer = device.CreateBuffer(bytes);
    if (!buffer) {
        ++refusedCount;
        return false;
    }
    Range range;
    range.end = end;
    range.buffer = *buffer;
    range.origin = origin;
    range.chunks.resize(static_cast<std::size_t>(bytes / ChunkBytes));
    ranges.emplace(address, std::move(range));
    return true;
}

void Residency::Remove(std::uint64_t address) {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end()) return;
    unbind(it->second);
    device.DestroyBuffer(it->second.buffer.handle);
    ranges.erase(it);
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

std::uint64_t Residency::unbind(Range& range) {
    if (range.boundChunks == 0) return 0;
    std::vector<ChunkBind> binds;
    binds.reserve(static_cast<std::size_t>(range.boundChunks));
    for (std::size_t index = 0; index < range.chunks.size(); ++index) {
        auto& slot = range.chunks[index];
        if (!slot) continue;
        binds.push_back({index * ChunkBytes, *slot, false});
        unmapChunk(range, index);
        put(*slot);
        slot.reset();
    }
    device.Bind(range.buffer.handle, binds);
    const auto released = range.boundChunks * ChunkBytes;
    boundBytes -= released;
    range.boundChunks = 0;
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
    const auto end = std::min(range.end, size > std::numeric_limits<std::uint64_t>::max() - address ? range.end : address + size);
    const auto first = static_cast<std::size_t>((address - range.origin) / ChunkBytes);
    const auto last = static_cast<std::size_t>((end - 1 - range.origin) / ChunkBytes);
    std::vector<ChunkBind> binds;
    std::vector<std::size_t> taken;
    for (auto index = first; index <= last; ++index) {
        if (range.chunks[index]) continue;
        const auto chunk = take();
        if (!chunk) {
            ++refusedCount;
            break;
        }
        binds.push_back({index * ChunkBytes, *chunk, true});
        taken.push_back(index);
    }
    if (binds.empty()) return made;
    if (!device.Bind(range.buffer.handle, binds)) {
        for (const auto& bind : binds) put(bind.chunk);
        ++refusedCount;
        return made;
    }
    for (std::size_t at = 0; at < taken.size(); ++at) {
        range.chunks[taken[at]] = binds[at].chunk;
        mapChunk(range, taken[at]);
        const auto from = std::max(base, range.origin + taken[at] * ChunkBytes);
        const auto to = std::min(range.end, range.origin + (taken[at] + 1) * ChunkBytes);
        if (!made.empty() && made.back().address + made.back().size == from) made.back().size += to - from;
        else made.push_back({from, to - from});
    }
    range.boundChunks += taken.size();
    boundBytes += taken.size() * ChunkBytes;
    boundCount += taken.size();
    return made;
}

std::optional<DeviceSpan> Residency::Translate(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end() || size == 0 || size > it->second.end - address) return std::nullopt;
    const auto& range = it->second;
    const auto first = static_cast<std::size_t>((address - range.origin) / ChunkBytes);
    const auto last = static_cast<std::size_t>((address + size - 1 - range.origin) / ChunkBytes);
    for (auto index = first; index <= last; ++index) {
        if (!range.chunks[index]) return std::nullopt;
    }
    const auto offset = address - range.origin;
    return DeviceSpan{range.buffer.handle, offset, range.buffer.address + offset, size};
}

std::optional<DeviceSpan> Residency::Base(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto it = rangeOf(address);
    if (it == ranges.end()) return std::nullopt;
    const auto offset = address - it->second.origin;
    return DeviceSpan{it->second.buffer.handle, offset, it->second.buffer.address + offset, it->second.end - address};
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
        if (it->second.end > end || it->second.boundChunks != 0) {
            ++it;
            continue;
        }
        device.DestroyBuffer(it->second.buffer.handle);
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
            if (it->second.boundChunks == 0 || it->second.lastUse >= frame) continue;
            if (oldest == ranges.end() || it->second.lastUse < oldest->second.lastUse) oldest = it;
        }
        if (oldest == ranges.end()) break;
        unbind(oldest->second);
        ++evictedCount;
        evicted.push_back({oldest->first, oldest->second.end - oldest->first});
    }
    return evicted;
}

void Residency::SetBudget(std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    budget = bytes;
}

void Residency::mapChunk(const Range& range, std::size_t index) {
    if (pageTable == nullptr) return;
    const auto guest = range.origin + index * ChunkBytes;
    const auto deviceAddress = range.buffer.address + index * ChunkBytes;
    if (const auto mapped = pageTable->Lookup(guest); mapped != 0 && mapped != deviceAddress) {
        ++sharedPages;
        return;
    }
    pageTable->Map(guest, ChunkBytes, deviceAddress, GpuPagePermissions);
}

void Residency::unmapChunk(const Range& range, std::size_t index) {
    if (pageTable == nullptr) return;
    const auto guest = range.origin + index * ChunkBytes;
    if (pageTable->Lookup(guest) == range.buffer.address + index * ChunkBytes) pageTable->Unmap(guest, ChunkBytes);
}

void Residency::SetPageTable(GpuPageTable* table) {
    std::lock_guard lock(mutex);
    pageTable = table;
    if (pageTable == nullptr) return;
    for (const auto& [base, range] : ranges) {
        for (std::size_t index = 0; index < range.chunks.size(); ++index) {
            if (range.chunks[index]) mapChunk(range, index);
        }
    }
}

ResidencyCounts Residency::Counts() const {
    std::lock_guard lock(mutex);
    return {ranges.size(), pools, boundBytes, budget, boundCount, evictedCount, refusedCount, sharedPages};
}

}
