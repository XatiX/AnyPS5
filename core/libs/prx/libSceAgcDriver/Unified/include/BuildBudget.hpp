#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_BUILDBUDGET_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_BUILDBUDGET_HPP

#include <cstdint>

namespace AgcDriver::Unified::BuildBudget {

struct Running {
    std::uint32_t builds = 0;
    std::uint64_t memoryBytes = 0;
};

struct Machine {
    std::uint32_t processors = 1;
    std::uint64_t freeMemoryBytes = 0;
    std::uint32_t fixedBuilds = 0;
};

[[nodiscard]] Machine Host();
[[nodiscard]] std::uint32_t Threads(const Machine& machine);
[[nodiscard]] std::uint32_t Share(const Machine& machine, bool frameWaits);
[[nodiscard]] std::uint64_t MemoryBytes(std::uint64_t spirvBytes);
[[nodiscard]] bool Admits(const Machine& machine, bool frameWaits, Running running, std::uint64_t spirvBytes);

}

#endif
