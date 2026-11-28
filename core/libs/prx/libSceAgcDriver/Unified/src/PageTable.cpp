#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <algorithm>
#include <bit>
#include <thread>
#include <utility>

namespace AgcDriver::Unified {

namespace {

std::uint64_t firstPage(std::uint64_t address) {
    return address / PageBytes;
}

std::uint64_t endPage(std::uint64_t address, std::uint64_t size) {
    return (address + size + PageBytes - 1) / PageBytes;
}

std::size_t slotOf(std::uint64_t page) {
    return static_cast<std::size_t>(page % RegionPages);
}

}

PageTable::Region* PageTable::tracked(std::uint64_t page) const {
    const auto index = page / RegionPages;
    if (index >= LeafCount * LeafRegions) return nullptr;
    const auto* leaf = leaves[index >> LeafBits].load(std::memory_order_acquire);
    return leaf == nullptr ? nullptr : leaf->regions[index & (LeafRegions - 1)].load(std::memory_order_acquire);
}

PageTable::Region* PageTable::regionOf(std::uint64_t page, bool create) {
    const auto index = page / RegionPages;
    const bool far = index >= LeafCount * LeafRegions;
    if (far) {
        if (const auto found = farRegions.find(index); found != farRegions.end()) return found->second;
    } else if (auto* found = tracked(page)) {
        return found;
    }
    if (!create) return nullptr;
    auto* region = regions.emplace_back(index, std::make_unique<Region>()).second.get();
    if (far) {
        farRegions.emplace(index, region);
        return region;
    }
    auto* leaf = leaves[index >> LeafBits].load(std::memory_order_acquire);
    if (leaf == nullptr) {
        leaf = ownedLeaves.emplace_back(std::make_unique<Leaf>()).get();
        leaves[index >> LeafBits].store(leaf, std::memory_order_release);
    }
    leaf->regions[index & (LeafRegions - 1)].store(region, std::memory_order_release);
    return region;
}

const PageTable::Region* PageTable::regionOf(std::uint64_t page) const {
    const auto index = page / RegionPages;
    if (index < LeafCount * LeafRegions) return tracked(page);
    const auto found = farRegions.find(index);
    return found == farRegions.end() ? nullptr : found->second;
}

Protection PageTable::protectionOf(PageState state) {
    switch (state) {
        case PageState::CpuClean: return Protection::ReadOnly;
        case PageState::GpuOwned: return Protection::NoAccess;
        default: return Protection::ReadWrite;
    }
}

void PageTable::appendChange(std::vector<ProtectionChange>& changes, std::uint64_t page, Protection protection) {
    const auto address = page * PageBytes;
    if (!changes.empty()) {
        auto& last = changes.back();
        if (last.protection == protection && last.address + last.size == address) {
            last.size += PageBytes;
            return;
        }
    }
    changes.push_back({address, PageBytes, protection});
}

void PageTable::list(std::uint64_t page) {
    auto* region = regionOf(page, true);
    const auto slot = slotOf(page);
    auto& word = region->listed[slot / 64];
    const auto bit = std::uint64_t{1} << (slot % 64);
    if ((word & bit) != 0) return;
    word |= bit;
    ++region->dirtyPages;
    dirty.push_back(page);
}

void PageTable::unlist(std::uint64_t page) {
    auto* region = regionOf(page, false);
    if (region == nullptr) return;
    const auto slot = slotOf(page);
    auto& word = region->listed[slot / 64];
    const auto bit = std::uint64_t{1} << (slot % 64);
    if ((word & bit) == 0) return;
    word &= ~bit;
    --region->dirtyPages;
}

void PageTable::account(Region& region, std::size_t slot, PageState state, PageState next) {
    if (state == PageState::GpuOwned && next != PageState::GpuOwned) --counts.gpuOwnedPages;
    if (state != PageState::GpuOwned && next == PageState::GpuOwned) ++counts.gpuOwnedPages;
    const auto followed = [](PageState value) { return value == PageState::CpuClean || value == PageState::CpuDirty; };
    if (followed(state) != followed(next)) followed(next) ? ++region.trackedPages : --region.trackedPages;
    const bool dirtyChanged = (state == PageState::CpuDirty) != (next == PageState::CpuDirty);
    if (dirtyChanged) next == PageState::CpuDirty ? ++region.writablePages : --region.writablePages;
    const auto bit = std::uint64_t{1} << (slot % 64);
    if (dirtyChanged || (!followed(state) && followed(next))) {
        auto& word = region.unreported[slot / 64];
        if ((word & bit) == 0) {
            word |= bit;
            ++region.unreportedPages;
        }
        if (next == PageState::CpuDirty && (state == PageState::Reserved || state == PageState::Untracked)) region.written[slot / 64] &= ~bit;
        else if (next == PageState::CpuDirty) region.written[slot / 64] |= bit;
    }
    if (!followed(next) || next == PageState::CpuDirty) region.device[slot / 64] &= ~bit;
}

void PageTable::settle(Region& region, std::uint64_t page, bool listed) {
    const auto slot = slotOf(page);
    const auto bit = std::uint64_t{1} << (slot % 64);
    while ((region.opening[slot / 64].load(std::memory_order_acquire) & bit) != 0) std::this_thread::yield();
    if ((region.faulted[slot / 64].load(std::memory_order_acquire) & bit) == 0) return;
    region.faulted[slot / 64].fetch_and(~bit, std::memory_order_acq_rel);
    pendingFaults.fetch_sub(1, std::memory_order_acq_rel);
    ++counts.writeFaults;
    account(region, slot, PageState::CpuClean, PageState::CpuDirty);
    if (listed && region.states[slot].load(std::memory_order_acquire) == PageState::CpuDirty) list(page);
}

void PageTable::drainFaults() {
    if (pendingFaults.load(std::memory_order_acquire) == 0) return;
    for (std::size_t at = 0; at < regions.size(); ++at) {
        auto& region = *regions[at].second;
        if (!region.hasFaults.exchange(false, std::memory_order_acq_rel)) continue;
        for (std::size_t word = 0; word < region.faulted.size(); ++word) {
            auto bits = region.faulted[word].load(std::memory_order_acquire);
            while (bits != 0) {
                const auto slot = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
                bits &= bits - 1;
                settle(region, regions[at].first * RegionPages + slot, true);
            }
        }
    }
}

bool PageTable::BeginWriteFault(std::uint64_t address) {
    const auto page = firstPage(address);
    auto* region = tracked(page);
    if (region == nullptr) return false;
    const auto slot = slotOf(page);
    const auto bit = std::uint64_t{1} << (slot % 64);
    region->opening[slot / 64].fetch_or(bit, std::memory_order_acq_rel);
    auto expected = PageState::CpuClean;
    if (!region->states[slot].compare_exchange_strong(expected, PageState::CpuDirty, std::memory_order_acq_rel)) {
        region->opening[slot / 64].fetch_and(~bit, std::memory_order_acq_rel);
        return false;
    }
    const auto pendingAt = [&](std::size_t other) { return (region->faulted[other / 64].load(std::memory_order_relaxed) & (std::uint64_t{1} << (other % 64))) != 0; };
    if ((slot > 0 && pendingAt(slot - 1)) || (slot + 1 < RegionPages && pendingAt(slot + 1))) adjacentFaults.fetch_add(1, std::memory_order_relaxed);
    const auto chunk = slot & ~std::size_t{15};
    bool first = true;
    for (std::size_t other = chunk; other < chunk + 16 && first; ++other) first = !pendingAt(other);
    if (first) chunkFirstFaults.fetch_add(1, std::memory_order_relaxed);
    region->faulted[slot / 64].fetch_or(bit, std::memory_order_acq_rel);
    region->hasFaults.store(true, std::memory_order_release);
    pendingFaults.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void PageTable::EndWriteFault(std::uint64_t address) {
    const auto page = firstPage(address);
    auto* region = tracked(page);
    if (region == nullptr) return;
    const auto slot = slotOf(page);
    region->opening[slot / 64].fetch_and(~(std::uint64_t{1} << (slot % 64)), std::memory_order_acq_rel);
}

std::vector<DirtyRange> PageTable::GrowWriteFault(std::uint64_t address) {
    std::vector<DirtyRange> runs;
    if (hasher == nullptr) return runs;
    const auto page = firstPage(address);
    auto* region = tracked(page);
    if (region == nullptr) return runs;
    const auto slot = slotOf(page);
    const auto test = [&](const std::array<std::atomic<std::uint64_t>, RegionPages / 64>& bits, std::size_t at) { return (bits[at / 64].load(std::memory_order_acquire) & (std::uint64_t{1} << (at % 64))) != 0; };
    std::size_t first = slot;
    std::size_t last = slot;
    if (test(region->whole, slot)) {
        while (first > 0 && slot - (first - 1) < WholeLimitPages && test(region->whole, first - 1)) --first;
        while (last + 1 < RegionPages && last + 1 - first < WholeLimitPages && test(region->whole, last + 1)) ++last;
        wholeFaults.fetch_add(1, std::memory_order_relaxed);
    } else if ((slot > 0 && test(region->faulted, slot - 1)) || (slot + 1 < RegionPages && test(region->faulted, slot + 1))) {
        first = slot & ~(GrowChunkPages - 1);
        last = first + GrowChunkPages - 1;
    } else {
        return runs;
    }
    const auto base = page - slot;
    for (auto at = first; at <= last; ++at) {
        if (at == slot) continue;
        const auto bit = std::uint64_t{1} << (at % 64);
        if (region->states[at].load(std::memory_order_acquire) != PageState::CpuClean) continue;
        region->opening[at / 64].fetch_or(bit, std::memory_order_acq_rel);
        std::uint64_t hash = 0;
        auto expected = PageState::CpuClean;
        if (!hasher(base + at, hash) || !region->states[at].compare_exchange_strong(expected, PageState::CpuDirty, std::memory_order_acq_rel)) {
            region->opening[at / 64].fetch_and(~bit, std::memory_order_acq_rel);
            continue;
        }
        region->openHash[at] = hash;
        region->unproven[at / 64].fetch_or(bit, std::memory_order_acq_rel);
        region->faulted[at / 64].fetch_or(bit, std::memory_order_acq_rel);
        pendingFaults.fetch_add(1, std::memory_order_acq_rel);
        const auto address = (base + at) * PageBytes;
        if (!runs.empty() && runs.back().address + runs.back().size == address) runs.back().size += PageBytes;
        else runs.push_back({address, PageBytes});
        grownPages.fetch_add(1, std::memory_order_relaxed);
    }
    if (!runs.empty()) {
        region->hasFaults.store(true, std::memory_order_release);
        grownFaults.fetch_add(1, std::memory_order_relaxed);
    }
    return runs;
}

void PageTable::EndGrow(std::span<const DirtyRange> runs) {
    for (const auto& run : runs) {
        for (auto at = run.address; at < run.address + run.size; at += PageBytes) EndWriteFault(at);
    }
}

void PageTable::OpenWhole(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    std::lock_guard lock(mutex);
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        region->whole[slot / 64].fetch_or(std::uint64_t{1} << (slot % 64), std::memory_order_acq_rel);
    }
}

std::vector<UnprovenPage> PageTable::TakeRearmedUnproven() {
    std::lock_guard lock(mutex);
    return std::exchange(rearmedUnproven, {});
}

PageState PageTable::setState(std::uint64_t page, PageState next) {
    auto* region = regionOf(page, true);
    const auto slot = slotOf(page);
    auto& cell = region->states[slot];
    auto state = cell.load(std::memory_order_acquire);
    if (state == PageState::CpuClean && next == PageState::CpuClean) return state;
    if (state == PageState::CpuClean && cell.compare_exchange_strong(state, next, std::memory_order_acq_rel)) {
        account(*region, slot, PageState::CpuClean, next);
        return PageState::CpuClean;
    }
    settle(*region, page, false);
    state = cell.exchange(next, std::memory_order_acq_rel);
    account(*region, slot, state, next);
    return state;
}

std::vector<ProtectionChange> PageTable::Register(std::uint64_t address, std::uint64_t size) {
    Reserve(address, size);
    return MakeResident(address, size);
}

void PageTable::Reserve(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, true);
        if (region->states[slotOf(page)] == PageState::Untracked) setState(page, PageState::Reserved);
    }
}

