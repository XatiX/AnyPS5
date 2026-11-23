#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <thread>
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

void testCleanInNeedsEveryPageArmed() {
    PageTable table;
    static_cast<void>(table.Register(Base, 4 * PageBytes));
    Expect(!table.CleanIn(Base, 4 * PageBytes), "registered pages are not clean before their first consume");
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    Expect(table.CleanIn(Base + 0x10, 4 * PageBytes - 0x20), "consumed pages are clean");
    Expect(!table.CleanIn(Base, 5 * PageBytes), "an untracked page is not clean");
    Expect(table.HandleWrite(Base + 3 * PageBytes, changes), "a write to a clean page is handled");
    Expect(!table.CleanIn(Base, 4 * PageBytes) && table.CleanIn(Base, 3 * PageBytes), "a written page makes only its range unclean");
    static_cast<void>(table.MarkGpuOwned(Base, PageBytes));
    Expect(!table.CleanIn(Base, PageBytes), "a GPU-owned page is not clean");
    Expect(!table.CleanIn(Base + PageBytes, 0), "an empty range is not clean");
    Expect(table.CleanIn(Base + PageBytes, PageBytes) && !table.GpuWrittenIn(Base + PageBytes, PageBytes), "an armed page is clean before the GPU writes it");
    table.NoteGpuWritten(Base + PageBytes + 0x20, 0x10);
    Expect(table.GpuWrittenIn(Base, 3 * PageBytes) && !table.CleanIn(Base + PageBytes, PageBytes), "a GPU write makes its page unclean");
    Expect(!table.GpuWrittenIn(Base + 2 * PageBytes, PageBytes) && table.Counts().gpuWrittenPages == 1, "only the written page is marked");
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

void testReservedPagesStayOpenUntilResident() {
    PageTable table;
    table.Reserve(Base, 4 * PageBytes);
    std::vector<ProtectionChange> changes;
    Expect(table.StateOf(Base) == PageState::Reserved && table.DirtyPages() == 0, "reserved pages are owned but not listed");
    Expect(table.ConsumeDirty(changes).empty() && changes.empty(), "a reserved range uploads and protects nothing");
    Expect(!table.HandleWrite(Base, changes) && changes.empty(), "a write to a reserved page is not the table's fault");
    Expect(table.MarkGpuOwned(Base, PageBytes).empty() && table.StateOf(Base) == PageState::Reserved, "a reserved page does not become GPU-owned");
    static_cast<void>(table.MakeResident(Base + PageBytes, 2 * PageBytes));
    Expect(table.StateOf(Base) == PageState::Reserved && table.StateOf(Base + PageBytes) == PageState::CpuDirty && table.DirtyPages() == 2, "only the pages made resident are listed");
    const auto ranges = table.ConsumeDirty(changes);
    Expect(ranges.size() == 1 && ranges[0] == DirtyRange{Base + PageBytes, 2 * PageBytes}, "resident pages upload once");
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base + PageBytes, 2 * PageBytes, Protection::ReadOnly}, "resident pages are armed, reserved ones are not");
    changes = table.Unregister(Base, 4 * PageBytes);
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{Base + PageBytes, 2 * PageBytes, Protection::ReadWrite} && table.StateOf(Base) == PageState::Untracked, "unregistering opens the armed pages and forgets the reserved ones");
}

void testOwnedByNeedsEveryPage() {
    PageTable table;
    table.Reserve(Base, 2 * PageBytes);
    static_cast<void>(table.Register(Base + 2 * PageBytes, PageBytes));
    Expect(table.OwnedBy(Base, 3 * PageBytes), "reserved and resident pages are both owned");
    Expect(!table.OwnedBy(Base, 4 * PageBytes), "a range reaching an unregistered page is not owned");
    Expect(!table.OwnedBy(Base, 0), "an empty range is not owned");
}

void testBusyPagesListed() {
    PageTable table;
    static_cast<void>(table.Register(Base, 2 * PageBytes));
    std::vector<ProtectionChange> changes;
    for (int round = 0; round < 3; ++round) {
        static_cast<void>(table.HandleWrite(Base, changes));
        static_cast<void>(table.ConsumeDirty(changes));
        changes.clear();
    }
    const auto busy = table.BusyPages();
    Expect(busy.size() == 1 && busy[0] == Base / PageBytes, "the page dirty in three consumes is the one busy page listed");
}

