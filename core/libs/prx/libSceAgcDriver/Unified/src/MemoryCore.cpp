#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#ifdef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#endif

namespace AgcDriver::Unified {

namespace {

bool handleGuestFault(std::uintptr_t address, bool write) {
    return MemoryCore::Get().HandleFault(address, write);
}

}

MemoryCore& MemoryCore::Get() {
    static MemoryCore& core = *new MemoryCore();
    return core;
}

void MemoryCore::Install() {
#ifdef _WIN32
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(handleGuestFault);
#endif
}

void MemoryCore::Uninstall() {
#ifdef _WIN32
    GuestArena::GuestArenaSetGpuFaultHandler_nid_no_patch(nullptr);
#endif
}

void MemoryCore::Register(std::uint64_t address, std::uint64_t size) {
    protector.Apply(pages.Register(address, size));
}

void MemoryCore::Unregister(std::uint64_t address, std::uint64_t size) {
    protector.Apply(pages.Unregister(address, size));
}

std::vector<DirtyRange> MemoryCore::ConsumeDirty() {
    std::vector<ProtectionChange> changes;
    auto ranges = pages.ConsumeDirty(changes);
    protector.Apply(changes);
    return ranges;
}

void MemoryCore::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    protector.Apply(pages.MarkGpuOwned(address, size));
}

bool MemoryCore::HandleFault(std::uint64_t address, bool write) {
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
    std::vector<ProtectionChange> changes;
    if (!pages.HandleWrite(address, changes)) return false;
    protector.Apply(changes);
    writesOpened.fetch_add(1, std::memory_order_relaxed);
    return true;
}

MemoryCoreCounts MemoryCore::Counts() const {
    return {writesOpened.load(std::memory_order_relaxed), gpuOwnedFaults.load(std::memory_order_relaxed), foreignFaults.load(std::memory_order_relaxed)};
}

}
