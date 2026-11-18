#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordVerify.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordDraw.hpp"
#include <atomic>
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <span>
#include <utility>
#include <string>

namespace AgcDriver::Unified::Shadow {

namespace {

bool drawScope() {
    static const bool scoped = [] {
        const char* value = std::getenv("APS5_RECODE_MEMORY");
        return value != nullptr && std::strcmp(value, "draws") == 0;
    }();
    return scoped;
}

void registerUses(std::span<const GuestUse> uses);

void consumeDecoded(const Operation& operation, const DecodeState& state) {
    if (Record::VerifyActive()) Record::SharedVerifier().NoteDecoded(operation, state);
    if (Record::DrawActive()) Record::SharedHandoff().Offer(operation, state);
}

bool recording() {
    return Record::VerifyActive() || Record::DrawActive();
}

Front& front() {
    static Front& shared = *new Front(recording() ? Front::Consumer(consumeDecoded) : Front::Consumer(), std::getenv("APS5_RECODE_RESOLVE") != nullptr || drawScope() || recording(), drawScope() ? UseSink(registerUses) : UseSink());
    return shared;
}

struct MemoryTotals {
    std::atomic<std::uint64_t> registrations{0};
    std::atomic<std::uint64_t> registeredBytes{0};
    std::atomic<std::uint64_t> outsideArena{0};
    std::atomic<std::uint64_t> accesses{0};
    std::atomic<std::uint64_t> unknownAccesses{0};
    std::atomic<std::uint64_t> proofObjects{0};
    std::atomic<std::uint64_t> proofObjectsOutside{0};
    std::atomic<std::uint64_t> consumes{0};
    std::atomic<std::uint64_t> ranges{0};
    std::atomic<std::uint64_t> dirtyBytes{0};
    std::atomic<std::uint64_t> consumeNanoseconds{0};
    std::atomic<std::uint64_t> maxConsumeNanoseconds{0};
};

MemoryTotals& memoryTotals() {
    static MemoryTotals& totals = *new MemoryTotals();
    return totals;
}

std::atomic<bool>& memoryRefused() {
    static std::atomic<bool> refused{false};
    return refused;
}

bool memoryInstalled() {
    static const bool installed = [] {
        const bool ok = MemoryCore::Get().Install();
        if (!ok) {
            memoryRefused().store(true, std::memory_order_relaxed);
            std::fputs("[recode-memory] write faults refused (no APS5RZ01 mark and no APS5_PROTECT_TRACKING, or APS5_NO_PROTECT_TRACKING); memory core off\n", stderr);
        }
        return ok;
    }();
    return installed;
}

bool registerRange(std::uint64_t begin, std::uint64_t end) {
    if (begin == 0 || end <= begin) return false;
    static std::mutex seenMutex;
    static std::map<std::pair<std::uint64_t, std::uint64_t>, bool>& seen = *new std::map<std::pair<std::uint64_t, std::uint64_t>, bool>();
    const auto first = begin & ~(PageBytes - 1);
    const auto last = (end + PageBytes - 1) & ~(PageBytes - 1);
    const bool inside = GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(first), static_cast<std::size_t>(last - first));
    {
        std::lock_guard lock(seenMutex);
        if (!seen.emplace(std::pair{first, last}, inside).second) return inside;
    }
    if (!inside) {
        memoryTotals().outsideArena.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MemoryCore::Get().Register(first, last - first);
    auto& totals = memoryTotals();
    totals.registrations.fetch_add(1, std::memory_order_relaxed);
    totals.registeredBytes.fetch_add(last - first, std::memory_order_relaxed);
    return true;
}

void registerUses(std::span<const GuestUse> uses) {
    if (!memoryInstalled()) return;
    for (const auto& use : uses) registerRange(use.address, use.address + use.size);
}

void consumeMemory() {
    if (!memoryInstalled()) return;
    const auto start = std::chrono::steady_clock::now();
    const auto ranges = MemoryCore::Get().ConsumeDirty();
    const auto nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    auto& totals = memoryTotals();
    std::uint64_t bytes = 0;
    for (const auto& range : ranges) bytes += range.size;
    totals.consumes.fetch_add(1, std::memory_order_relaxed);
    totals.ranges.fetch_add(ranges.size(), std::memory_order_relaxed);
    totals.dirtyBytes.fetch_add(bytes, std::memory_order_relaxed);
    totals.consumeNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto seen = totals.maxConsumeNanoseconds.load(std::memory_order_relaxed);
    while (nanoseconds > seen && !totals.maxConsumeNanoseconds.compare_exchange_weak(seen, nanoseconds, std::memory_order_relaxed)) {
    }
}

std::string memoryLine() {
    if (!MemoryActive() || memoryRefused().load(std::memory_order_relaxed)) return {};
    auto& totals = memoryTotals();
    const auto core = MemoryCore::Get().Counts();
    const auto table = MemoryCore::Get().Pages().Counts();
    const auto protect = MemoryCore::Get().Protection().Counts();
    const auto consumes = totals.consumes.load(std::memory_order_relaxed);
    const auto nanoseconds = totals.consumeNanoseconds.load(std::memory_order_relaxed);
    char line[896];
    std::snprintf(line, sizeof(line), "[recode-memory] registered %llu ranges %llu MiB, %llu outside the arena; backend accesses %llu, unknown memory %llu; consumes %llu, dirty ranges %llu, dirty %llu MiB, consume %.1f us avg %.1f us max; write faults opened %llu, host writes opened %llu, stale %llu, gpu-owned %llu, foreign %llu; kept busy %llu, rearmed %llu, busy unchanged %llu; protect calls %llu (%llu MiB), split %llu, skipped %llu MiB, failures %llu\n",
        static_cast<unsigned long long>(totals.registrations.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.registeredBytes.load(std::memory_order_relaxed) >> 20), static_cast<unsigned long long>(totals.outsideArena.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(totals.accesses.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.unknownAccesses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(consumes), static_cast<unsigned long long>(totals.ranges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.dirtyBytes.load(std::memory_order_relaxed) >> 20),
        consumes == 0 ? 0.0 : static_cast<double>(nanoseconds) / static_cast<double>(consumes) / 1000.0, static_cast<double>(totals.maxConsumeNanoseconds.exchange(0, std::memory_order_relaxed)) / 1000.0,
        static_cast<unsigned long long>(core.writesOpened), static_cast<unsigned long long>(core.hostWritesOpened), static_cast<unsigned long long>(core.staleFaults), static_cast<unsigned long long>(core.gpuOwnedFaults), static_cast<unsigned long long>(core.foreignFaults),
        static_cast<unsigned long long>(table.keptBusy), static_cast<unsigned long long>(table.rearmed), static_cast<unsigned long long>(table.unchangedBusy),
        static_cast<unsigned long long>(protect.calls), static_cast<unsigned long long>(protect.bytes >> 20), static_cast<unsigned long long>(protect.splitCalls), static_cast<unsigned long long>(protect.skippedBytes >> 20), static_cast<unsigned long long>(protect.failures));
    const auto events = Events::Shared().Counts();
    char eventLine[384];
    std::snprintf(eventLine, sizeof(eventLine), "[recode-events] notes %llu serial %llu; queries %llu, region skips %llu, changed %llu, owner exempt %llu; proof objects %llu, with ranges outside the arena %llu\n", static_cast<unsigned long long>(events.notes), static_cast<unsigned long long>(Events::Shared().Serial()), static_cast<unsigned long long>(events.queries), static_cast<unsigned long long>(events.regionSkips), static_cast<unsigned long long>(events.changed), static_cast<unsigned long long>(events.ownerExempt), static_cast<unsigned long long>(totals.proofObjects.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.proofObjectsOutside.load(std::memory_order_relaxed)));
    return std::string(line) + eventLine;
}

void report() {
    static std::mutex reportMutex;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(reportMutex);
        if (now - last < std::chrono::seconds(10)) return;
        last = now;
    }
    if (!Active()) {
        const auto memory = memoryLine() + Record::VerifyLine() + Record::HandoffLine();
        if (!memory.empty()) std::fputs(memory.c_str(), stderr);
        return;
    }
    std::string line = "[recode-shadow]";
    std::uint64_t draws = 0, conditionalDraws = 0, dispatches = 0, conditionalDispatches = 0;
    for (const auto& lane : front().Totals(false)) {
        draws += lane.counts.draws;
        conditionalDraws += lane.counts.conditionalDraws;
        dispatches += lane.counts.dispatches;
        conditionalDispatches += lane.counts.conditionalDispatches;
        char part[384];
        std::snprintf(part, sizeof(part), " q0x%x: %llu subs %llu ops %llu draws %llu dispatches %llu flips %.2f us/op stalls %llu memstate %llu errors %llu resolved %llu resolve errors %llu;", lane.queue, static_cast<unsigned long long>(lane.submissions), static_cast<unsigned long long>(lane.counts.operations), static_cast<unsigned long long>(lane.counts.draws), static_cast<unsigned long long>(lane.counts.dispatches), static_cast<unsigned long long>(lane.counts.flips), lane.NanosecondsPerOperation() / 1000.0, static_cast<unsigned long long>(lane.counts.snapshotStalls), static_cast<unsigned long long>(lane.counts.stateFromMemory), static_cast<unsigned long long>(lane.counts.stateErrors), static_cast<unsigned long long>(lane.counts.resolved), static_cast<unsigned long long>(lane.counts.resolveErrors));
        line += part;
    }
    const auto order = front().Ordering().Counts();
    char tail[160];
    std::snprintf(tail, sizeof(tail), " waits: edges %llu released %llu pending %llu superseded %llu always %llu\n", static_cast<unsigned long long>(order.edges), static_cast<unsigned long long>(order.released), static_cast<unsigned long long>(order.pending), static_cast<unsigned long long>(order.superseded), static_cast<unsigned long long>(order.alwaysPass));
    line += tail;
    const auto driver = PacketCounters::Sum();
    char compare[256];
    std::snprintf(compare, sizeof(compare), "[recode-shadow] compare: draws shadow %llu (conditional %llu) driver %llu; dispatches shadow %llu (conditional %llu) driver %llu\n", static_cast<unsigned long long>(draws), static_cast<unsigned long long>(conditionalDraws), static_cast<unsigned long long>(driver.draws), static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(conditionalDispatches), static_cast<unsigned long long>(driver.dispatches));
    line += compare;
    line += memoryLine();
    line += Record::VerifyLine();
    line += Record::HandoffLine();
    std::fputs(line.c_str(), stderr);
}

}

bool Active() {
    static const bool active = std::getenv("APS5_RECODE_SHADOW") != nullptr;
    return active;
}

bool MemoryActive() {
    static const bool active = std::getenv("APS5_RECODE_MEMORY") != nullptr;
    return active;
}

bool DrawScope() {
    return drawScope();
}

bool Teeing() {
    static const bool teeing = Active() || MemoryActive() || recording();
    return teeing;
}

std::shared_ptr<QueueState> RecordedState(const DriverDetail::Submission& submission, std::size_t packetOffset) {
    if (!Record::DrawActive()) return nullptr;
    return Record::SharedHandoff().Take(submission.queue, submission.serial, packetOffset, std::chrono::milliseconds(100));
}

void NoteDriverDraw(const QueueState& queue, const DriverDetail::Submission& submission, std::size_t packetOffset) {
    if (!Record::VerifyActive()) return;
    Record::SharedVerifier().NoteDriverDraw(queue, submission.queue, submission.serial, packetOffset);
}

void NoteGuestAccess(GuestRanges reads, GuestRanges writes, bool unknown) {
    if (!drawScope() || !memoryInstalled()) return;
    auto& totals = memoryTotals();
    totals.accesses.fetch_add(1, std::memory_order_relaxed);
    if (unknown) totals.unknownAccesses.fetch_add(1, std::memory_order_relaxed);
    for (const auto& [begin, end] : reads) registerRange(begin, end);
    for (const auto& [begin, end] : writes) registerRange(begin, end);
}

bool NoteProofRanges(GuestRanges ranges) {
    if (!drawScope() || !memoryInstalled()) return false;
    bool registered = true;
    for (const auto& [begin, end] : ranges) registered = registerRange(begin, end) && registered;
    auto& totals = memoryTotals();
    totals.proofObjects.fetch_add(1, std::memory_order_relaxed);
    if (!registered) totals.proofObjectsOutside.fetch_add(1, std::memory_order_relaxed);
    return registered;
}

void NoteGpuMapping(const GuestAllocations::Mapped& ranges) {
    if (!MemoryActive() || drawScope() || !memoryInstalled()) return;
    auto& totals = memoryTotals();
    for (const auto& weak : ranges) {
        const auto range = weak.lock();
        if (!range || range->bytes == 0) continue;
        const auto first = range->address & ~(PageBytes - 1);
        const auto last = (range->address + range->bytes + PageBytes - 1) & ~(PageBytes - 1);
        MemoryCore::Get().Register(first, last - first);
        totals.registrations.fetch_add(1, std::memory_order_relaxed);
        totals.registeredBytes.fetch_add(last - first, std::memory_order_relaxed);
    }
}

std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission) {
    auto copy = std::make_shared<DriverDetail::Submission>();
    copy->serial = submission.serial;
    copy->queue = submission.queue;
    copy->commands = submission.commands;
    copy->conditionalEnds = submission.conditionalEnds;
    copy->registerLists = submission.registerLists;
    copy->flipHandles = submission.flipHandles;
    for (const auto& [cursor, request] : submission.flips) copy->flips.emplace(cursor, nullptr);
    return copy;
}

void Tee(const DriverDetail::Submission& submission) {
    const bool decoding = Active() || drawScope() || recording();
    if (submission.suspend && decoding) front().NoteSuspend();
    if (submission.suspend || submission.commands.empty()) return;
    if (MemoryActive()) {
        consumeMemory();
        if (!submission.flips.empty() && memoryInstalled()) MemoryCore::Get().AdvanceFrame();
    }
    if (decoding) front().Push(Copy(submission));
    report();
}

}
