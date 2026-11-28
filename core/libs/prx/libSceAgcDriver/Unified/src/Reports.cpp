#include "prx/libSceAgcDriver/Unified/include/Reports.hpp"
#include "prx/libSceAgcDriver/Unified/include/Intake.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteMemory.hpp"
#include "prx/libSceAgcDriver/Unified/include/RouteDefaults.hpp"
#include "prx/libSceAgcDriver/Unified/include/Downgrades.hpp"
#include "prx/libSceAgcDriver/Unified/include/FaultSites.hpp"
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
#include <string_view>
#include <array>

namespace AgcDriver::Unified::Reports {

namespace {

std::string frameLine() {
    struct Sample {
        std::uint64_t frames = 0;
        std::uint64_t writeFaults = 0;
        std::uint64_t lockFreeWrites = 0;
        std::uint64_t ownedFaults = 0;
        std::uint64_t parked = 0;
        std::uint64_t bypassed = 0;
        std::uint64_t protectCalls = 0;
        std::uint64_t faultCalls = 0;
        std::uint64_t faultNanoseconds = 0;
        std::uint64_t lockNanoseconds = 0;
        std::uint64_t tableNanoseconds = 0;
        std::uint64_t protectNanoseconds = 0;
        std::uint64_t readBackBytes = 0;
        std::uint64_t landedBytes = 0;
    };
    static std::mutex sampleMutex;
    static Sample last;
    const auto core = MemoryCore::Get().Counts();
    Sample now;
    now.frames = RouteMemory::Counts().frames.load(std::memory_order_relaxed);
    now.writeFaults = core.writesOpened;
    now.lockFreeWrites = core.lockFreeWrites;
    now.ownedFaults = core.gpuOwnedFaults;
    now.parked = core.faultsParked;
    now.bypassed = core.faultsBypassed;
    now.protectCalls = MemoryCore::Get().Protection().Counts().calls;
    now.faultCalls = core.faultCalls;
    now.faultNanoseconds = core.faultNanoseconds;
    now.lockNanoseconds = core.faultLockNanoseconds;
    now.tableNanoseconds = core.faultTableNanoseconds;
    now.protectNanoseconds = core.faultProtectNanoseconds;
    now.readBackBytes = ResidencyService::Get().ReadBackBytes();
    now.landedBytes = core.ownedLandedBytes + core.diffLandedBytes + core.landedBytes;
    std::lock_guard lock(sampleMutex);
    const auto frames = now.frames - last.frames;
    const auto each = [&](std::uint64_t current, std::uint64_t before) { return frames == 0u ? 0.0 : static_cast<double>(current - before) / static_cast<double>(frames); };
    char line[896];
    std::snprintf(line, sizeof(line), "[recode-frame] %llu frames; per frame: write faults %.1f (%.1f without the core's lock), faults on GPU-owned pages %.2f (parked %.2f, bypassed %.2f), protect calls %.1f, fault handler %.1f us in %.1f calls (waiting for the core's lock %.1f us, page table and change note %.1f us, protection call %.1f us), read back %.1f KiB, landed %.1f KiB\n",
        static_cast<unsigned long long>(frames), each(now.writeFaults, last.writeFaults), each(now.lockFreeWrites, last.lockFreeWrites), each(now.ownedFaults, last.ownedFaults), each(now.parked, last.parked), each(now.bypassed, last.bypassed), each(now.protectCalls, last.protectCalls),
        each(now.faultNanoseconds, last.faultNanoseconds) / 1000.0, each(now.faultCalls, last.faultCalls), each(now.lockNanoseconds, last.lockNanoseconds) / 1000.0, each(now.tableNanoseconds, last.tableNanoseconds) / 1000.0, each(now.protectNanoseconds, last.protectNanoseconds) / 1000.0, each(now.readBackBytes, last.readBackBytes) / 1024.0, each(now.landedBytes, last.landedBytes) / 1024.0);
    static PageTableCounts lastTable;
    const auto table = MemoryCore::Get().Pages().Counts();
    char clusters[448];
    std::snprintf(clusters, sizeof(clusters), "[recode-frame] write faults next to a page opened since the last consume %.1f per frame, first in their 64 KiB chunk %.1f per frame; faults that opened more pages %.1f (%.1f of them over an image source), pages opened without their own fault %.1f, found unchanged at consume %.1f, changed %.1f\n",
        each(table.adjacentFaults, lastTable.adjacentFaults), each(table.chunkFirstFaults, lastTable.chunkFirstFaults), each(table.grownFaults, lastTable.grownFaults), each(table.wholeFaults, lastTable.wholeFaults), each(table.grownPages, lastTable.grownPages), each(table.unprovenUnchanged, lastTable.unprovenUnchanged), each(table.unprovenChanged, lastTable.unprovenChanged));
    lastTable = table;
    last = now;
    return std::string(line) + clusters;
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
    const auto retired = surfaces != nullptr ? surfaces->RetiredAlive() : std::pair<std::uint64_t, std::uint64_t>{};
    char line[512];
    std::snprintf(line, sizeof(line), "[recode-footprint] GPU-visible guest memory %llu MiB in %llu ranges; registered %llu MiB; mirror bound %llu MiB; image records %llu MiB in %llu records, retired but alive %llu MiB in %llu; depth records %llu\n",
        static_cast<unsigned long long>(gpuVisible >> 20), static_cast<unsigned long long>(gpuRanges), static_cast<unsigned long long>(RouteMemory::Counts().registeredBytes.load(std::memory_order_relaxed) >> 20),
        static_cast<unsigned long long>(ResidencyService::Get().BoundBytes() >> 20), static_cast<unsigned long long>(surfaces != nullptr ? surfaces->HeldBytes() >> 20 : 0u), static_cast<unsigned long long>(surfaces != nullptr ? surfaces->Size() : 0u),
        static_cast<unsigned long long>(retired.second >> 20), static_cast<unsigned long long>(retired.first), static_cast<unsigned long long>(Images::Depths().Size()));
    return line;
}

std::string memoryLine() {
    if (!RouteMemory::Active() || RouteMemory::Refused()) return {};
    auto& totals = RouteMemory::Counts();
    const auto core = MemoryCore::Get().Counts();
    const auto table = MemoryCore::Get().Pages().Counts();
    const auto protect = MemoryCore::Get().Protection().Counts();
    const auto consumes = totals.consumes.load(std::memory_order_relaxed);
    const auto nanoseconds = totals.consumeNanoseconds.load(std::memory_order_relaxed);
    char line[1024];
    std::snprintf(line, sizeof(line), "[recode-memory] registered %llu ranges %llu MiB, %llu outside the arena; backend accesses %llu, unknown memory %llu; consumes %llu (%llu submissions without device work not consumed), dirty ranges %llu, dirty %llu MiB, consume %.1f us avg %.1f us max; write faults opened %llu, host writes opened %llu, driver pages settled %llu, stale %llu, gpu-owned %llu, foreign %llu; kept busy %llu (%llu by an operation's consume), rearmed %llu, busy unchanged %llu, changed while rearming %llu; protect calls %llu (%llu MiB), split %llu, skipped %llu MiB, failures %llu\n",
        static_cast<unsigned long long>(totals.registrations.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.registeredBytes.load(std::memory_order_relaxed) >> 20), static_cast<unsigned long long>(totals.outsideArena.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(totals.accesses.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.unknownAccesses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(consumes), static_cast<unsigned long long>(totals.consumesSkipped.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.ranges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(totals.dirtyBytes.load(std::memory_order_relaxed) >> 20),
        consumes == 0 ? 0.0 : static_cast<double>(nanoseconds) / static_cast<double>(consumes) / 1000.0, static_cast<double>(totals.maxConsumeNanoseconds.exchange(0, std::memory_order_relaxed)) / 1000.0,
        static_cast<unsigned long long>(core.writesOpened), static_cast<unsigned long long>(core.hostWritesOpened), static_cast<unsigned long long>(core.driverPagesSettled), static_cast<unsigned long long>(core.staleFaults), static_cast<unsigned long long>(core.gpuOwnedFaults), static_cast<unsigned long long>(core.foreignFaults),
        static_cast<unsigned long long>(table.keptBusy), static_cast<unsigned long long>(table.rangedBusy), static_cast<unsigned long long>(table.rearmed), static_cast<unsigned long long>(table.unchangedBusy), static_cast<unsigned long long>(core.rearmedChanged),
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
    owned += RouteMemory::WriteAuditLine();
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
    return std::string(line) + eventLine + ResidencyService::Get().Line() + owned + UploadClash::Line() + MetadataWatch::Line() + frameLine() + footprintLine() + FaultSites::Line();
}

}

void Report() {
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
    if (!Intake::Active()) {
        const auto memory = memoryLine() + Record::VerifyLine() + Record::HandoffLine() + Proof::VerdictLine();
        if (!memory.empty()) std::fputs(memory.c_str(), stderr);
        return;
    }
    std::string line = "[recode-shadow]";
    std::uint64_t draws = 0, conditionalDraws = 0, dispatches = 0, conditionalDispatches = 0;
    for (const auto& lane : Intake::Lanes().Totals(false)) {
        draws += lane.counts.draws;
        conditionalDraws += lane.counts.conditionalDraws;
        dispatches += lane.counts.dispatches;
        conditionalDispatches += lane.counts.conditionalDispatches;
        char part[384];
        std::snprintf(part, sizeof(part), " q0x%x: %llu subs %llu ops %llu draws %llu dispatches %llu flips %.2f us/op stalls %llu memstate %llu errors %llu resolved %llu resolve errors %llu;", lane.queue, static_cast<unsigned long long>(lane.submissions), static_cast<unsigned long long>(lane.counts.operations), static_cast<unsigned long long>(lane.counts.draws), static_cast<unsigned long long>(lane.counts.dispatches), static_cast<unsigned long long>(lane.counts.flips), lane.NanosecondsPerOperation() / 1000.0, static_cast<unsigned long long>(lane.counts.snapshotStalls), static_cast<unsigned long long>(lane.counts.stateFromMemory), static_cast<unsigned long long>(lane.counts.stateErrors), static_cast<unsigned long long>(lane.counts.resolved), static_cast<unsigned long long>(lane.counts.resolveErrors));
        line += part;
    }
    const auto order = Intake::Lanes().Ordering().Counts();
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