std::vector<ProtectionChange> PageTable::MakeResident(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr || region->states[slotOf(page)] != PageState::Reserved) continue;
        setState(page, PageState::CpuDirty);
        region->busyUntil[slotOf(page)] = 0;
        region->streak[slotOf(page)] = 0;
        list(page);
    }
    return {};
}

bool PageTable::OwnedBy(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    if (size == 0) return false;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        const auto* region = regionOf(page);
        if (region == nullptr || region->states[slotOf(page)] == PageState::Untracked) return false;
    }
    return true;
}

std::vector<std::uint64_t> PageTable::BusyPages() const {
    std::lock_guard lock(mutex);
    std::vector<std::uint64_t> busy;
    for (const auto page : dirty) {
        const auto* region = regionOf(page);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        if (region->busyUntil[slot] != 0 && region->states[slot] == PageState::CpuDirty) busy.push_back(page);
    }
    std::sort(busy.begin(), busy.end());
    busy.erase(std::unique(busy.begin(), busy.end()), busy.end());
    return busy;
}

std::vector<ProtectionChange> PageTable::Unregister(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto state = region->states[slotOf(page)].load(std::memory_order_acquire);
        if (state == PageState::Untracked) continue;
        if (protectionOf(state) != Protection::ReadWrite) appendChange(changes, page, Protection::ReadWrite);
        unlist(page);
        setState(page, PageState::Untracked);
    }
    return changes;
}

