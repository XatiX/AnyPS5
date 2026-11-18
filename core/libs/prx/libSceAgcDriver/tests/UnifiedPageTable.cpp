#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <cstdint>
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

constexpr std::uint64_t Base = 0x200000000ull;

void testRegisteredPagesUploadOnceThenArm() {
    PageTable table;
    static_cast<void>(table.Register(Base, 3 * PageBytes));
    Expect(table.StateOf(Base) == PageState::CpuDirty && table.DirtyPages() == 3, "registered pages start dirty and listed");
    Expect(table.RegionDirty(Base), "the region of a dirty page is flagged");
    std::vector<ProtectionChange> changes;
    const auto ranges = table.ConsumeDirty(changes);
    Expect(ranges.size() == 1 && ranges[0] == DirtyRange{Base, 3 * PageBytes}, "adjacent dirty pages upload as one range");
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base, 3 * PageBytes, Protection::ReadOnly}, "consumed pages are armed read-only in one run");
    Expect(table.StateOf(Base + PageBytes) == PageState::CpuClean && table.DirtyPages() == 0 && !table.RegionDirty(Base), "consumed pages are clean and the region flag clears");
    changes.clear();
    Expect(table.ConsumeDirty(changes).empty() && changes.empty(), "nothing is uploaded twice");
}

void testWriteFaultOpensOnePage() {
    PageTable table;
    static_cast<void>(table.Register(Base, 8 * PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    changes.clear();
    Expect(table.HandleWrite(Base + 2 * PageBytes + 0x10, changes), "a write to a clean page is handled");
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base + 2 * PageBytes, PageBytes, Protection::ReadWrite}, "only the written page opens");
    changes.clear();
    Expect(table.HandleWrite(Base + 2 * PageBytes + 0x20, changes) && changes.empty(), "a second write to an open page needs no change");
    Expect(!table.HandleWrite(Base + 64 * PageBytes, changes), "a write outside tracked memory is not handled");
    Expect(table.Counts().writeFaults == 1, "one write fault counted");
}

void testGapsMergeOnlyOverCpuPages() {
    PageTable table;
    static_cast<void>(table.Register(Base, 16 * PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    static_cast<void>(table.MarkGpuOwned(Base + 9 * PageBytes, PageBytes));
    changes.clear();
    for (const std::uint64_t page : {0ull, 3ull, 8ull, 10ull}) static_cast<void>(table.HandleWrite(Base + page * PageBytes, changes));
    changes.clear();
    const auto ranges = table.ConsumeDirty(changes);
    Expect(ranges.size() == 3, "a gap of two clean pages merges, a gap of four does not, a GPU-owned page splits");
    if (ranges.size() == 3) {
        Expect(ranges[0] == DirtyRange{Base, 4 * PageBytes}, "pages 0 and 3 merge over the clean gap");
        Expect(ranges[1] == DirtyRange{Base + 8 * PageBytes, PageBytes}, "page 8 stands alone");
        Expect(ranges[2] == DirtyRange{Base + 10 * PageBytes, PageBytes}, "page 10 is not merged over the GPU-owned page 9");
    }
}

void testBusyPagesStayOpen() {
    PageTable table;
    static_cast<void>(table.Register(Base, PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    changes.clear();
    static_cast<void>(table.HandleWrite(Base, changes));
    changes.clear();
    static_cast<void>(table.ConsumeDirty(changes));
    Expect(table.StateOf(Base) == PageState::CpuClean, "a page dirty in two consumes in a row is armed again");
    changes.clear();
    static_cast<void>(table.HandleWrite(Base, changes));
    changes.clear();
    const auto third = table.ConsumeDirty(changes);
    Expect(third.size() == 1 && changes.empty() && table.StateOf(Base) == PageState::CpuDirty, "a page dirty in three consumes in a row stays open");
    Expect(table.Counts().keptBusy == 1, "one page kept busy");
    for (int round = 0; round < PageTable::BusyConsumes - 1; ++round) {
        changes.clear();
        Expect(table.ConsumeDirty(changes).size() == 1 && changes.empty(), "a busy page uploads at every consume without protection calls");
    }
    changes.clear();
    static_cast<void>(table.ConsumeDirty(changes));
    Expect(changes.size() == 1 && changes[0].protection == Protection::ReadOnly && table.StateOf(Base) == PageState::CpuClean, "a busy page is armed again after its consumes run out");
}

void testGpuOwnedPagesReclaim() {
    PageTable table;
    static_cast<void>(table.Register(Base, 4 * PageBytes));
    auto changes = table.MarkGpuOwned(Base + PageBytes, 2 * PageBytes);
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base + PageBytes, 2 * PageBytes, Protection::NoAccess}, "GPU-written pages lose all access");
    Expect(table.DirtyPages() == 2, "GPU-owned pages leave the dirty list");
    const auto owned = table.GpuOwnedIn(Base, 4 * PageBytes);
    Expect(owned.size() == 1 && owned[0] == DirtyRange{Base + PageBytes, 2 * PageBytes}, "the GPU-owned range is found for read-back");
    std::vector<ProtectionChange> none;
    Expect(!table.HandleWrite(Base + PageBytes, none), "a write fault on a GPU-owned page needs a reclaim");
    changes = table.Reclaim(Base + PageBytes, PageBytes, false);
    Expect(changes.size() == 1 && changes[0].protection == Protection::ReadOnly && table.StateOf(Base + PageBytes) == PageState::CpuClean, "a read reclaim leaves the page clean");
    changes = table.Reclaim(Base + 2 * PageBytes, PageBytes, true);
    Expect(changes.size() == 1 && changes[0].protection == Protection::ReadWrite && table.StateOf(Base + 2 * PageBytes) == PageState::CpuDirty, "a write reclaim opens the page and lists it");
    Expect(table.Counts().gpuOwnedPages == 0, "no page is GPU-owned after both reclaims");
    changes = table.Unregister(Base, 4 * PageBytes);
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base + PageBytes, PageBytes, Protection::ReadWrite}, "unregistering opens only the armed page");
    Expect(table.StateOf(Base) == PageState::Untracked && table.DirtyPages() == 0, "unregistered pages are untracked and unlisted");
}

}

