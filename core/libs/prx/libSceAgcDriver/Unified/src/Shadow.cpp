#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/ProofVerdict.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordVerify.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordDraw.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/UploadClash.hpp"
#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Unified/include/MetadataWatch.hpp"
#include <atomic>
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <span>
#include <utility>
#include <string>

namespace AgcDriver::Unified::Shadow {

std::string writeAuditLine();

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
    if (Resolve::Active()) Resolve::Consume(operation, state);
}

bool recording() {
    return Record::VerifyActive() || Record::DrawActive() || Resolve::Active();
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
    std::atomic<std::uint64_t> ownedServes{0};
    std::atomic<std::uint64_t> ownedWalks{0};
    std::atomic<std::uint64_t> ownedWalkNanoseconds{0};
    std::atomic<std::uint64_t> ownedRecords{0};
    std::atomic<std::uint64_t> ownedRecordNanoseconds{0};
    std::atomic<std::uint64_t> frames{0};
};

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

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
    ResidencyService::Get().Add(first, last - first);
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
    if (Downgrades::Watching()) {
        for (const auto& range : ranges) Downgrades::WatchTouch("a CPU-dirty range", range.address, range.size);
        Downgrades::WatchPoll();
    }
    if (Mirror::WritesEnabled()) {
        const auto upload = MemoryCore::Get().WithoutGpuWritten(ranges);
        UploadClash::CheckUploads(upload);
        if (Images::DepthRecordsOn()) {
            for (const auto& range : upload) Images::Depths().NoteCpuWrites(range.address, range.size);
        }
        ResidencyService::Get().NoteDirty(upload);
        MemoryCore::Get().LandAhead();
        MemoryCore::Get().LandOpened();
    } else {
        ResidencyService::Get().NoteDirty(ranges);
    }
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

std::string frameLine() {
    struct Sample {
        std::uint64_t frames = 0;
        std::uint64_t writeFaults = 0;
        std::uint64_t ownedFaults = 0;
        std::uint64_t parked = 0;
        std::uint64_t bypassed = 0;
        std::uint64_t protectCalls = 0;
        std::uint64_t faultCalls = 0;
        std::uint64_t faultNanoseconds = 0;
        std::uint64_t readBackBytes = 0;
        std::uint64_t landedBytes = 0;
    };
    static std::mutex sampleMutex;
    static Sample last;
    const auto core = MemoryCore::Get().Counts();
    Sample now;
    now.frames = memoryTotals().frames.load(std::memory_order_relaxed);
    now.writeFaults = core.writesOpened;
    now.ownedFaults = core.gpuOwnedFaults;
    now.parked = core.faultsParked;
    now.bypassed = core.faultsBypassed;
    now.protectCalls = MemoryCore::Get().Protection().Counts().calls;
    now.faultCalls = core.faultCalls;
    now.faultNanoseconds = core.faultNanoseconds;
    now.readBackBytes = ResidencyService::Get().ReadBackBytes();
    now.landedBytes = core.ownedLandedBytes + core.diffLandedBytes + core.landedBytes;
    std::lock_guard lock(sampleMutex);
    const auto frames = now.frames - last.frames;
    const auto each = [&](std::uint64_t current, std::uint64_t before) { return frames == 0u ? 0.0 : static_cast<double>(current - before) / static_cast<double>(frames); };
    char line[640];
    std::snprintf(line, sizeof(line), "[recode-frame] %llu frames; per frame: write faults %.1f, faults on GPU-owned pages %.2f (parked %.2f, bypassed %.2f), protect calls %.1f, fault handler %.1f us in %.1f calls, read back %.1f KiB, landed %.1f KiB\n",
        static_cast<unsigned long long>(frames), each(now.writeFaults, last.writeFaults), each(now.ownedFaults, last.ownedFaults), each(now.parked, last.parked), each(now.bypassed, last.bypassed), each(now.protectCalls, last.protectCalls),
        each(now.faultNanoseconds, last.faultNanoseconds) / 1000.0, each(now.faultCalls, last.faultCalls), each(now.readBackBytes, last.readBackBytes) / 1024.0, each(now.landedBytes, last.landedBytes) / 1024.0);
    last = now;
    return line;
}

