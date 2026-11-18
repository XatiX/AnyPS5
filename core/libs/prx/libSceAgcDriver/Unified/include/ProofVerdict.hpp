#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROOFVERDICT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROOFVERDICT_HPP

#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Unified::Proof {

struct Span {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uintptr_t owner = 0;
};

inline constexpr std::size_t GuardCount = 5;

struct Guards {
    std::array<std::uint64_t, GuardCount> serials{};
    bool operator==(const Guards&) const = default;
};

struct Stamp {
    std::uint64_t serial = 0;
    Guards guards;
    bool valid = false;
};

enum class Miss : std::uint8_t {
    None,
    NoStamp,
    Unregistered,
    Guard,
    CpuWrites,
    Range,
    Count
};

[[nodiscard]] Miss Verdict(const Events::ChangeLog& log, const Stamp& last, const Guards& now, bool cpuWriters, bool registered, std::span<const Span> spans);

struct VerdictCounts {
    std::uint64_t agreed = 0;
    std::uint64_t unsafe = 0;
    std::uint64_t bothRefused = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Miss::Count)> opportunities{};
    std::array<std::uint64_t, GuardCount> guardMoves{};
};

class Tally {
public:
    void Count(Miss miss, bool existing);
    void CountGuards(const Guards& last, const Guards& now);
    [[nodiscard]] VerdictCounts Counts() const;

private:
    std::array<std::atomic<std::uint64_t>, 3> outcomes{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Miss::Count)> opportunities{};
    std::array<std::atomic<std::uint64_t>, GuardCount> guardMoves{};
};

bool VerifyActive();
Tally& SharedTally();
std::string VerdictLine();

}

#endif