void collectInto(void* context, std::uintptr_t begin, std::uintptr_t end) {
    static_cast<std::vector<DirtyRange>*>(context)->push_back({begin, end - begin});
}

std::vector<DirtyRange> collected(PageTable& table, std::uint64_t address, std::uint64_t size) {
    std::vector<DirtyRange> runs;
    table.Collect(address, size, collectInto, &runs);
    return runs;
}

void testCollectReportsWritesOnce() {
    PageTable table;
    table.Reserve(Base, 8 * PageBytes);
    Expect(!table.Covers(Base, PageBytes), "reserved pages are not covered");
    table.MakeResident(Base, 4 * PageBytes);
    Expect(table.Covers(Base, 4 * PageBytes) && !table.Covers(Base, 5 * PageBytes), "resident pages are covered, reserved ones are not");
    Expect(collected(table, Base, 4 * PageBytes) == std::vector<DirtyRange>{{Base, 4 * PageBytes}}, "dirty pages are reported");
    std::vector<ProtectionChange> changes;
    table.ConsumeDirty(changes);
    Expect(collected(table, Base, 4 * PageBytes) == std::vector<DirtyRange>{{Base, 4 * PageBytes}}, "pages armed since the last collect are reported once more");
    Expect(collected(table, Base, 4 * PageBytes).empty(), "clean pages are not reported again");
    changes.clear();
    table.HandleWrite(Base + 2 * PageBytes, changes);
    Expect(collected(table, Base, 4 * PageBytes) == std::vector<DirtyRange>{{Base + 2 * PageBytes, PageBytes}}, "a written page is reported");
    Expect(collected(table, Base, 4 * PageBytes) == std::vector<DirtyRange>{{Base + 2 * PageBytes, PageBytes}}, "a page still open is reported at every collect");
    Expect(collected(table, Base + 3 * PageBytes, PageBytes).empty(), "a collect reports only its range");
    table.ConsumeDirty(changes);
    static_cast<void>(collected(table, Base, 4 * PageBytes));
    static_cast<void>(table.MarkGpuOwned(Base, PageBytes));
    Expect(collected(table, Base, 4 * PageBytes).empty(), "GPU-owned pages are not reported");
    static_cast<void>(table.Reclaim(Base, PageBytes, false));
    Expect(collected(table, Base, 4 * PageBytes) == std::vector<DirtyRange>{{Base, PageBytes}}, "a page read back from the GPU is reported once");
    Expect(collected(table, Base, 4 * PageBytes).empty(), "a read-back page is not reported again");
}

void testCollectChecksOpenPages() {
    PageTable table;
    table.Reserve(Base, 2 * PageBytes);
    table.MakeResident(Base, 2 * PageBytes);
    std::vector<std::uint64_t> contents(2, 0);
    std::vector<std::uint64_t> seen(2, ~0ull);
    std::vector<bool> provenCalls;
    const PageTable::WriteCheck changed = [&](std::uint64_t page, bool proven) {
        provenCalls.push_back(proven);
        const auto index = page - Base / PageBytes;
        const bool differs = proven || seen[index] != contents[index];
        seen[index] = contents[index];
        return differs;
    };
    const auto collect = [&] {
        std::vector<DirtyRange> runs;
        table.Collect(Base, 2 * PageBytes, collectInto, &runs, changed);
        return runs;
    };
    Expect(collect() == std::vector<DirtyRange>{{Base, 2 * PageBytes}} && provenCalls == std::vector<bool>{false, false}, "pages made resident are content-checked, not proven written");
    Expect(collect().empty(), "open pages whose content is unchanged are not reported");
    contents[1] = 7;
    Expect(collect() == std::vector<DirtyRange>{{Base + PageBytes, PageBytes}}, "an open page is reported when its content changed");
    Expect(collect().empty(), "an open page is reported once per change");
    std::vector<ProtectionChange> changes;
    table.ConsumeDirty(changes);
    provenCalls.clear();
    Expect(collect().empty() && provenCalls == std::vector<bool>{false, false}, "pages armed after an upload are checked, not reported unseen");
    table.HandleWrite(Base, changes);
    provenCalls.clear();
    Expect(collect() == std::vector<DirtyRange>{{Base, PageBytes}} && provenCalls == std::vector<bool>{true}, "a write fault is reported without a content check");
}