bool PageTable::HandleWrite(std::uint64_t address, std::vector<ProtectionChange>& changes) {
    std::lock_guard lock(mutex);
    const auto page = firstPage(address);
    auto* region = regionOf(page, false);
    if (region == nullptr) return false;
    switch (region->states[slotOf(page)].load(std::memory_order_acquire)) {
        case PageState::CpuClean:
            if (setState(page, PageState::CpuDirty) == PageState::CpuClean) ++counts.writeFaults;
            list(page);
            appendChange(changes, page, Protection::ReadWrite);
            return true;
        case PageState::CpuDirty: return true;
        default: return false;
    }
}

void PageTable::AdvanceFrame() {
    std::lock_guard lock(mutex);
    framePaced = true;
    ++frameEpoch;
}

std::vector<DirtyRange> PageTable::ConsumeDirty(std::vector<ProtectionChange>& changes, const WriteCheck& written) {
    std::lock_guard lock(mutex);
    drainFaults();
    ++consumeEpoch;
    ++counts.consumes;
    std::vector<std::uint64_t> pages;
    pages.swap(dirty);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    std::vector<DirtyRange> ranges;
    std::vector<std::uint64_t> keep;
    for (const auto page : pages) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        if ((region->listed[slot / 64] & (std::uint64_t{1} << (slot % 64))) == 0) continue;
        unlist(page);
        if (region->states[slot] != PageState::CpuDirty) continue;
        if (const auto bit = std::uint64_t{1} << (slot % 64); (region->unproven[slot / 64].load(std::memory_order_acquire) & bit) != 0) {
            region->unproven[slot / 64].fetch_and(~bit, std::memory_order_acq_rel);
            std::uint64_t now = 0;
            if (hasher != nullptr && hasher(page, now) && now == region->openHash[slot]) {
                ++counts.unprovenUnchanged;
                region->streak[slot] = 0;
                setState(page, PageState::CpuClean);
                appendChange(changes, page, Protection::ReadOnly);
                rearmedUnproven.push_back({page, region->openHash[slot]});
                continue;
            }
            ++counts.unprovenChanged;
        }
        if (region->busyUntil[slot] != 0 && written && !written(page, false)) {
            ++counts.unchangedBusy;
            const auto epoch = framePaced ? frameEpoch : consumeEpoch;
            if (region->busyUntil[slot] > epoch && epoch - region->lastConsume[slot] < QuietEpochs) {
                keep.push_back(page);
                continue;
            }
            region->busyUntil[slot] = 0;
            region->streak[slot] = 0;
            ++counts.rearmed;
            setState(page, PageState::CpuClean);
            appendChange(changes, page, Protection::ReadOnly);
            continue;
        }
        ++counts.consumedPages;
        const auto address = page * PageBytes;
        const auto gapSafe = [&] {
            for (auto gap = (ranges.back().address + ranges.back().size) / PageBytes; gap < page; ++gap) {
                const auto* holder = regionOf(gap);
                const auto state = holder == nullptr ? PageState::Untracked : holder->states[slotOf(gap)].load(std::memory_order_acquire);
                if (state != PageState::CpuClean && state != PageState::CpuDirty) return false;
            }
            return true;
        };
        if (!ranges.empty() && address <= ranges.back().address + ranges.back().size + MaxGapPages * PageBytes && gapSafe()) {
            ranges.back().size = address + PageBytes - ranges.back().address;
        } else {
            ranges.push_back({address, PageBytes});
        }
        auto& streak = region->streak[slot];
        auto& busyUntil = region->busyUntil[slot];
        auto& last = region->lastConsume[slot];
        const auto epoch = framePaced ? frameEpoch : consumeEpoch;
        if (last != epoch) streak = last + 1 == epoch ? static_cast<std::uint8_t>(std::min<int>(streak + 1, 255)) : 1;
        last = epoch;
        if (busyUntil > epoch) {
            keep.push_back(page);
            continue;
        }
        if (busyUntil == 0 && streak >= BusyAfterWrites) {
            busyUntil = epoch + BusyConsumes;
            ++counts.keptBusy;
            if (written) static_cast<void>(written(page, true));
            keep.push_back(page);
            continue;
        }
        if (busyUntil != 0) streak = 0;
        busyUntil = 0;
        ++counts.rearmed;
        setState(page, PageState::CpuClean);
        appendChange(changes, page, Protection::ReadOnly);
    }
    for (const auto page : keep) list(page);
    return ranges;
}

