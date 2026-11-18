#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#ifdef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#endif

namespace AgcDriver::Unified {

namespace {

bool handleGuestFault(std::uintptr_t address, bool write) {
    return MemoryCore::Get().HandleFault(address, write);
}

void prepareGuestHostWrite(std::uintptr_t address, std::size_t bytes) {
    MemoryCore::Get().PrepareHostWrite(address, bytes);
}

}

MemoryCore& MemoryCore::Get() {
    static MemoryCore& core = *new MemoryCore();
    return core;
}

bool MemoryCore::Install() {
#ifdef _WIN32
    if (!GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(handleGuestFault)) return false;
    GuestArena::GuestArenaSetGpuHostWriteHook_nid_no_patch(prepareGuestHostWrite);
    return true;
#else
    return false;
#endif
}

void MemoryCore::Uninstall() {
#ifdef _WIN32
    GuestArena::GuestArenaSetGpuHostWriteHook_nid_no_patch(nullptr);
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(nullptr);
#endif
}

void MemoryCore::Register(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.Register(address, size));
}

void MemoryCore::Unregister(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.Unregister(address, size));
}

std::vector<DirtyRange> MemoryCore::ConsumeDirty() {
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    auto ranges = pages.ConsumeDirty(changes);
    protector.Apply(changes);
    return ranges;
}

void MemoryCore::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.MarkGpuOwned(address, size));
}

bool MemoryCore::HandleFault(std::uint64_t address, bool write) {
    std::lock_guard lock(mutex);
    const auto state = pages.StateOf(address);
    if (state == PageState::Untracked) {
        foreignFaults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (state == PageState::GpuOwned) {
        gpuOwnedFaults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!write) return false;
    if (state != PageState::CpuClean) {
        staleFaults.fetch_add(1, std::memory_order_relaxed);
        const ProtectionChange reopen{address & ~(PageBytes - 1), PageBytes, Protection::ReadWrite};
        return protector.Apply(std::span(&reopen, 1)) == 1;
    }
    std::vector<ProtectionChange> changes;
    if (!pages.HandleWrite(address, changes)) return false;
    protector.Apply(changes);
    writesOpened.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MemoryCore::PrepareHostWrite(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    const auto end = address + size;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        if (pages.StateOf(page) != PageState::CpuClean) continue;
        if (pages.HandleWrite(page, changes)) hostWritesOpened.fetch_add(1, std::memory_order_relaxed);
    }
    protector.Apply(changes);
}

MemoryCoreCounts MemoryCore::Counts() const {
    return {writesOpened.load(std::memory_order_relaxed), hostWritesOpened.load(std::memory_order_relaxed), gpuOwnedFaults.load(std::memory_order_relaxed), foreignFaults.load(std::memory_order_relaxed), staleFaults.load(std::memory_order_relaxed)};
}

}
