#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <array>
#include <cstdlib>
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

void MemoryCore::Reserve(std::uint64_t address, std::uint64_t size) {
    pages.Reserve(address, size);
}

void MemoryCore::MakeResident(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.MakeResident(address, size));
}

void MemoryCore::Unregister(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.Unregister(address, size));
}

std::uint64_t MemoryCore::PageHash(std::uint64_t page) {
    const auto* words = reinterpret_cast<const std::uint64_t*>(page * PageBytes);
    std::array<std::uint64_t, 4> lanes{0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    for (std::size_t i = 0; i < PageBytes / sizeof(std::uint64_t); i += lanes.size()) {
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) lanes[lane] = (lanes[lane] ^ words[i + lane]) * 0x100000001b3ull + (lanes[lane] >> 29u);
    }
    return lanes[0] ^ (lanes[1] * 31u) ^ (lanes[2] * 131u) ^ (lanes[3] * 1031u);
}

bool MemoryCore::rehash(std::uint64_t page, bool proven) {
    const auto hash = PageHash(page);
    auto [entry, inserted] = busyHashes.try_emplace(page, hash);
    if (inserted || proven || entry->second != hash) {
        entry->second = hash;
        return true;
    }
    return false;
}

std::vector<DirtyRange> MemoryCore::ConsumeDirty() {
    static const bool hashBusy = std::getenv("APS5_RECODE_NO_BUSY_HASH") == nullptr;
    static const bool unlockedHash = std::getenv("APS5_RECODE_NO_UNLOCKED_HASH") == nullptr;
    std::lock_guard hashLock(hashMutex);
    std::unordered_map<std::uint64_t, bool> hashed;
    if (hashBusy && unlockedHash) {
        for (const auto page : pages.BusyPages()) hashed.emplace(page, rehash(page, false));
    }
    std::lock_guard lock(mutex);
    std::vector<ProtectionChange> changes;
    const PageTable::WriteCheck written = [this, &hashed](std::uint64_t page, bool proven) {
        if (!proven) {
            if (const auto found = hashed.find(page); found != hashed.end()) return found->second;
        }
        return rehash(page, proven);
    };
    auto ranges = hashBusy ? pages.ConsumeDirty(changes, written) : pages.ConsumeDirty(changes);
    protector.Apply(changes);
    for (const auto& range : ranges) Events::Shared().Note(range.address, range.size);
    return ranges;
}

void MemoryCore::MarkGpuOwned(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(mutex);
    protector.Apply(pages.MarkGpuOwned(address, size));
}

bool MemoryCore::HandleFault(std::uint64_t address, bool write) {
    std::lock_guard lock(mutex);
    const auto state = pages.StateOf(address);
    if (state == PageState::Untracked || state == PageState::Reserved) {
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
        Events::Shared().Note(address & ~(PageBytes - 1), PageBytes);
        const ProtectionChange reopen{address & ~(PageBytes - 1), PageBytes, Protection::ReadWrite};
        return protector.Apply(std::span(&reopen, 1)) == 1;
    }
    std::vector<ProtectionChange> changes;
    if (!pages.HandleWrite(address, changes)) return false;
    Events::Shared().Note(address & ~(PageBytes - 1), PageBytes);
    protector.Apply(changes);
    writesOpened.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MemoryCore::PrepareHostWrite(std::uint64_t address, std::uint64_t size) {
    if (size == 0) return;
    Events::Shared().Note(address, size);
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