void testDeviceWrittenPagesReportOnlyCpuWrites() {
    PageTable table;
    table.Reserve(Base, 2 * PageBytes);
    table.MakeResident(Base, 2 * PageBytes);
    std::vector<std::uint64_t> contents(2, 0);
    std::vector<std::uint64_t> seen(2, 0);
    const PageTable::WriteCheck changed = [&](std::uint64_t page, bool proven) {
        const auto index = page - Base / PageBytes;
        const bool differs = proven || seen[index] != contents[index];
        seen[index] = contents[index];
        return differs;
    };
    const auto collect = [&] {
        std::vector<DirtyRange> runs;
        table.Collect(Base, 2 * PageBytes, collectInto, &runs, changed);
        return runs;
    };
    static_cast<void>(collect());
    const auto armed = table.MarkDeviceWritten(Base, 2 * PageBytes);
    Expect(armed == std::vector<ProtectionChange>{{Base, 2 * PageBytes, Protection::ReadOnly}} && table.StateOf(Base) == PageState::CpuClean, "open pages written by the device are armed for CPU writes");
    contents[0] = 5;
    contents[1] = 9;
    Expect(collect().empty(), "device writes are not reported as CPU writes");
    std::vector<ProtectionChange> changes;
    table.HandleWrite(Base + PageBytes, changes);
    contents[1] = 11;
    Expect(collect() == std::vector<DirtyRange>{{Base + PageBytes, PageBytes}}, "a CPU write after the device write is reported");
    contents[1] = 12;
    Expect(collect() == std::vector<DirtyRange>{{Base + PageBytes, PageBytes}}, "a page reopened by the CPU is content-checked again");
}