std::string footprintLine() {
    std::uint64_t gpuVisible = 0;
    std::uint64_t gpuRanges = 0;
    for (const auto& range : GuestAllocations::GuestAllocationsAcquireAll_nid_postfix()) {
        if (range == nullptr || !range->gpu) continue;
        gpuVisible += range->bytes;
        ++gpuRanges;
    }
    const auto* surfaces = Resolve::Surfaces();
    char line[512];
    std::snprintf(line, sizeof(line), "[recode-footprint] GPU-visible guest memory %llu MiB in %llu ranges; registered %llu MiB; mirror bound %llu MiB; image records %llu MiB in %llu records; depth records %llu\n",
        static_cast<unsigned long long>(gpuVisible >> 20), static_cast<unsigned long long>(gpuRanges), static_cast<unsigned long long>(memoryTotals().registeredBytes.load(std::memory_order_relaxed) >> 20),
        static_cast<unsigned long long>(ResidencyService::Get().BoundBytes() >> 20), static_cast<unsigned long long>(surfaces != nullptr ? surfaces->HeldBytes() >> 20 : 0u), static_cast<unsigned long long>(surfaces != nullptr ? surfaces->Size() : 0u),
        static_cast<unsigned long long>(Images::Depths().Size()));
    return line;
}

std::string memoryLine() {
    if (!MemoryActive() || memoryRefused().load(std::memory_order_relaxed)) return {};
    auto& totals = memoryTotals();
    const auto core = MemoryCore::Get().Counts();
    const auto table = MemoryCore::Get().Pages().Counts();
    const auto protect = MemoryCore::Get().Protection().Counts();
    const auto consumes = totals.consumes.load(std::memory_order_relaxed);
    const auto nanoseconds = totals.consumeNanoseconds.load(std::memory_order_relaxed);
    char line[1024];
    std::snprintf(line, sizeof(line), "[recode-memory] registered %llu ranges %llu MiB, %llu outside the arena; backend accesses %llu, unknown memory %llu; consumes %llu, dirty ranges %llu, dirty %llu MiB, consume %.1f us avg %.1f us max; write faults opened %llu, host writes opened %llu, driver pages settled %llu, stale %llu, gpu-owned %llu, foreign %llu; kept busy %llu, rearmed %llu, busy unchanged %llu, changed while rearming %llu; protect calls %llu (%llu MiB), split %llu, skipped %llu MiB, failures %llu\n",
        static_cast<unsigned long long>(totals.registrations.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.registeredBytes.load(std::memory_order_relaxed) >> 20), static_cast<unsigned long long>(totals.outsideArena.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(totals.accesses.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.unknownAccesses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(consumes), static_cast<unsigned long long>(totals.ranges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.dirtyBytes.load(std::memory_order_relaxed) >> 20),
        consumes == 0 ? 0.0 : static_cast<double>(nanoseconds) / static_cast<double>(consumes) / 1000.0, static_cast<double>(totals.maxConsumeNanoseconds.exchange(0, std::memory_order_relaxed)) / 1000.0,
        static_cast<unsigned long long>(core.writesOpened), static_cast<unsigned long long>(core.hostWritesOpened), static_cast<unsigned long long>(core.driverPagesSettled), static_cast<unsigned long long>(core.staleFaults), static_cast<unsigned long long>(core.gpuOwnedFaults), static_cast<unsigned long long>(core.foreignFaults),
        static_cast<unsigned long long>(table.keptBusy), static_cast<unsigned long long>(table.rearmed), static_cast<unsigned long long>(table.unchangedBusy), static_cast<unsigned long long>(core.rearmedChanged),
        static_cast<unsigned long long>(protect.calls), static_cast<unsigned long long>(protect.bytes >> 20), static_cast<unsigned long long>(protect.splitCalls), static_cast<unsigned long long>(protect.skippedBytes >> 20), static_cast<unsigned long long>(protect.failures));
    const auto events = Events::Shared().Counts();
    char eventLine[384];
    std::snprintf(eventLine, sizeof(eventLine), "[recode-events] notes %llu serial %llu; queries %llu, region skips %llu, changed %llu, owner exempt %llu; proof objects %llu, with ranges outside the arena %llu\n", static_cast<unsigned long long>(events.notes), static_cast<unsigned long long>(Events::Shared().Serial()), static_cast<unsigned long long>(events.queries), static_cast<unsigned long long>(events.regionSkips), static_cast<unsigned long long>(events.changed), static_cast<unsigned long long>(events.ownerExempt), static_cast<unsigned long long>(totals.proofObjects.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.proofObjectsOutside.load(std::memory_order_relaxed)));
    std::string owned;
    if (const auto serves = totals.ownedServes.load(std::memory_order_relaxed); serves != 0) {
        char ownedLine[256];
        std::snprintf(ownedLine, sizeof(ownedLine), "[recode-owned] serves %llu, walks %llu (%.1f ms), upload records %llu (%.1f ms)\n", static_cast<unsigned long long>(serves), static_cast<unsigned long long>(totals.ownedWalks.load(std::memory_order_relaxed)), static_cast<double>(totals.ownedWalkNanoseconds.load(std::memory_order_relaxed)) / 1e6, static_cast<unsigned long long>(totals.ownedRecords.load(std::memory_order_relaxed)), static_cast<double>(totals.ownedRecordNanoseconds.load(std::memory_order_relaxed)) / 1e6);
        owned = ownedLine;
    }
    owned += writeAuditLine();
    if (Mirror::WritesEnabled()) {
        const auto written = MemoryCore::Get().Written().Counts();
        char gpuOwnedLine[768];
        std::snprintf(gpuOwnedLine, sizeof(gpuOwnedLine), "[recode-gpu-owned] pages owned %llu, open %llu KiB; faults parked %llu, bypassed %llu (landed at once %llu), resolved %llu, opened stale %llu; read-ahead pages %llu, hot pages landed %llu; landed %llu MiB whole, %llu KiB by diff (%llu KiB unchanged); small reads %llu (%llu bytes); flushes %llu, pending at flush %llu KiB; pre-images %llu; outside the second view %llu KiB, GPU-only (never landed) %llu MiB; unreadable pages not hashed %llu; pending %llu pieces %llu KiB\n",
            static_cast<unsigned long long>(core.ownedPages), static_cast<unsigned long long>(core.openBytes >> 10), static_cast<unsigned long long>(core.faultsParked), static_cast<unsigned long long>(core.faultsBypassed), static_cast<unsigned long long>(core.bypassLanded),static_cast<unsigned long long>(core.faultsResolved), static_cast<unsigned long long>(core.faultsOpenedStale),
            static_cast<unsigned long long>(core.readAheadPages), static_cast<unsigned long long>(core.hotLandedPages), static_cast<unsigned long long>(core.ownedLandedBytes >> 20), static_cast<unsigned long long>(core.diffLandedBytes >> 10), static_cast<unsigned long long>(core.diffUnchangedBytes >> 10), static_cast<unsigned long long>(core.smallReads), static_cast<unsigned long long>(core.smallReadBytes),
            static_cast<unsigned long long>(core.flushes), static_cast<unsigned long long>(core.flushPendingBytes >> 10), static_cast<unsigned long long>(core.preImages), static_cast<unsigned long long>(core.unviewedBytes >> 10), static_cast<unsigned long long>(core.gpuOnlyBytes >> 20), static_cast<unsigned long long>(core.unreadableHashes), static_cast<unsigned long long>(written.pieces), static_cast<unsigned long long>(written.bytes >> 10));
        owned += gpuOwnedLine;
        owned += "[recode-gpu-owned] landing uploads skipped over later GPU writes: " + std::to_string(Mirror::SkippedUploadBytes() >> 10) + " KiB; CPU faults on records not yet written back: " + std::to_string(core.imageFaults) + " reads, " + std::to_string(core.imageWriteFaults) + " writes; view lookups that waited: " + std::to_string(Mirror::ViewWaits().waited) + " (" + std::to_string(Mirror::ViewWaits().found) + " found, " + std::to_string(Mirror::ViewWaits().microseconds / 1000u) + " ms); writes taken in pieces: " + std::to_string(core.pieceWrites) + ", outside the second view on pages already GPU-owned: " + std::to_string(core.ownedUnviewedBytes >> 10) + " KiB; diff landings: " + std::to_string(core.diffChainedBytes) + " bytes behind an older landing, " + std::to_string(core.diffForeignBytes) + " bytes left unlike the mirror\n";
    }
    return std::string(line) + eventLine + ResidencyService::Get().Line() + owned + UploadClash::Line() + MetadataWatch::Line() + frameLine() + footprintLine();
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
    if (Resolve::Active()) std::fputs((Resolve::Line() + "\n").c_str(), stderr);
    if (Record::RecordActive()) std::fputs((Record::RecordLine() + "\n").c_str(), stderr);
    if (!Active()) {
        const auto memory = memoryLine() + Record::VerifyLine() + Record::HandoffLine() + Proof::VerdictLine();
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
    char tail[256];
    std::snprintf(tail, sizeof(tail), " waits: edges %llu (across queues %llu) released %llu pending %llu (behind another queue's store %llu) superseded %llu always %llu\n", static_cast<unsigned long long>(order.edges), static_cast<unsigned long long>(order.crossEdges), static_cast<unsigned long long>(order.released), static_cast<unsigned long long>(order.pending), static_cast<unsigned long long>(order.crossPending), static_cast<unsigned long long>(order.superseded), static_cast<unsigned long long>(order.alwaysPass));
    line += tail;
    const auto driver = PacketCounters::Sum();
    char compare[256];
    std::snprintf(compare, sizeof(compare), "[recode-shadow] compare: draws shadow %llu (conditional %llu) driver %llu; dispatches shadow %llu (conditional %llu) driver %llu\n", static_cast<unsigned long long>(draws), static_cast<unsigned long long>(conditionalDraws), static_cast<unsigned long long>(driver.draws), static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(conditionalDispatches), static_cast<unsigned long long>(driver.dispatches));
    line += compare;
    line += memoryLine();
    line += Record::VerifyLine();
    line += Record::HandoffLine();
    line += Proof::VerdictLine();
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

void NoteDevice(const std::shared_ptr<VulkanDevice>& device) {
    Resolve::Attach(device);
    Mirror::Attach(device);
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
        ResidencyService::Get().Add(first, last - first);
        totals.registrations.fetch_add(1, std::memory_order_relaxed);
        totals.registeredBytes.fetch_add(last - first, std::memory_order_relaxed);
    }
}

namespace {

bool coreCoversWrites(std::uintptr_t address, std::size_t bytes) {
    return MemoryCore::Get().Pages().Covers(address, bytes);
}

struct WriteAudit {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> hashes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> reported;
    std::atomic<std::uint64_t> pages{0};
    std::atomic<std::uint64_t> misses{0};
    std::vector<std::uintptr_t> listed;
};

WriteAudit& writeAudit() {
    static auto* audit = new WriteAudit;
    return *audit;
}

struct AuditForward {
    void (*written)(void*, std::uintptr_t, std::uintptr_t);
    void* context;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>>* runs;
};

void auditWritten(void* context, std::uintptr_t begin, std::uintptr_t end) {
    auto* forward = static_cast<AuditForward*>(context);
    forward->runs->emplace_back(begin, end);
    forward->written(forward->context, begin, end);
}

bool coreCollectWrites(std::uintptr_t address, std::size_t bytes, void (*written)(void*, std::uintptr_t, std::uintptr_t), void* context) {
    static const bool audit = std::getenv("APS5_RECODE_AUDIT_CORE_WRITES") != nullptr;
    if (!audit) {
        MemoryCore::Get().CollectWrites(address, bytes, written, context);
        return true;
    }
    auto& state = writeAudit();
    std::lock_guard lock(state.mutex);
    state.reported.clear();
    AuditForward forward{written, context, &state.reported};
    auto& pages = MemoryCore::Get().Pages();
    MemoryCore::Get().CollectWrites(address, bytes, auditWritten, &forward);
    const auto end = address + bytes;
    for (auto page = address & ~(PageBytes - 1); page < end; page += PageBytes) {
        const auto kind = pages.StateOf(page);
        if (kind != PageState::CpuClean && kind != PageState::CpuDirty) continue;
        std::uint64_t hash = 0xcbf29ce484222325ull;
        const auto* words = reinterpret_cast<const std::uint64_t*>(page);
        for (std::size_t i = 0; i < PageBytes / sizeof(std::uint64_t); ++i) hash = (hash ^ words[i]) * 0x100000001b3ull;
        state.pages.fetch_add(1, std::memory_order_relaxed);
        auto [entry, inserted] = state.hashes.try_emplace(page, hash);
        if (inserted || entry->second == hash) continue;
        entry->second = hash;
        bool seen = false;
        for (const auto& [from, to] : state.reported) seen = seen || (page >= from && page < to);
        if (seen) continue;
        if (state.listed.size() < 8 && std::find(state.listed.begin(), state.listed.end(), page) == state.listed.end()) state.listed.push_back(page);
        state.misses.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

}

std::string writeAuditLine() {
    auto& state = writeAudit();
    if (state.pages.load(std::memory_order_relaxed) == 0) return {};
    std::string pages;
    {
        std::lock_guard lock(state.mutex);
        for (const auto page : state.listed) {
            char one[32];
            std::snprintf(one, sizeof(one), " 0x%llx", static_cast<unsigned long long>(page));
            pages += one;
        }
    }
    char text[160];
    std::snprintf(text, sizeof(text), "[recode-audit] pages hashed %llu, missed writes %llu, first missed pages", static_cast<unsigned long long>(state.pages.load(std::memory_order_relaxed)), static_cast<unsigned long long>(state.misses.load(std::memory_order_relaxed)));
    return std::string(text) + pages + "\n";
}

namespace {

void coreNoteWritten(std::uintptr_t address, std::size_t bytes) {
    MemoryCore::Get().NoteDriverWrite(address, bytes);
    MetadataWatch::CheckWrite(address, bytes, true, 0u);
    if (Images::DepthRecordsOn() && Images::Depths().NoteHtileWrites(address, bytes) != 0u) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) std::fprintf(stderr, "[recode-depth] first driver write over a depth record's HTILE: 0x%llx+0x%llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    }
}

const GuestMemory::CoreWriteHooks coreWriteHooks{coreCoversWrites, coreCollectWrites, coreNoteWritten};

std::pair<std::uint64_t, std::uint64_t> allocationAround(std::uint64_t first, std::uint64_t last) {
    static const bool whole = std::getenv("APS5_RECODE_NO_ALLOCATION_RANGES") == nullptr;
    if (!whole) return {first, last};
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    bool readable = false;
    if (auto found = GuestAllocations::GuestAllocationsLookup_nid_postfix(static_cast<std::uintptr_t>(first), false)) {
        address = found->address;
        bytes = found->bytes;
        readable = found->readable;
    }
    const auto begin = address & ~(PageBytes - 1);
    const auto end = (address + bytes + PageBytes - 1) & ~(PageBytes - 1);
    if (bytes == 0 || !readable || begin > first || last > end) return {first, last};
    if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(begin), static_cast<std::size_t>(end - begin))) return {first, last};
    return {begin, end};
}

}

bool ServesOwned() {
    static const bool serves = ResidencyService::Enabled() && std::getenv("APS5_RECODE_NO_OWNED_REGIONS") == nullptr;
    if (!serves || !memoryInstalled()) return false;
    static const bool hooked = [] {
        if (std::getenv("APS5_RECODE_CORE_WRITES") != nullptr) GuestMemory::SetCoreWriteHooks(&coreWriteHooks);
        return true;
    }();
    return hooked;
}

bool ServeOwned(const Graphics::Context& context, std::uint64_t address, std::uint64_t size, OwnedSpan* span) {
    if (size == 0 || !ServesOwned()) return false;
    auto& service = ResidencyService::Get();
    if (!service.Attach(context)) return false;
    const auto first = address & ~(PageBytes - 1);
    const auto last = (address + size + PageBytes - 1) & ~(PageBytes - 1);
    auto& totals = memoryTotals();
    totals.ownedServes.fetch_add(1, std::memory_order_relaxed);
    struct Served {
        std::uint64_t last = 0;
        std::uint64_t frame = 0;
        std::uint64_t removals = 0;
        OwnedSpan span;
    };
    static std::mutex servedMutex;
    static auto& served = *new std::map<std::uint64_t, Served>();
    const auto frame = service.Frame();
    const auto removals = service.Removals();
    std::lock_guard lock(servedMutex);
    if (const auto found = served.find(first); found != served.end() && found->second.last == last && found->second.frame == frame && found->second.removals == removals) {
        *span = found->second.span;
        span->address += address - first;
        return true;
    }
    if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(first), static_cast<std::size_t>(last - first))) return false;
    const auto start = std::chrono::steady_clock::now();
    const auto [owner, ownerEnd] = allocationAround(first, last);
    const auto from = std::max(owner, first & ~(GpuPageBytes - 1));
    const auto to = std::min(ownerEnd, (last + GpuPageBytes - 1) & ~(GpuPageBytes - 1));
    const auto committed = GuestMemory::CommittedRanges(from, static_cast<std::size_t>(to - from));
    auto& core = MemoryCore::Get();
    core.Reserve(from, to - from);
    for (const auto& [begin, end] : committed) core.MakeResident(begin, end - begin);
    const auto base = service.Serve(from, to - from, committed, owner, ownerEnd - owner);
    totals.ownedWalks.fetch_add(1, std::memory_order_relaxed);
    totals.ownedWalkNanoseconds.fetch_add(nanosecondsSince(start), std::memory_order_relaxed);
    if (!base) {
        served.erase(first);
        return false;
    }
    const OwnedSpan whole{base->handle, base->address + (first - from), from - base->offset};
    served[first] = {last, frame, service.Removals(), whole};
    *span = whole;
    span->address += address - first;
    return true;
}

void ServeFaultPages(std::span<const std::uint64_t> pages) {
    if (pages.empty() || !ServesOwned()) return;
    auto& service = ResidencyService::Get();
    if (!service.Attached()) return;
    struct FaultPage {
        std::uint64_t page = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
    };
    std::vector<FaultPage> found;
    found.reserve(pages.size());
    {
        const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        for (const auto address : pages) {
            const auto page = address & ~(GpuPageBytes - 1);
            FaultPage entry{page, page, page + GpuPageBytes};
            auto range = std::upper_bound(lease.begin(), lease.end(), page, [](std::uint64_t at, const auto& candidate) { return at < candidate->address; });
            if (range != lease.begin()) {
                const auto& allocation = *--range;
                const auto first = allocation->address & ~(PageBytes - 1);
                const auto last = (allocation->address + allocation->bytes + PageBytes - 1) & ~(PageBytes - 1);
                if (allocation->bytes != 0 && first <= page && page + GpuPageBytes <= last) {
                    entry.first = first;
                    entry.last = last;
                }
            }
            found.push_back(entry);
        }
    }
    auto& core = MemoryCore::Get();
    for (const auto& [page, first, last] : found) {
        if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(page), static_cast<std::size_t>(GpuPageBytes))) continue;
        const bool whole = registerRange(first, last);
        if (!whole && first != page) registerRange(page, page + GpuPageBytes);
        const auto committed = GuestMemory::CommittedRanges(page, static_cast<std::size_t>(GpuPageBytes));
        core.Reserve(page, GpuPageBytes);
        for (const auto& [from, to] : committed) core.MakeResident(from, to - from);
        if (whole) service.ServeFaultPage(page, first, last - first, committed);
        else service.ServeFaultPage(page, 0, 0, committed);
    }
}


