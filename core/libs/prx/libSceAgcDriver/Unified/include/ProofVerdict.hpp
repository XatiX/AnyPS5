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

enum class SpanKind : std::uint8_t {
    Storage,
    View,
    Snapshot,
    Keys,
    Direct,
    Written,
    Buffer,
    Count
};

struct Span {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uintptr_t owner = 0;
    SpanKind kind = SpanKind::Buffer;
};

inline constexpr std::size_t GuardCount = 5;

struct Guards {
    std::array<std::uint64_t, GuardCount> serials{};
    bool operator==(const Guards&) const = default;
};

struct Stamp {
    std::uint64_t serial = 0;
    std::uint64_t imageSerial = 0;
    Guards guards;
    bool valid = false;
};

constexpr bool ReadInPlace(SpanKind kind) {
    return kind == SpanKind::Direct || kind == SpanKind::Buffer;
}

enum class Miss : std::uint8_t {
    None,
    NoStamp,
    Unregistered,
    Guard,
    CpuWrites,
    Range,
    Count
};

[[nodiscard]] Miss Verdict(const Events::ChangeLog& log, const Events::ChangeLog& images, const Stamp& last, const Guards& now, bool cpuWriters, bool registered, std::span<const Span> spans, bool* exempted = nullptr, const Span** changed = nullptr);

inline constexpr std::size_t PathCount = 3;

struct VerdictCounts {
    std::uint64_t agreed = 0;
    std::uint64_t unsafe = 0;
    std::uint64_t bothRefused = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Miss::Count)> opportunities{};
    std::array<std::uint64_t, GuardCount> guardMoves{};
    std::array<std::array<std::uint64_t, PathCount>, 2> agreedBy{};
    std::array<std::uint64_t, static_cast<std::size_t>(SpanKind::Count)> rangeKinds{};
    std::uint64_t workSeen = 0;
    std::uint64_t shortcuts = 0;
    std::uint64_t shortcutFallbacks = 0;
};

class Tally {
public:
    void Count(Miss miss, bool existing);
    void CountGuards(const Guards& last, const Guards& now);
    void CountAgreed(std::size_t path, bool exempted);
    void CountWorkSeen() { workSeen.fetch_add(1, std::memory_order_relaxed); }
    void CountRangeKind(SpanKind kind) { rangeKinds[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed); }
    void CountViewWriter(std::size_t writer) { viewWriters[writer < 5 ? writer : 4].fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] std::array<std::uint64_t, 5> ViewWriters() const { return {viewWriters[0].load(), viewWriters[1].load(), viewWriters[2].load(), viewWriters[3].load(), viewWriters[4].load()}; }
    void CountShortcut(bool taken) { (taken ? shortcuts : shortcutFallbacks).fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] VerdictCounts Counts() const;

private:
    std::atomic<std::uint64_t> workSeen{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(SpanKind::Count)> rangeKinds{};
    std::array<std::atomic<std::uint64_t>, 5> viewWriters{};
    std::atomic<std::uint64_t> shortcuts{0};
    std::atomic<std::uint64_t> shortcutFallbacks{0};
    std::array<std::atomic<std::uint64_t>, 3> outcomes{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Miss::Count)> opportunities{};
    std::array<std::atomic<std::uint64_t>, GuardCount> guardMoves{};
    std::array<std::array<std::atomic<std::uint64_t>, PathCount>, 2> agreedBy{};
};

bool VerifyActive();
bool ShortcutActive();
Tally& SharedTally();
std::string VerdictLine();

}

#endif
