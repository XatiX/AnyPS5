#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MEMORYCORE_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace AgcDriver::Unified {

struct MemoryCoreCounts {
    std::uint64_t writesOpened = 0;
    std::uint64_t hostWritesOpened = 0;
    std::uint64_t gpuOwnedFaults = 0;
    std::uint64_t foreignFaults = 0;
    std::uint64_t staleFaults = 0;
};

class MemoryCore {
public:
    static MemoryCore& Get();

    bool Install();
    void Uninstall();
    void Register(std::uint64_t address, std::uint64_t size);
    void Unregister(std::uint64_t address, std::uint64_t size);
    std::vector<DirtyRange> ConsumeDirty();
    void MarkGpuOwned(std::uint64_t address, std::uint64_t size);
    bool HandleFault(std::uint64_t address, bool write);
    void PrepareHostWrite(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] PageTable& Pages() { return pages; }
    [[nodiscard]] Protector& Protection() { return protector; }
    [[nodiscard]] MemoryCoreCounts Counts() const;

private:
    std::mutex mutex;
    PageTable pages;
    Protector protector;
    std::atomic<std::uint64_t> writesOpened{0};
    std::atomic<std::uint64_t> hostWritesOpened{0};
    std::atomic<std::uint64_t> gpuOwnedFaults{0};
    std::atomic<std::uint64_t> foreignFaults{0};
    std::atomic<std::uint64_t> staleFaults{0};
};

}

#endif