std::vector<DirtyRange> PageTable::ConsumeDirtyIn(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans, std::vector<ProtectionChange>& changes, const WriteCheck& written) {
    std::lock_guard lock(mutex);
    drainFaults();
    std::vector<std::uint64_t> pages;
    for (const auto& [address, size] : spans) {
        if (size == 0u) continue;
        for (auto page = firstPage(address); page < endPage(address, size); ++page) {
            auto* region = regionOf(page, false);
            if (region == nullptr) {
                page = (page / RegionPages + 1u) * RegionPages - 1u;
                continue;
            }
            const auto slot = slotOf(page);
            if ((region->listed[slot / 64] & (std::uint64_t{1} << (slot % 64))) == 0) {
                if (region->listed[slot / 64] == 0 && slot % 64 == 0) page += 63u;
                continue;
            }
            if (region->states[slot] == PageState::CpuDirty) pages.push_back(page);
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    std::vector<DirtyRange> ranges;
    const auto epoch = framePaced ? frameEpoch : consumeEpoch;
    for (const auto page : pages) {
        auto* region = regionOf(page, false);
        const auto slot = slotOf(page);
        const auto address = page * PageBytes;
        if (const auto bit = std::uint64_t{1} << (slot % 64); (region->unproven[slot / 64].load(std::memory_order_acquire) & bit) != 0) {
            std::uint64_t now = 0;
            if (hasher != nullptr && hasher(page, now) && now == region->openHash[slot]) continue;
        }
        if (region->busyUntil[slot] != 0) {
            if (written && !written(page, false)) continue;
            region->lastConsume[slot] = epoch;
            ranges.push_back({address, PageBytes});
            ++counts.rangedPages;
            continue;
        }
        auto& streak = region->streak[slot];
        auto& last = region->lastConsume[slot];
        if (last != epoch) streak = last + 1 == epoch ? static_cast<std::uint8_t>(std::min<int>(streak + 1, 255)) : 1;
        last = epoch;
        if (framePaced && streak >= BusyAfterWrites) {
            region->busyUntil[slot] = epoch + BusyConsumes;
            region->unproven[slot / 64].fetch_and(~(std::uint64_t{1} << (slot % 64)), std::memory_order_acq_rel);
            ++counts.keptBusy;
            ++counts.rangedBusy;
            if (written) static_cast<void>(written(page, true));
            ranges.push_back({address, PageBytes});
            ++counts.rangedPages;
            continue;
        }
        unlist(page);
        region->unproven[slot / 64].fetch_and(~(std::uint64_t{1} << (slot % 64)), std::memory_order_acq_rel);
        setState(page, PageState::CpuClean);
        appendChange(changes, page, Protection::ReadOnly);
        if (!ranges.empty() && ranges.back().address + ranges.back().size == address) ranges.back().size += PageBytes;
        else ranges.push_back({address, PageBytes});
        ++counts.rangedPages;
    }
    ++counts.rangedConsumes;
    return ranges;
}

std::vector<ProtectionChange> PageTable::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        const auto state = region->states[slot].load(std::memory_order_acquire);
        if (state == PageState::Untracked || state == PageState::GpuOwned || state == PageState::Reserved) continue;
        unlist(page);
        region->busyUntil[slot] = 0;
        region->streak[slot] = 0;
        setState(page, PageState::GpuOwned);
        appendChange(changes, page, Protection::NoAccess);
    }
    return changes;
}

