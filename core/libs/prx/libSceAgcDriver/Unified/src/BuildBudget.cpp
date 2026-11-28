#include "prx/libSceAgcDriver/Unified/include/BuildBudget.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::Unified::BuildBudget {

namespace {

constexpr std::uint64_t BaseBuildBytes = 128ull << 20;
constexpr std::uint64_t BuildBytesPerSpirvByte = 400;
constexpr std::uint32_t PlayShareDivisor = 4;

std::uint64_t freeMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) != 0 ? status.ullAvailPhys : 0u;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size) : 0u;
#endif
}

std::uint32_t fixedBuilds() {
    static const std::uint32_t fixed = [] {
        const char* value = std::getenv("APS5_RECODE_PIPELINE_BUILDERS");
        return value != nullptr && std::atoi(value) > 0 ? static_cast<std::uint32_t>(std::atoi(value)) : 0u;
    }();
    return fixed;
}

}

Machine Host() {
    return Machine{std::max(1u, UsableProcessors()), freeMemory(), fixedBuilds()};
}

std::uint32_t Threads(const Machine& machine) {
    if (machine.fixedBuilds != 0u) return machine.fixedBuilds;
    return machine.processors > 1u ? machine.processors - 1u : 1u;
}

std::uint32_t Share(const Machine& machine, bool frameWaits) {
    if (machine.fixedBuilds != 0u) return machine.fixedBuilds;
    return frameWaits ? Threads(machine) : std::max(1u, machine.processors / PlayShareDivisor);
}

std::uint64_t MemoryBytes(std::uint64_t spirvBytes) {
    return BaseBuildBytes + spirvBytes * BuildBytesPerSpirvByte;
}

bool Admits(const Machine& machine, bool frameWaits, Running running, std::uint64_t spirvBytes) {
    if (running.builds == 0u) return true;
    if (running.builds >= Share(machine, frameWaits)) return false;
    if (machine.freeMemoryBytes == 0u) return true;
    const std::uint64_t before = machine.freeMemoryBytes + running.memoryBytes / 2u;
    return running.memoryBytes + MemoryBytes(spirvBytes) <= before / 4u * 3u;
}

}