void armed(PageTable& table, std::uint64_t pages) {
    static_cast<void>(table.Register(Base, pages * PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
}

void testLockFreeWriteFaultIsReportedAtTheNextConsume() {
    PageTable table;
    armed(table, 4);
    const auto page = Base + PageBytes;
    Expect(table.BeginWriteFault(page + 12), "a write fault on a clean page opens it without the table's lock");
    Expect(table.StateOf(page) == PageState::CpuDirty, "the page is dirty at once");
    table.EndWriteFault(page + 12);
    Expect(!table.BeginWriteFault(page), "a second fault on the open page is not a new opening");
    Expect(!table.BeginWriteFault(Base + 64 * PageBytes), "a fault on a page the table does not follow is refused");
    Expect(table.DirtyPages() == 1 && table.RegionDirty(Base) && table.Counts().writeFaults == 1, "the opening is counted and listed once a lock holder looks");
    std::vector<ProtectionChange> changes;
    const auto ranges = table.ConsumeDirty(changes);
    Expect(ranges.size() == 1 && ranges[0] == DirtyRange{page, PageBytes}, "the next consume reports the page");
    Expect(changes.size() == 1 && changes[0] == ProtectionChange{page, PageBytes, Protection::ReadOnly}, "and arms it again");
    Expect(table.StateOf(page) == PageState::CpuClean && table.DirtyPages() == 0, "the page is clean again");
}

void testGpuTakeSeesAnUndrainedFaultAsDirty() {
    PageTable table;
    armed(table, 4);
    Expect(table.BeginWriteFault(Base + PageBytes), "a clean page opens");
    table.EndWriteFault(Base + PageBytes);
    const auto take = table.TakeForGpu(Base, 4 * PageBytes);
    Expect(take.dirty.size() == 1 && take.dirty[0] == DirtyRange{Base + PageBytes, PageBytes}, "a page opened since the last consume is taken as dirty");
    Expect(take.owned.size() == 1 && take.owned[0] == DirtyRange{Base, 4 * PageBytes} && table.StateOf(Base + PageBytes) == PageState::GpuOwned, "every page of the range is GPU-owned");
    Expect(table.DirtyPages() == 0, "no taken page stays listed");
    PageTable kept;
    armed(kept, 4);
    Expect(kept.BeginWriteFault(Base + PageBytes), "a clean page opens");
    kept.EndWriteFault(Base + PageBytes);
    const auto clean = kept.TakeForGpu(Base, 4 * PageBytes, false);
    Expect(clean.dirty.empty() && kept.StateOf(Base + PageBytes) == PageState::CpuDirty && kept.StateOf(Base) == PageState::GpuOwned, "a take of clean pages only leaves the opened page with the CPU");
    Expect(kept.DirtyPages() == 1, "and it stays listed");
}

void testLockHolderWaitsForAnOpeningPage() {
    PageTable table;
    armed(table, 2);
    Expect(table.BeginWriteFault(Base), "a clean page starts opening");
    std::atomic<bool> marked{false};
    std::thread holder([&] {
        static_cast<void>(table.MarkGpuOwned(Base, PageBytes));
        marked.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!marked.load(std::memory_order_acquire), "a lock holder does not change a page whose protection is still being opened");
    table.EndWriteFault(Base);
    holder.join();
    Expect(marked.load() && table.StateOf(Base) == PageState::GpuOwned && table.DirtyPages() == 0, "it takes the page once the opening is finished");
    Expect(table.Counts().writeFaults == 1 && table.Counts().gpuOwnedPages == 1, "both changes are counted");
}

void testWritersAgainstAConsumer() {
    constexpr std::uint64_t Pages = 512;
    constexpr int Writers = 4;
    PageTable table;
    armed(table, Pages);
    std::vector<std::atomic<int>> pending(Pages);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> opened{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < Writers; ++writer) {
        writers.emplace_back([&, writer] {
            std::uint64_t seed = 0x9e3779b97f4a7c15ull * static_cast<std::uint64_t>(writer + 1);
            for (int turn = 0; turn < 20000; ++turn) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const auto index = (seed >> 33) % Pages;
                pending[index].store(1, std::memory_order_release);
                if (table.BeginWriteFault(Base + index * PageBytes)) {
                    opened.fetch_add(1, std::memory_order_relaxed);
                    table.EndWriteFault(Base + index * PageBytes);
                }
            }
        });
    }
    const auto consume = [&] {
        std::vector<ProtectionChange> changes;
        for (const auto& range : table.ConsumeDirty(changes)) {
            for (auto at = range.address; at < range.address + range.size; at += PageBytes) pending[(at - Base) / PageBytes].store(0, std::memory_order_release);
        }
    };
    std::thread consumer([&] {
        while (!stop.load(std::memory_order_acquire)) consume();
    });
    for (auto& writer : writers) writer.join();
    stop.store(true, std::memory_order_release);
    consumer.join();
    for (int turn = 0; turn < 400 && table.DirtyPages() != 0; ++turn) consume();
    bool reported = true;
    bool clean = true;
    for (std::uint64_t index = 0; index < Pages; ++index) {
        reported = reported && pending[index].load() == 0;
        clean = clean && table.StateOf(Base + index * PageBytes) == PageState::CpuClean;
    }
    Expect(reported, "every page a writer opened was reported by a consume after it");
    Expect(clean && table.DirtyPages() == 0, "every page ends clean and unlisted");
    Expect(table.Counts().writeFaults == opened.load(), "every opening is counted exactly once");
}

std::uint64_t& unreadablePage() {
    static std::uint64_t page = 0;
    return page;
}

std::map<std::uint64_t, std::uint64_t>& fakeHashes() {
    static std::map<std::uint64_t, std::uint64_t> hashes;
    return hashes;
}

bool fakeHash(std::uint64_t page, std::uint64_t& hash) {
    if (page == unreadablePage()) return false;
    hash = fakeHashes()[page];
    return true;
}

std::uint64_t pageOf(std::uint64_t index) {
    return Base / PageBytes + index;
}

void testAdjacentFaultOpensItsChunk() {
    PageTable table;
    table.SetHasher(fakeHash);
    armed(table, 32);
    Expect(table.BeginWriteFault(Base + 3 * PageBytes) && table.GrowWriteFault(Base + 3 * PageBytes).empty(), "a lone fault opens only its page");
    table.EndWriteFault(Base + 3 * PageBytes);
    Expect(table.BeginWriteFault(Base + 4 * PageBytes), "the neighbouring page opens");
    const auto runs = table.GrowWriteFault(Base + 4 * PageBytes);
    table.EndWriteFault(Base + 4 * PageBytes);
    table.EndGrow(runs);
    Expect(runs.size() == 2 && runs[0] == DirtyRange{Base, 3 * PageBytes} && runs[1] == DirtyRange{Base + 5 * PageBytes, 11 * PageBytes}, "a fault next to an open page opens the rest of its 64 KiB chunk");
    Expect(table.StateOf(Base + 15 * PageBytes) == PageState::CpuDirty && table.StateOf(Base + 16 * PageBytes) == PageState::CpuClean, "the next chunk stays armed");
    fakeHashes()[pageOf(7)] = 0x77;
    std::vector<ProtectionChange> changes;
    const auto ranges = table.ConsumeDirty(changes);
    const auto rearmed = table.TakeRearmedUnproven();
    Expect(ranges.size() == 1 && ranges[0] == DirtyRange{Base + 3 * PageBytes, 5 * PageBytes}, "the faulted pages and the changed unproven page are reported (merged across the short gap), the unchanged ones are not");
    Expect(rearmed.size() == 13 && table.Counts().unprovenUnchanged == 13 && table.Counts().unprovenChanged == 1, "thirteen unproven pages were unchanged and are armed again");
    Expect(table.StateOf(Base) == PageState::CpuClean && table.DirtyPages() == 0, "every page is clean again");
    fakeHashes().clear();
}

void testImageSourceOpensWhole() {
    PageTable table;
    table.SetHasher(fakeHash);
    armed(table, 64);
    table.OpenWhole(Base + 8 * PageBytes, 40 * PageBytes);
    Expect(table.BeginWriteFault(Base + 20 * PageBytes), "a page of the source opens");
    const auto runs = table.GrowWriteFault(Base + 20 * PageBytes);
    table.EndWriteFault(Base + 20 * PageBytes);
    table.EndGrow(runs);
    Expect(runs.size() == 2 && runs[0] == DirtyRange{Base + 8 * PageBytes, 12 * PageBytes} && runs[1] == DirtyRange{Base + 21 * PageBytes, 27 * PageBytes}, "its first write opens the whole source range");
    Expect(table.StateOf(Base + 7 * PageBytes) == PageState::CpuClean && table.StateOf(Base + 48 * PageBytes) == PageState::CpuClean, "pages outside it stay armed");
    Expect(table.Counts().wholeFaults == 1, "the fault is counted as a whole opening");
}

void testUnreadablePagesDoNotGrow() {
    PageTable table;
    table.SetHasher(fakeHash);
    armed(table, 16);
    unreadablePage() = pageOf(9);
    Expect(table.BeginWriteFault(Base + 2 * PageBytes), "a page opens");
    table.EndWriteFault(Base + 2 * PageBytes);
    Expect(table.BeginWriteFault(Base + 3 * PageBytes), "its neighbour opens");
    const auto runs = table.GrowWriteFault(Base + 3 * PageBytes);
    table.EndWriteFault(Base + 3 * PageBytes);
    table.EndGrow(runs);
    Expect(table.StateOf(Base + 9 * PageBytes) == PageState::CpuClean && table.StateOf(Base + 8 * PageBytes) == PageState::CpuDirty, "a page that cannot be read is not opened without its own fault");
    unreadablePage() = 0;
}

int main() {
    testAdjacentFaultOpensItsChunk();
    testImageSourceOpensWhole();
    testUnreadablePagesDoNotGrow();
    testLockFreeWriteFaultIsReportedAtTheNextConsume();
    testGpuTakeSeesAnUndrainedFaultAsDirty();
    testLockHolderWaitsForAnOpeningPage();
    testWritersAgainstAConsumer();
    testDeviceWrittenPagesReportOnlyCpuWrites();
    testCollectReportsWritesOnce();
    testCollectChecksOpenPages();
    testReservedPagesStayOpenUntilResident();
    testOwnedByNeedsEveryPage();
    testBusyPagesListed();
    testFramePacedPagesStayOpen();
    testUnchangedBusyPagesAreNotReported();
    testRegisteredPagesUploadOnceThenArm();
    testWriteFaultOpensOnePage();
    testGapsMergeOnlyOverCpuPages();
    testBusyPagesStayOpen();
    testGpuOwnedPagesReclaim();
    testCleanInNeedsEveryPageArmed();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified page table tests passed\n");
    return 0;
}