GpuTake PageTable::TakeForGpu(std::uint64_t address, std::uint64_t size, bool takeDirty) {
    GpuTake take;
    if (size == 0) return take;
    const auto append = [](std::vector<DirtyRange>& list, std::uint64_t begin, std::uint64_t end) {
        if (end <= begin) return;
        if (!list.empty() && list.back().address + list.back().size == begin) list.back().size += end - begin;
        else list.push_back({begin, end - begin});
    };
    const auto end = address + size;
    const auto firstWhole = (address + PageBytes - 1) & ~(PageBytes - 1);
    const auto lastWhole = end & ~(PageBytes - 1);
    if (firstWhole >= lastWhole) {
        append(take.open, address, end);
        return take;
    }
    std::lock_guard lock(mutex);
    append(take.open, address, firstWhole);
    for (auto page = firstWhole / PageBytes; page < lastWhole / PageBytes; ++page) {
        const auto at = page * PageBytes;
        auto* region = regionOf(page, false);
        const auto slot = slotOf(page);
        if (region != nullptr) settle(*region, page, true);
        const auto state = region == nullptr ? PageState::Untracked : region->states[slot].load(std::memory_order_acquire);
        if (state == PageState::GpuOwned) {
            append(take.owned, at, at + PageBytes);
            continue;
        }
        const bool followed = state == PageState::CpuClean || state == PageState::CpuDirty;
        if (!followed || region->busyUntil[slot] != 0 || (!takeDirty && state == PageState::CpuDirty)) {
            append(take.open, at, at + PageBytes);
            continue;
        }
        unlist(page);
        region->streak[slot] = 0;
        const bool dirty = setState(page, PageState::GpuOwned) == PageState::CpuDirty;
        if (dirty && !takeDirty) {
            setState(page, PageState::CpuDirty);
            list(page);
            append(take.open, at, at + PageBytes);
            continue;
        }
        if (dirty) append(take.dirty, at, at + PageBytes);
        appendChange(take.changes, page, Protection::NoAccess);
        append(take.owned, at, at + PageBytes);
    }
    append(take.open, lastWhole, end);
    return take;
}

