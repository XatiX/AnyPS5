#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "prx/libc/include/GuestArena.hpp"
#endif

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testTakeForGpuSplitsWholePagesFromEdges() {
    PageTable table;
    const std::uint64_t base = 0x40000000;
    static_cast<void>(table.Register(base, 8 * PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    static_cast<void>(table.HandleWrite(base + 3 * PageBytes, changes));
    const auto take = table.TakeForGpu(base + 0x800, 5 * PageBytes);
    Expect(take.owned.size() == 1 && take.owned[0].address == base + PageBytes && take.owned[0].size == 4 * PageBytes, "the whole pages inside a write become GPU-owned");
    Expect(take.open.size() == 2 && take.open[0].address == base + 0x800 && take.open[0].size == 0x800 && take.open[1].address == base + 5 * PageBytes && take.open[1].size == 0x800, "the partial head and tail stay open as byte ranges");
    Expect(take.dirty.size() == 1 && take.dirty[0].address == base + 3 * PageBytes && take.dirty[0].size == PageBytes, "a page the CPU had written is reported for upload");
    Expect(table.StateOf(base + PageBytes) == PageState::GpuOwned && table.StateOf(base) == PageState::CpuClean && table.StateOf(base + 5 * PageBytes) == PageState::CpuClean, "only the whole pages change state");
    Expect(take.changes.size() == 1 && take.changes[0].protection == Protection::NoAccess && take.changes[0].size == 4 * PageBytes, "the owned pages are closed in one change");
    const auto small = table.TakeForGpu(base + 6 * PageBytes + 0x10, 0x20);
    Expect(small.owned.empty() && small.open.size() == 1 && small.open[0].size == 0x20, "a write inside one page takes no page");
    const auto unknown = table.TakeForGpu(base + 64 * PageBytes, 2 * PageBytes);
    Expect(unknown.owned.empty() && unknown.open.size() == 1 && unknown.open[0].size == 2 * PageBytes, "untracked pages are left open");
}

void testTakeForGpuAfterCompletionLeavesDirtyPagesOpen() {
    PageTable table;
    const std::uint64_t base = 0x50000000;
    static_cast<void>(table.Register(base, 4 * PageBytes));
    std::vector<ProtectionChange> changes;
    static_cast<void>(table.ConsumeDirty(changes));
    static_cast<void>(table.HandleWrite(base + PageBytes, changes));
    const auto take = table.TakeForGpu(base, 3 * PageBytes, false);
    Expect(take.dirty.empty() && take.owned.size() == 2 && take.owned[0].address == base && take.owned[1].address == base + 2 * PageBytes, "clean pages are owned around a dirty one");
    Expect(take.open.size() == 1 && take.open[0].address == base + PageBytes && take.open[0].size == PageBytes, "a page the CPU wrote after the batch stays open for a byte-wise landing");
    Expect(table.StateOf(base + PageBytes) == PageState::CpuDirty, "the dirty page keeps its state");
}

#ifdef _WIN32
constexpr std::uint64_t Pages = 16;

std::uint64_t reserve() {
    return reinterpret_cast<std::uint64_t>(VirtualAlloc(nullptr, Pages * PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

DWORD protectionAt(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));
    return info.Protect;
}

class FakeHost final : public MemoryHost {
public:
    std::uint64_t base = 0;
    std::vector<std::byte> mirror = std::vector<std::byte>(Pages * PageBytes);
    std::vector<std::byte> shadow = std::vector<std::byte>(Pages * PageBytes);
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> uploads{0};
    std::atomic<bool> bypass{false};

    std::uint64_t Completed() override { return completed.load(); }
    bool WaitFor(std::uint64_t serial) override {
        waits.fetch_add(1);
        auto seen = completed.load();
        while (seen < serial && !completed.compare_exchange_weak(seen, serial)) {
        }
        return true;
    }
    bool ReadMirror(std::uint64_t address, std::span<std::byte> out) override {
        std::memcpy(out.data(), mirror.data() + (address - base), out.size());
        return true;
    }
    std::byte* View(std::uint64_t address, std::size_t) override { return shadow.data() + (address - base); }
    void Upload(std::uint64_t, std::uint64_t) override { uploads.fetch_add(1); }
    bool Bypass() override { return bypass.load(); }
};

LONG WINAPI routeToGuestArena(EXCEPTION_POINTERS* info) {
    if (!GuestArena::GuestArenaHandleGpuFault_nid_no_patch(info)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void testOwnedPagesLandOnFault() {
    const auto base = reserve();
    Expect(base != 0, "host pages are reserved");
    if (base == 0) return;
    auto& core = MemoryCore::Get();
    FakeHost fake;
    fake.base = base;
    core.Register(base, Pages * PageBytes);
    static_cast<void>(core.ConsumeDirty());
    _putenv_s("APS5_PROTECT_TRACKING", "1");
    Expect(core.Install(), "forcing write faults installs the handler");
    core.SetHost(&fake);
    void* handler = AddVectoredExceptionHandler(1, routeToGuestArena);

    const auto mark = core.NoteGpuWrite(base + 2 * PageBytes, 3 * PageBytes, 5);
    Expect(mark.owned.size() == 1 && mark.owned[0].size == 3 * PageBytes && mark.open.empty() && mark.upload.empty(), "a write over whole clean pages owns them");
    Expect(protectionAt(base + 2 * PageBytes) == PAGE_NOACCESS && protectionAt(base + 5 * PageBytes) == PAGE_READONLY, "GPU-owned pages are closed, their neighbours are not");
    std::memset(fake.mirror.data() + 2 * PageBytes, 0x5c, 3 * PageBytes);
    std::uint8_t seen = 0;
    std::thread reader([&] { seen = *reinterpret_cast<volatile std::uint8_t*>(base + 2 * PageBytes + 0x40); });
    reader.join();
    Expect(fake.waits.load() == 1 && fake.completed.load() == 5, "the fault waits for the batch that wrote the page");
    Expect(fake.shadow[2 * PageBytes + 0x40] == std::byte{0x5c} && fake.shadow[4 * PageBytes + 0x40] == std::byte{0x5c}, "the faulting page and the owned pages after it are landed through the view");
    Expect(core.Pages().StateOf(base + 2 * PageBytes) == PageState::CpuClean && protectionAt(base + 2 * PageBytes) == PAGE_READONLY, "a read fault leaves the page clean and readable");
    Expect(!core.Written().Any(base + 2 * PageBytes, 3 * PageBytes), "landed bytes leave the written set");
    Expect(core.Counts().faultsParked == 1 && core.Counts().faultsResolved == 1 && core.Counts().readAheadPages == 2, "one parked fault resolved three pages");
    static_cast<void>(seen);

    static_cast<void>(core.NoteGpuWrite(base + 8 * PageBytes, PageBytes, 6));
    std::memset(fake.mirror.data() + 8 * PageBytes, 0x33, PageBytes);
    std::thread writer([&] { *reinterpret_cast<volatile std::uint8_t*>(base + 8 * PageBytes + 4) = 0x77; });
    writer.join();
    Expect(core.Pages().StateOf(base + 8 * PageBytes) == PageState::CpuDirty && protectionAt(base + 8 * PageBytes) == PAGE_READWRITE, "a write fault lands the page and leaves it dirty");
    Expect(*reinterpret_cast<std::uint8_t*>(base + 8 * PageBytes + 4) == 0x77, "the faulting write completes");

    static_cast<void>(core.NoteGpuWrite(base + 10 * PageBytes, PageBytes, 9));
    std::memset(fake.shadow.data() + 10 * PageBytes, 0x11, PageBytes);
    std::memset(fake.mirror.data() + 10 * PageBytes, 0x11, PageBytes);
    fake.mirror[10 * PageBytes + 8] = std::byte{0x99};
    fake.bypass.store(true);
    const auto before = *reinterpret_cast<volatile std::uint8_t*>(base + 10 * PageBytes);
    static_cast<void>(before);
    Expect(core.Counts().faultsBypassed == 1 && protectionAt(base + 10 * PageBytes) == PAGE_READWRITE && core.Written().Any(base + 10 * PageBytes, PageBytes), "a thread that must not park gets the page opened with its GPU bytes still pending");
    fake.bypass.store(false);
    fake.shadow[10 * PageBytes + 16] = std::byte{0x42};
    fake.completed.store(9);
    core.FlushGpuWrites(base + 10 * PageBytes, PageBytes);
    Expect(fake.shadow[10 * PageBytes + 8] == std::byte{0x99} && fake.shadow[10 * PageBytes + 16] == std::byte{0x42}, "only the bytes the GPU changed are landed over a page that was opened early");
    Expect(!core.Written().Any(base + 10 * PageBytes, PageBytes) && fake.uploads.load() >= 1, "the opened page's pieces are taken and the page is uploaded again");

    const std::vector<std::byte> pre(0x40, std::byte{0x01});
    std::vector<std::byte> post(0x40, std::byte{0x01});
    post[4] = std::byte{0xaa};
    post[5] = std::byte{0xab};
    core.Written().Note(base + 12 * PageBytes + 0x100, 0x40, 11);
    std::memset(fake.shadow.data() + 12 * PageBytes + 0x100, 0x02, 0x40);
    core.LandDiff(base + 12 * PageBytes + 0x100, pre, post, 11);
    Expect(fake.shadow[12 * PageBytes + 0x104] == std::byte{0xaa} && fake.shadow[12 * PageBytes + 0x105] == std::byte{0xab} && fake.shadow[12 * PageBytes + 0x106] == std::byte{0x02}, "a diff lands only the bytes the GPU changed");
    Expect(!core.Written().Any(base + 12 * PageBytes, PageBytes) && core.Counts().diffLandedBytes >= 2, "a landed diff leaves the written set");

    const std::vector<DirtyRange> dirty{{base + 13 * PageBytes, PageBytes}};
    core.Written().Note(base + 13 * PageBytes + 0x80, 0x80, 12);
    const auto kept = core.WithoutGpuWritten(dirty);
    Expect(kept.size() == 2 && kept[0].size == 0x80 && kept[1].address == base + 13 * PageBytes + 0x100, "an upload skips the bytes the GPU wrote and has not landed");

    RemoveVectoredExceptionHandler(handler);
    core.SetHost(nullptr);
    core.Uninstall();
    core.Unregister(base, Pages * PageBytes);
    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
}
#endif

}

int main() {
    testTakeForGpuSplitsWholePagesFromEdges();
    testTakeForGpuAfterCompletionLeavesDirtyPagesOpen();
#ifdef _WIN32
    testOwnedPagesLandOnFault();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified GPU-owned tests passed\n");
    return 0;
}