void LandGpuWrites(GuestRanges ranges) {
    static const bool ahead = std::getenv("APS5_RECODE_NO_READBACK_AHEAD") == nullptr;
    if (!ahead || ranges.empty() || !ServesOwned()) return;
    std::unique_lock<GuestMemory::GpuMutexType> gpu(GuestMemory::GpuMutex(), std::defer_lock);
    if (!GuestMemory::GpuMutex().HeldByThisThread()) gpu.lock();
    const auto pieces = ResidencyService::Get().ReadBack(ranges);
    auto& core = MemoryCore::Get();
    for (const auto& piece : pieces) core.LandGpuWrite(piece.guest, piece.bytes, GuestMemory::OwnStoreTarget(piece.guest, piece.bytes.size()));
}

bool ResidentDrawInputs() {
    static const bool resident = [] {
        if (std::getenv("APS5_RECODE_IMPORTED_INPUTS") != nullptr) return false;
        GuestMemory::SetGpuWriteHook([](std::uint64_t address, std::uint64_t bytes) {
            const std::array written{std::pair{address, address + bytes}};
            NoteGpuWrites(written);
        });
        return true;
    }();
    return resident;
}

namespace {

constexpr std::size_t WrittenChunksPendingBound = std::size_t{1} << 16u;

struct WrittenChunks {
    std::mutex mutex;
    std::set<std::uint64_t> queued;
    std::vector<std::uint64_t> pending;
};

WrittenChunks& writtenChunks() {
    static auto& chunks = *new WrittenChunks();
    return chunks;
}

void queueWrittenChunks(std::uint64_t begin, std::uint64_t end) {
    auto& chunks = writtenChunks();
    std::lock_guard lock(chunks.mutex);
    for (auto chunk = begin & ~(ChunkBytes - 1); chunk < end; chunk += ChunkBytes) {
        if (chunks.pending.size() < WrittenChunksPendingBound && chunks.queued.insert(chunk).second) chunks.pending.push_back(chunk);
    }
}

}

