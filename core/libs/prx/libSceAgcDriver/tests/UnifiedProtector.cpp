#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
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

#ifdef _WIN32
constexpr std::uint64_t Pages = 16;

std::uint64_t reserve() {
    return reinterpret_cast<std::uint64_t>(VirtualAlloc(nullptr, Pages * PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

void release(std::uint64_t base) {
    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
}

DWORD protectionAt(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));
    return info.Protect;
}

void testApplySkipsNeverArmedPages() {
    const auto base = reserve();
    Expect(base != 0, "host pages are reserved");
    if (base == 0) return;
    Protector protector;
    protector.NeverArm(base + 2 * PageBytes, PageBytes);
    protector.NeverArm(base + 3 * PageBytes + 0x10, 0x20);
    Expect(protector.NeverArmed(base + 3 * PageBytes) && !protector.NeverArmed(base + 4 * PageBytes), "adjacent never-arm ranges merge to whole pages");
    const std::vector<ProtectionChange> changes{{base, 6 * PageBytes, Protection::ReadOnly}};
    Expect(protector.Apply(changes) == 2, "one armed run before and one after the excluded pages");
    Expect(protectionAt(base) == PAGE_READONLY && protectionAt(base + PageBytes) == PAGE_READONLY, "pages before the exclusion are read-only");
    Expect(protectionAt(base + 2 * PageBytes) == PAGE_READWRITE && protectionAt(base + 3 * PageBytes) == PAGE_READWRITE, "excluded pages stay open");
    Expect(protectionAt(base + 5 * PageBytes) == PAGE_READONLY && protectionAt(base + 6 * PageBytes) == PAGE_READWRITE, "the run after ends where the change ends");
    Expect(protector.Counts().neverArmed == 2 * PageBytes, "two excluded pages counted");
    const std::vector<ProtectionChange> open{{base, 6 * PageBytes, Protection::ReadWrite}};
    Expect(protector.Apply(open) == 1 && protectionAt(base) == PAGE_READWRITE, "opening ignores exclusions and takes one call");
    protector.AllowArm(base + 2 * PageBytes, PageBytes);
    Expect(!protector.NeverArmed(base + 2 * PageBytes) && protector.NeverArmed(base + 3 * PageBytes), "allowing one page keeps the rest excluded");
    release(base);
}

void testDeferredChangesFlushAsRuns() {
    const auto base = reserve();
    if (base == 0) return;
    Protector protector;
    for (std::uint64_t page = 0; page < 4; ++page) {
        const ProtectionChange change{base + page * PageBytes, PageBytes, Protection::NoAccess};
        protector.Defer(std::span(&change, 1));
    }
    Expect(protectionAt(base) == PAGE_READWRITE, "deferred changes are not applied yet");
    Expect(protector.Flush() == 1, "four adjacent deferred pages flush in one call");
    Expect(protectionAt(base) == PAGE_NOACCESS && protectionAt(base + 3 * PageBytes) == PAGE_NOACCESS && protectionAt(base + 4 * PageBytes) == PAGE_READWRITE, "the flushed run covers exactly the deferred pages");
    Expect(protector.Flush() == 0, "a second flush has nothing to do");
    Expect(protector.Counts().failures == 0, "no protection call failed");
    release(base);
}

std::atomic<int> routed{0};

LONG WINAPI routeToGuestArena(EXCEPTION_POINTERS* info) {
    if (!GuestArena::GuestArenaHandleGpuFault_nid_no_patch(info)) return EXCEPTION_CONTINUE_SEARCH;
    routed.fetch_add(1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void testWriteFaultOpensACleanPage() {
    const auto base = reserve();
    if (base == 0) return;
    auto& core = MemoryCore::Get();
    core.Register(base, 4 * PageBytes);
    const auto first = core.ConsumeDirty();
    Expect(first.size() == 1 && first[0].size == 4 * PageBytes, "the registered range uploads once");
    Expect(protectionAt(base + PageBytes) == PAGE_READONLY, "consumed pages are armed");
    core.Install();
    void* handler = AddVectoredExceptionHandler(1, routeToGuestArena);
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(base + PageBytes + 8);
    *bytes = 0x5a;
    RemoveVectoredExceptionHandler(handler);
    core.Uninstall();
    Expect(*bytes == 0x5a, "the faulting write completes after the page opens");
    Expect(routed.load() >= 1 && core.Counts().writesOpened == 1, "the write fault goes through the guest arena to the memory core");
    Expect(protectionAt(base + PageBytes) == PAGE_READWRITE && protectionAt(base) == PAGE_READONLY, "only the written page opens");
    Expect(core.Pages().StateOf(base + PageBytes) == PageState::CpuDirty, "the written page is dirty");
    const auto second = core.ConsumeDirty();
    Expect(second.size() == 1 && second[0].address == base + PageBytes && second[0].size == PageBytes, "the next consume uploads only the written page");
    core.Unregister(base, 4 * PageBytes);
    Expect(protectionAt(base) == PAGE_READWRITE, "unregistering opens the pages");
    release(base);
}
#endif

}

int main() {
#ifdef _WIN32
    testApplySkipsNeverArmedPages();
    testDeferredChangesFlushAsRuns();
    testWriteFaultOpensACleanPage();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified protector tests passed\n");
    return 0;
}