std::vector<ProtectionChange> PageTable::MarkDeviceWritten(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        settle(*region, page, true);
        const auto state = region->states[slot].load(std::memory_order_acquire);
        if (state != PageState::CpuClean && state != PageState::CpuDirty) continue;
        if (state == PageState::CpuDirty) {
            unlist(page);
            region->busyUntil[slot] = 0;
            region->streak[slot] = 0;
            setState(page, PageState::CpuClean);
            appendChange(changes, page, Protection::ReadOnly);
        }
        const auto bit = std::uint64_t{1} << (slot % 64);
        if ((region->unreported[slot / 64] & bit) != 0) {
            region->unreported[slot / 64] &= ~bit;
            --region->unreportedPages;
        }
        region->written[slot / 64] &= ~bit;
        region->device[slot / 64] |= bit;
    }
    return changes;
}

std::vector<DirtyRange> PageTable::GpuOwnedIn(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    std::vector<DirtyRange> ranges;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        const auto* region = regionOf(page);
        if (region == nullptr || region->states[slotOf(page)] != PageState::GpuOwned) continue;
        const auto at = page * PageBytes;
        if (!ranges.empty() && ranges.back().address + ranges.back().size == at) ranges.back().size += PageBytes;
        else ranges.push_back({at, PageBytes});
    }
    return ranges;
}

