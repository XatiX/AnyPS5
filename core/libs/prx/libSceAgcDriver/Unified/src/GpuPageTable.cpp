#include "prx/libSceAgcDriver/Unified/include/GpuPageTable.hpp"
#include <utility>

namespace AgcDriver::Unified {

GpuPageTable::GpuPageTable() : root(GpuRootEntries, 0) {}

std::uint64_t* GpuPageTable::entry(std::uint64_t page, bool create) {
    const auto slot = static_cast<std::uint32_t>(page >> GpuLeafBits);
    auto& index = root[slot];
    if (index == 0) {
        if (!create) return nullptr;
        leaves.push_back(std::make_unique<LeafEntries>(GpuLeafEntries, 0));
        leafDirty.push_back(false);
        index = static_cast<std::uint32_t>(leaves.size());
        dirty.root = true;
    }
    return &(*leaves[index - 1])[page & (GpuLeafEntries - 1)];
}

void GpuPageTable::markLeaf(std::uint32_t index) {
    if (leafDirty[index]) return;
    leafDirty[index] = true;
    dirty.leaves.push_back(index);
}

bool GpuPageTable::Map(std::uint64_t address, std::uint64_t size, std::uint64_t deviceAddress, std::uint64_t permissions) {
    if (size == 0 || address >= GpuAddressSpace || size > GpuAddressSpace - address) return false;
    if ((permissions & ~GpuPagePermissions) != 0 || permissions == 0) return false;
    const auto offset = address & (GpuPageBytes - 1);
    if (deviceAddress < offset || ((deviceAddress - offset) & GpuPagePermissions) != 0) return false;
    const auto base = deviceAddress - offset;
    const auto first = address >> GpuPageBits;
    const auto last = (address + size - 1) >> GpuPageBits;
    for (auto page = first; page <= last; ++page) {
        auto* slot = entry(page, true);
        *slot = (base + ((page - first) << GpuPageBits)) | permissions;
        markLeaf(root[static_cast<std::uint32_t>(page >> GpuLeafBits)] - 1);
    }
    return true;
}

void GpuPageTable::Unmap(std::uint64_t address, std::uint64_t size) {
    if (size == 0 || address >= GpuAddressSpace) return;
    const auto end = size > GpuAddressSpace - address ? GpuAddressSpace : address + size;
    for (auto page = address >> GpuPageBits; page <= (end - 1) >> GpuPageBits; ++page) {
        auto* slot = entry(page, false);
        if (slot == nullptr) {
            page |= GpuLeafEntries - 1;
            continue;
        }
        if (*slot == 0) continue;
        *slot = 0;
        markLeaf(root[static_cast<std::uint32_t>(page >> GpuLeafBits)] - 1);
    }
}

std::uint64_t GpuPageTable::Lookup(std::uint64_t address, bool write) const {
    if (address >= GpuAddressSpace) return 0;
    const auto page = address >> GpuPageBits;
    const auto index = root[static_cast<std::uint32_t>(page >> GpuLeafBits)];
    if (index == 0) return 0;
    const auto value = (*leaves[index - 1])[page & (GpuLeafEntries - 1)];
    if ((value & (write ? GpuPageWrite : GpuPageRead)) == 0) return 0;
    return (value & ~GpuPagePermissions) + (address & (GpuPageBytes - 1));
}

std::span<const std::uint64_t> GpuPageTable::Leaf(std::uint32_t index) const {
    if (index >= leaves.size()) return {};
    return *leaves[index];
}

GpuPageTableDirty GpuPageTable::TakeDirty() {
    for (const auto index : dirty.leaves) leafDirty[index] = false;
    return std::exchange(dirty, {});
}

}
