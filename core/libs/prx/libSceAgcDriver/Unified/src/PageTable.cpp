#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <algorithm>

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

PageTable::Region* PageTable::regionOf(std::uint64_t page, bool create) {
    const auto index = page / RegionPages;
    const auto found = regions.find(index);
    if (found != regions.end()) return found->second.get();
    if (!create) return nullptr;
    return regions.emplace(index, std::make_unique<Region>()).first->second.get();
}

const PageTable::Region* PageTable::regionOf(std::uint64_t page) const {
    const auto found = regions.find(page / RegionPages);
    return found == regions.end() ? nullptr : found->second.get();
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

void PageTable::setState(std::uint64_t page, PageState next) {
    auto* region = regionOf(page, true);
    auto& state = region->states[slotOf(page)];
    if (state == PageState::GpuOwned && next != PageState::GpuOwned) --counts.gpuOwnedPages;
    if (state != PageState::GpuOwned && next == PageState::GpuOwned) ++counts.gpuOwnedPages;
    state = next;
}

std::vector<ProtectionChange> PageTable::Register(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, true);
        if (region->states[slotOf(page)] != PageState::Untracked) continue;
        setState(page, PageState::CpuDirty);
        region->busy[slotOf(page)] = 0;
        region->streak[slotOf(page)] = 0;
        list(page);
    }
    return {};
}

std::vector<ProtectionChange> PageTable::Unregister(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto state = region->states[slotOf(page)];
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
    switch (region->states[slotOf(page)]) {
        case PageState::CpuClean:
            ++counts.writeFaults;
            setState(page, PageState::CpuDirty);
            list(page);
            appendChange(changes, page, Protection::ReadWrite);
            return true;
        case PageState::CpuDirty: return true;
        default: return false;
    }
}

std::vector<DirtyRange> PageTable::ConsumeDirty(std::vector<ProtectionChange>& changes) {
    std::lock_guard lock(mutex);
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
        ++counts.consumedPages;
        const auto address = page * PageBytes;
        const auto gapSafe = [&] {
            for (auto gap = (ranges.back().address + ranges.back().size) / PageBytes; gap < page; ++gap) {
                const auto* holder = regionOf(gap);
                const auto state = holder == nullptr ? PageState::Untracked : holder->states[slotOf(gap)];
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
        auto& busy = region->busy[slot];
        auto& last = region->lastConsume[slot];
        streak = last + 1 == consumeEpoch ? static_cast<std::uint8_t>(std::min<int>(streak + 1, 255)) : 1;
        last = consumeEpoch;
        if (busy > 1) {
            --busy;
            keep.push_back(page);
            continue;
        }
        if (busy == 0 && streak >= BusyAfterWrites) {
            busy = BusyConsumes;
            ++counts.keptBusy;
            keep.push_back(page);
            continue;
        }
        if (busy == 1) streak = 0;
        busy = 0;
        ++counts.rearmed;
        setState(page, PageState::CpuClean);
        appendChange(changes, page, Protection::ReadOnly);
    }
    for (const auto page : keep) list(page);
    return ranges;
}

std::vector<ProtectionChange> PageTable::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    for (auto page = firstPage(address); page < endPage(address, size); ++page) {
        auto* region = regionOf(page, false);
        if (region == nullptr) continue;
        const auto slot = slotOf(page);
        const auto state = region->states[slot];
        if (state == PageState::Untracked || state == PageState::GpuOwned) continue;
        unlist(page);
        region->busy[slot] = 0;
        region->streak[slot] = 0;
        setState(page, PageState::GpuOwned);
        appendChange(changes, page, Protection::NoAccess);
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
    std::lock_guard lock(mutex);
    const auto page = firstPage(address);
    const auto* region = regionOf(page);
    return region == nullptr ? PageState::Untracked : region->states[slotOf(page)];
}

bool PageTable::RegionDirty(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto* region = regionOf(firstPage(address));
    return region != nullptr && region->dirtyPages != 0;
}

std::size_t PageTable::DirtyPages() const {
    std::lock_guard lock(mutex);
    std::size_t total = 0;
    for (const auto& [index, region] : regions) total += region->dirtyPages;
    return total;
}

PageTableCounts PageTable::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

}
