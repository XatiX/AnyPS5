#include "prx/libSceAgcDriver/Unified/include/ProofVerdict.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Unified::Proof {

namespace {

enum Outcome : std::size_t {
    Agreed,
    Unsafe,
    BothRefused
};

}

Miss Verdict(const Events::ChangeLog& log, const Stamp& last, const Guards& now, bool cpuWriters, bool registered, std::span<const Span> spans, bool* exempted) {
    if (!last.valid) return Miss::NoStamp;
    if (!registered) return Miss::Unregistered;
    if (last.guards != now) return Miss::Guard;
    if (cpuWriters) return Miss::CpuWrites;
    for (const auto& span : spans) {
        if (log.ChangedSince(span.begin, span.end - span.begin, last.serial, span.owner, exempted)) return Miss::Range;
    }
    return Miss::None;
}

void Tally::CountAgreed(std::size_t path, bool exempted) {
    if (path < PathCount) agreedBy[exempted ? 1 : 0][path].fetch_add(1, std::memory_order_relaxed);
}

void Tally::Count(Miss miss, bool existing) {
    if (miss == Miss::None) {
        outcomes[existing ? Agreed : Unsafe].fetch_add(1, std::memory_order_relaxed);
        if (!existing && std::getenv("APS5_RECODE_PROOF_ABORT") != nullptr) {
            std::fputs("[recode-proof] unsafe verdict: the change log proved an object the existing proof refused\n", stderr);
            std::abort();
        }
        return;
    }
    if (existing) opportunities[static_cast<std::size_t>(miss)].fetch_add(1, std::memory_order_relaxed);
    else outcomes[BothRefused].fetch_add(1, std::memory_order_relaxed);
}

void Tally::CountGuards(const Guards& last, const Guards& now) {
    for (std::size_t i = 0; i < GuardCount; ++i) {
        if (last.serials[i] != now.serials[i]) guardMoves[i].fetch_add(1, std::memory_order_relaxed);
    }
}

VerdictCounts Tally::Counts() const {
    VerdictCounts counts;
    for (std::size_t i = 0; i < GuardCount; ++i) counts.guardMoves[i] = guardMoves[i].load(std::memory_order_relaxed);
    counts.workSeen = workSeen.load(std::memory_order_relaxed);
    counts.shortcuts = shortcuts.load(std::memory_order_relaxed);
    counts.shortcutFallbacks = shortcutFallbacks.load(std::memory_order_relaxed);
    for (std::size_t exempt = 0; exempt < 2; ++exempt) {
        for (std::size_t path = 0; path < PathCount; ++path) counts.agreedBy[exempt][path] = agreedBy[exempt][path].load(std::memory_order_relaxed);
    }
    counts.agreed = outcomes[Agreed].load(std::memory_order_relaxed);
    counts.unsafe = outcomes[Unsafe].load(std::memory_order_relaxed);
    counts.bothRefused = outcomes[BothRefused].load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < opportunities.size(); ++i) counts.opportunities[i] = opportunities[i].load(std::memory_order_relaxed);
    return counts;
}

bool VerifyActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECODE_PROOF");
        return value != nullptr && std::strcmp(value, "verify") == 0;
    }();
    return active;
}

bool ShortcutActive() {
    static const bool active = [] {
        const char* value = std::getenv("APS5_RECODE_PROOF");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return active;
}

Tally& SharedTally() {
    static Tally& tally = *new Tally();
    return tally;
}

std::string VerdictLine() {
    if (!VerifyActive() && !ShortcutActive()) return {};
    const auto counts = SharedTally().Counts();
    const auto& missed = counts.opportunities;
    const auto& moves = counts.guardMoves;
    char line[512];
    std::snprintf(line, sizeof(line), "[recode-proof] agreed %llu, unsafe %llu, both refused %llu; refused while proved: no stamp %llu, unregistered %llu, guard %llu (proof %llu, depth %llu, multisample %llu, retired %llu, imports %llu), cpu writes %llu, range %llu\n",
        static_cast<unsigned long long>(counts.agreed), static_cast<unsigned long long>(counts.unsafe), static_cast<unsigned long long>(counts.bothRefused),
        static_cast<unsigned long long>(missed[static_cast<std::size_t>(Miss::NoStamp)]), static_cast<unsigned long long>(missed[static_cast<std::size_t>(Miss::Unregistered)]),
        static_cast<unsigned long long>(missed[static_cast<std::size_t>(Miss::Guard)]), static_cast<unsigned long long>(moves[0]), static_cast<unsigned long long>(moves[1]), static_cast<unsigned long long>(moves[2]), static_cast<unsigned long long>(moves[3]), static_cast<unsigned long long>(moves[4]),
        static_cast<unsigned long long>(missed[static_cast<std::size_t>(Miss::CpuWrites)]), static_cast<unsigned long long>(missed[static_cast<std::size_t>(Miss::Range)]));
    const auto& by = counts.agreedBy;
    char paths[384];
    std::snprintf(paths, sizeof(paths), "[recode-proof] agreed by existing path (clean/own exempt): fast %llu/%llu, own refreshed %llu/%llu, full %llu/%llu; clean agreements whose own proof noted changes over the spans %llu; shortcuts %llu, tail fallbacks %llu\n",
        static_cast<unsigned long long>(by[0][0]), static_cast<unsigned long long>(by[1][0]), static_cast<unsigned long long>(by[0][1]), static_cast<unsigned long long>(by[1][1]), static_cast<unsigned long long>(by[0][2]), static_cast<unsigned long long>(by[1][2]),
        static_cast<unsigned long long>(counts.workSeen), static_cast<unsigned long long>(counts.shortcuts), static_cast<unsigned long long>(counts.shortcutFallbacks));
    return std::string(line) + paths;
}

}
