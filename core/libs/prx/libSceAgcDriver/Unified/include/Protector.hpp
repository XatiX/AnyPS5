#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROTECTOR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROTECTOR_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Unified {

struct ProtectorCounts {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t neverArmed = 0;
    std::uint64_t failures = 0;
    std::uint64_t deferred = 0;
    std::uint64_t splitCalls = 0;
    std::uint64_t skippedBytes = 0;
};

class Protector {
public:
    void NeverArm(std::uint64_t address, std::uint64_t size);
    void AllowArm(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool NeverArmed(std::uint64_t address) const;
    std::size_t Apply(std::span<const ProtectionChange> changes);
    void Defer(std::span<const ProtectionChange> changes);
    std::size_t Flush();
    [[nodiscard]] ProtectorCounts Counts() const;

private:
    std::size_t applyLocked(std::span<const ProtectionChange> changes);
    bool protect(std::uint64_t address, std::uint64_t size, Protection protection);

    mutable std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> excluded;
    std::vector<ProtectionChange> pending;
    ProtectorCounts counts;
};

}

#endif
