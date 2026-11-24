#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_OPERATIONGUARD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_OPERATIONGUARD_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::Unified {

struct OperationGuard {
    std::uint64_t submission = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    bool condition = false;
    std::array<std::uint32_t, 5> words{};
    Predication predication;
    std::uint32_t sampleDumpBlocks = 0;
};

}

#endif