std::vector<std::uint64_t> TakeGpuWrittenChunks() {
    auto& chunks = writtenChunks();
    std::lock_guard lock(chunks.mutex);
    std::vector<std::uint64_t> taken;
    taken.swap(chunks.pending);
    return taken;
}

void ForgetImport(std::uint64_t base, std::uint64_t bytes, std::uint64_t deviceBegin, std::uint64_t deviceEnd) {
    const auto forgotten = ResidencyService::Get().ForgetImport(base, bytes, deviceBegin, deviceEnd);
    if (forgotten.empty()) return;
    auto& chunks = writtenChunks();
    std::lock_guard lock(chunks.mutex);
    for (const auto chunk : forgotten) {
        if (chunks.pending.size() < WrittenChunksPendingBound) chunks.pending.push_back(chunk);
    }
}

void NoteGpuWrites(GuestRanges ranges) {
    if (Downgrades::Watching()) {
        for (const auto& [begin, end] : ranges) {
            if (end > begin) Downgrades::WatchTouch("a GPU write note", begin, end - begin);
        }
    }
    if (ranges.empty() || !ResidentDrawInputs() || !memoryInstalled()) return;
    static std::mutex markedMutex;
    static auto& marked = *new std::map<std::uint64_t, std::uint64_t>();
    std::lock_guard lock(markedMutex);
    for (auto [begin, end] : ranges) {
        if (end <= begin) continue;
        begin &= ~(PageBytes - 1);
        end = (end + PageBytes - 1) & ~(PageBytes - 1);
        queueWrittenChunks(begin, end);
        auto next = marked.upper_bound(begin);
        if (next != marked.begin()) {
            const auto previous = std::prev(next);
            if (previous->second >= end) continue;
            if (previous->second >= begin) {
                begin = previous->first;
                next = marked.erase(previous);
            }
        }
        while (next != marked.end() && next->first <= end) {
            end = std::max(end, next->second);
            next = marked.erase(next);
        }
        marked.emplace(begin, end);
        MemoryCore::Get().Pages().NoteGpuWritten(begin, end - begin);
    }
}

bool OwnedUploadsPending() {
    return ServesOwned() && ResidencyService::Get().Pending();
}

std::size_t RecordOwnedUploads(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept) {
    if (!ServesOwned()) return 0;
    const auto start = std::chrono::steady_clock::now();
    const auto recorded = ResidencyService::Get().Record(commands, kept);
    if (recorded != 0) {
        auto& totals = memoryTotals();
        totals.ownedRecords.fetch_add(1, std::memory_order_relaxed);
        totals.ownedRecordNanoseconds.fetch_add(nanosecondsSince(start), std::memory_order_relaxed);
    }
    return recorded;
}

std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission) {
    auto copy = std::make_shared<DriverDetail::Submission>();
    copy->serial = submission.serial;
    copy->queue = submission.queue;
    copy->commands = submission.commands;
    copy->conditionalEnds = submission.conditionalEnds;
    copy->registerLists = submission.registerLists;
    copy->shaders = submission.shaders;
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
        if (!submission.flips.empty() && memoryInstalled()) {
            MemoryCore::Get().AdvanceFrame();
            ResidencyService::Get().AdvanceFrame();
            memoryTotals().frames.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (decoding) front().Push(Copy(submission));
    report();
}

}
