#include "prx/libSceAgcDriver/Unified/include/GpuPageTable.hpp"
#include <cstdio>
#include <vector>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

constexpr std::uint64_t Guest = 0x3e0400000ull;
constexpr std::uint64_t Device = 0x7f0000000000ull;

void testMappedPagesTranslate() {
    GpuPageTable table;
    Expect(table.Map(Guest, 3 * GpuPageBytes, Device, GpuPageRead | GpuPageWrite), "a chunk-aligned span maps");
    Expect(table.Lookup(Guest) == Device, "the first byte translates to the device base");
    Expect(table.Lookup(Guest + 2 * GpuPageBytes + 0x123, true) == Device + 2 * GpuPageBytes + 0x123, "a later byte keeps its offset");
    Expect(table.Lookup(Guest + 3 * GpuPageBytes) == 0, "the page after the span is unmapped");
    Expect(table.Lookup(Guest - 1) == 0, "the page before the span is unmapped");
    Expect(table.Leaves() == 1 && table.Root()[static_cast<std::uint32_t>(Guest >> (GpuPageBits + GpuLeafBits))] == 1, "one leaf holds the span and the root points at it");
}

void testUnalignedSpanKeepsOffsets() {
    GpuPageTable table;
    Expect(table.Map(Guest + 0x1000, GpuPageBytes, Device + 0x1000, GpuPageRead), "an unaligned span maps whole pages");
    Expect(table.Lookup(Guest + 0x1000) == Device + 0x1000, "the span start translates");
    Expect(table.Lookup(Guest + GpuPageBytes + 0x800) == Device + GpuPageBytes + 0x800, "the second touched page translates linearly");
    Expect(!table.Map(Guest, GpuPageBytes, Device + 1, GpuPageRead), "a device page base must leave the permission bits free");
}

void testPermissions() {
    GpuPageTable table;
    static_cast<void>(table.Map(Guest, GpuPageBytes, Device, GpuPageRead));
    Expect(table.Lookup(Guest) == Device, "a read-only page reads");
    Expect(table.Lookup(Guest, true) == 0, "a read-only page refuses writes");
    Expect(!table.Map(Guest, GpuPageBytes, Device, 0), "a page needs a permission");
}

void testUnmapAndDirtyLeaves() {
    GpuPageTable table;
    static_cast<void>(table.Map(Guest, GpuPageBytes, Device, GpuPageRead));
    static_cast<void>(table.Map(Guest + (std::uint64_t{1} << 32), GpuPageBytes, Device + GpuPageBytes, GpuPageRead));
    auto dirty = table.TakeDirty();
    Expect(dirty.root && dirty.leaves == std::vector<std::uint32_t>{0, 1}, "new leaves and the root are dirty");
    Expect(table.TakeDirty().leaves.empty() && !table.TakeDirty().root, "taking the dirty parts clears them");
    table.Unmap(Guest, GpuPageBytes);
    Expect(table.Lookup(Guest) == 0, "an unmapped page no longer translates");
    dirty = table.TakeDirty();
    Expect(!dirty.root && dirty.leaves == std::vector<std::uint32_t>{0}, "unmapping dirties only its leaf");
    table.Unmap(Guest + (std::uint64_t{2} << 32), std::uint64_t{1} << 30);
    Expect(table.TakeDirty().leaves.empty(), "unmapping where no leaf exists changes nothing");
}

void testAddressSpaceBounds() {
    GpuPageTable table;
    Expect(!table.Map(GpuAddressSpace, GpuPageBytes, Device, GpuPageRead), "addresses past the table are refused");
    Expect(!table.Map(GpuAddressSpace - GpuPageBytes, 2 * GpuPageBytes, Device, GpuPageRead), "spans running past the table are refused");
    Expect(table.Lookup(GpuAddressSpace + 5) == 0, "lookups past the table miss");
}

}

int main() {
    testMappedPagesTranslate();
    testUnalignedSpanKeepsOffsets();
    testPermissions();
    testUnmapAndDirtyLeaves();
    testAddressSpaceBounds();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified GPU page table tests passed\n");
    return 0;
}