bool PageTable::CleanIn(std::uint64_t address, std::uint64_t size) const {
    if (size == 0) return false;
    std::lock_guard lock(mutex);
    const auto end = endPage(address, size);
    for (auto page = firstPage(address); page < end; ++page) {
        const auto* region = regionOf(page);
        const auto slot = slotOf(page);
        if (region == nullptr || region->states[slot] != PageState::CpuClean || (region->gpuWritten[slot / 64] & (std::uint64_t{1} << (slot % 64))) != 0) return false;
    }
    return true;
}

void PageTable::NoteGpuWritten(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    const auto end = endPage(address, size);
    for (auto page = firstPage(address); page < end; ++page) {
        auto* region = regionOf(page, true);
        const auto slot = slotOf(page);
        auto& word = region->gpuWritten[slot / 64];
        const auto bit = std::uint64_t{1} << (slot % 64);
        if ((word & bit) != 0) continue;
        word |= bit;
        ++counts.gpuWrittenPages;
    }
}

bool PageTable::GpuWrittenIn(std::uint64_t address, std::uint64_t size) const {
    std::lock_guard lock(mutex);
    const auto end = endPage(address, size);
    for (auto page = firstPage(address); page < end; ++page) {
        const auto* region = regionOf(page);
        const auto slot = slotOf(page);
        if (region != nullptr && (region->gpuWritten[slot / 64] & (std::uint64_t{1} << (slot % 64))) != 0) return true;
    }
    return false;
}

std::vector<ProtectionChange> PageTable::Reclaim(std::uint64_t address, std::uint64_t size, bool forWrite) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    ++counts.gpuFaults;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr || region->states[slotOf(page)] != PageState::GpuOwned) continue;
        if (forWrite) {
            setState(page, PageState::CpuDirty);
            list(page);
            appendChange(changes, page, Protection::ReadWrite);
        } else {
            setState(page, PageState::CpuClean);
            appendChange(changes, page, Protection::ReadOnly);
        }
    }
    return changes;
}

