#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified {

struct MemoryCoreCounts {
    std::uint64_t writesOpened = 0;
    std::uint64_t hostWritesOpened = 0;
    std::uint64_t gpuOwnedFaults = 0;
    std::uint64_t foreignFaults = 0;
    std::uint64_t staleFaults = 0;
    std::uint64_t landedBytes = 0;
    std::uint64_t driverPagesSettled = 0;
};

class MemoryCore {
public:
    static MemoryCore& Get();

    bool Install();
    void Uninstall();
    void Register(std::uint64_t address, std::uint64_t size);
    void Reserve(std::uint64_t address, std::uint64_t size);
    void MakeResident(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool OwnedBy(std::uint64_t address, std::uint64_t size) const { return pages.OwnedBy(address, size); }
    void Unregister(std::uint64_t address, std::uint64_t size);
    std::vector<DirtyRange> ConsumeDirty();
    void AdvanceFrame() { pages.AdvanceFrame(); }
    void MarkGpuOwned(std::uint64_t address, std::uint64_t size);
    bool HandleFault(std::uint64_t address, bool write);
    void PrepareHostWrite(std::uint64_t address, std::uint64_t size);
    void LandGpuWrite(std::uint64_t address, std::span<const std::byte> bytes, void* view = nullptr);
    void CollectWrites(std::uint64_t address, std::uint64_t size, PageTable::WrittenRun written, void* context);
    void NoteDriverWrite(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] PageTable& Pages() { return pages; }
    [[nodiscard]] Protector& Protection() { return protector; }
    [[nodiscard]] MemoryCoreCounts Counts() const;

private:
    static std::uint64_t PageHash(std::uint64_t page);
    bool rehash(std::uint64_t page, bool proven);

    std::mutex hashMutex;
    std::mutex collectMutex;
    std::mutex mutex;
    PageTable pages;
    std::unordered_map<std::uint64_t, std::uint64_t> busyHashes;
    std::unordered_map<std::uint64_t, std::uint64_t> collectHashes;
    Protector protector;
    std::atomic<std::uint64_t> writesOpened{0};
    std::atomic<std::uint64_t> hostWritesOpened{0};
    std::atomic<std::uint64_t> gpuOwnedFaults{0};
    std::atomic<std::uint64_t> foreignFaults{0};
    std::atomic<std::uint64_t> staleFaults{0};
    std::atomic<std::uint64_t> landedBytes{0};
    std::atomic<std::uint64_t> driverPagesSettled{0};
};

}

#endif