void testFramePacedPagesStayOpen() {
    PageTable table;
    std::vector<ProtectionChange> changes;
    table.Register(Base, PageBytes);
    table.AdvanceFrame();
    table.ConsumeDirty(changes);
    std::uint64_t faults = 0;
    for (int frame = 0; frame < 4; ++frame) {
        table.AdvanceFrame();
        for (int submission = 0; submission < 10; ++submission) {
            changes.clear();
            if (submission == 3 && table.StateOf(Base) == PageState::CpuClean && table.HandleWrite(Base, changes)) ++faults;
            changes.clear();
            table.ConsumeDirty(changes);
        }
    }
    Expect(faults == 2, "a page written once a frame faults in two frames, then counts as busy at its third");
    Expect(table.StateOf(Base) == PageState::CpuDirty && table.Counts().keptBusy == 1, "a page written in three frames in a row stays open across the submissions of later frames");
}

void testUnchangedBusyPagesAreNotReported() {
    PageTable table;
    std::vector<ProtectionChange> changes;
    table.Register(Base, PageBytes);
    table.ConsumeDirty(changes);
    bool changed = true;
    int proven = 0;
    const PageTable::WriteCheck written = [&](std::uint64_t, bool isProven) {
        if (isProven) ++proven;
        return isProven || changed;
    };
    for (int round = 0; round < 3; ++round) {
        changes.clear();
        table.HandleWrite(Base, changes);
        changes.clear();
        table.ConsumeDirty(changes, written);
    }
    Expect(proven == 1 && table.Counts().keptBusy == 1, "the consume that makes a page busy records it as written");
    changed = false;
    changes.clear();
    Expect(table.ConsumeDirty(changes, written).empty(), "an unchanged busy page is not reported");
    Expect(table.StateOf(Base) == PageState::CpuDirty && changes.empty() && table.Counts().unchangedBusy == 1, "an unchanged busy page stays open");
    changed = true;
    Expect(table.ConsumeDirty(changes, written).size() == 1, "a changed busy page is reported");
    changed = false;
    changes.clear();
    table.ConsumeDirty(changes, written);
    Expect(table.StateOf(Base) == PageState::CpuDirty, "a busy page unchanged for one epoch stays open");
    changes.clear();
    table.ConsumeDirty(changes, written);
    Expect(table.StateOf(Base) == PageState::CpuClean && changes.size() == 1, "a busy page unchanged for two epochs is armed again before its busy window ends");
}

int main() {
    testFramePacedPagesStayOpen();
    testUnchangedBusyPagesAreNotReported();
    testRegisteredPagesUploadOnceThenArm();
    testWriteFaultOpensOnePage();
    testGapsMergeOnlyOverCpuPages();
    testBusyPagesStayOpen();
    testGpuOwnedPagesReclaim();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified page table tests passed\n");
    return 0;
}