PageState PageTable::StateOf(std::uint64_t address) const {
    const auto page = firstPage(address);
    if (page / RegionPages < LeafCount * LeafRegions) {
        const auto* region = tracked(page);
        return region == nullptr ? PageState::Untracked : region->states[slotOf(page)].load(std::memory_order_acquire);
    }
    std::lock_guard lock(mutex);
    const auto* region = regionOf(page);
    return region == nullptr ? PageState::Untracked : region->states[slotOf(page)].load(std::memory_order_acquire);
}

bool PageTable::Covers(std::uint64_t address, std::uint64_t size) const {
    if (size == 0) return false;
    std::lock_guard lock(mutex);
    const auto end = endPage(address, size);
    for (auto page = firstPage(address); page < end;) {
        const auto* region = regionOf(page);
        if (region == nullptr) return false;
        const auto regionEnd = (page / RegionPages + 1) * RegionPages;
        if (region->trackedPages == RegionPages) {
            page = regionEnd;
            continue;
        }
        for (; page < std::min(end, regionEnd); ++page) {
            const auto state = region->states[slotOf(page)].load(std::memory_order_acquire);
            if (state != PageState::CpuClean && state != PageState::CpuDirty) return false;
        }
    }
    return true;
}

void PageTable::Collect(std::uint64_t address, std::uint64_t size, WrittenRun written, void* context, const WriteCheck& changed) {
    if (size == 0) return;
    std::lock_guard lock(mutex);
    drainFaults();
    const auto end = endPage(address, size);
    std::uint64_t runBegin = 0;
    std::uint64_t runEnd = 0;
    const auto flush = [&] {
        if (runEnd > runBegin) written(context, static_cast<std::uintptr_t>(runBegin * PageBytes), static_cast<std::uintptr_t>(runEnd * PageBytes));
        runBegin = runEnd = 0;
    };
    for (auto page = firstPage(address); page < end;) {
        auto* region = regionOf(page, false);
        const auto regionEnd = (page / RegionPages + 1) * RegionPages;
        if (region == nullptr || (region->writablePages == 0 && region->unreportedPages == 0)) {
            page = regionEnd;
            continue;
        }
        for (; page < std::min(end, regionEnd); ++page) {
            const auto slot = slotOf(page);
            auto& word = region->unreported[slot / 64];
            const auto bit = std::uint64_t{1} << (slot % 64);
            const bool unreported = (word & bit) != 0;
            auto& writtenWord = region->written[slot / 64];
            const bool proven = (writtenWord & bit) != 0;
            if (unreported) {
                word &= ~bit;
                writtenWord &= ~bit;
                --region->unreportedPages;
            }
            if (!unreported && region->states[slot] != PageState::CpuDirty) continue;
            const bool device = (region->device[slot / 64] & bit) != 0;
            if (device && !proven) {
                if (changed) static_cast<void>(changed(page, false));
                continue;
            }
            if (changed && !changed(page, proven)) continue;
            if (runEnd != page) {
                flush();
                runBegin = page;
            }
            runEnd = page + 1;
        }
    }
    flush();
}

bool PageTable::RegionDirty(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const_cast<PageTable*>(this)->drainFaults();
    const auto* region = regionOf(firstPage(address));
    return region != nullptr && region->dirtyPages != 0;
}

std::size_t PageTable::DirtyPages() const {
    std::lock_guard lock(mutex);
    const_cast<PageTable*>(this)->drainFaults();
    std::size_t total = 0;
    for (const auto& [index, region] : regions) total += region->dirtyPages;
    return total;
}

PageTableCounts PageTable::Counts() const {
    std::lock_guard lock(mutex);
    const_cast<PageTable*>(this)->drainFaults();
    auto total = counts;
    total.adjacentFaults = adjacentFaults.load(std::memory_order_relaxed);
    total.grownFaults = grownFaults.load(std::memory_order_relaxed);
    total.grownPages = grownPages.load(std::memory_order_relaxed);
    total.wholeFaults = wholeFaults.load(std::memory_order_relaxed);
    total.chunkFirstFaults = chunkFirstFaults.load(std::memory_order_relaxed);
    return total;
}

}
