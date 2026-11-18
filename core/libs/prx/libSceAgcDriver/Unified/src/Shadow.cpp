#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace AgcDriver::Unified::Shadow {

namespace {

Front& front() {
    static Front& shared = *new Front();
    return shared;
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
    std::string line = "[recode-shadow]";
    std::uint64_t draws = 0, conditionalDraws = 0, dispatches = 0, conditionalDispatches = 0;
    for (const auto& lane : front().Totals(false)) {
        draws += lane.counts.draws;
        conditionalDraws += lane.counts.conditionalDraws;
        dispatches += lane.counts.dispatches;
        conditionalDispatches += lane.counts.conditionalDispatches;
        char part[256];
        std::snprintf(part, sizeof(part), " q0x%x: %llu subs %llu ops %llu draws %llu dispatches %llu flips %.2f us/op stalls %llu memstate %llu errors %llu;", lane.queue, static_cast<unsigned long long>(lane.submissions), static_cast<unsigned long long>(lane.counts.operations), static_cast<unsigned long long>(lane.counts.draws), static_cast<unsigned long long>(lane.counts.dispatches), static_cast<unsigned long long>(lane.counts.flips), lane.NanosecondsPerOperation() / 1000.0, static_cast<unsigned long long>(lane.counts.snapshotStalls), static_cast<unsigned long long>(lane.counts.stateFromMemory), static_cast<unsigned long long>(lane.counts.stateErrors));
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
    std::fputs(line.c_str(), stderr);
}

}

bool Active() {
    static const bool active = std::getenv("APS5_RECODE_SHADOW") != nullptr;
    return active;
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
    if (submission.suspend || submission.commands.empty()) return;
    front().Push(Copy(submission));
    report();
}

}
