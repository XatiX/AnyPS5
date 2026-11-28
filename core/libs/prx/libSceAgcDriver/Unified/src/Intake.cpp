#include "prx/libSceAgcDriver/Unified/include/Intake.hpp"
#include "prx/libSceAgcDriver/Unified/include/Reports.hpp"
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

namespace AgcDriver::Unified::Intake {

namespace {

void consumeDecoded(const Operation& operation, const DecodeState& state) {
    if (Record::VerifyActive()) Record::SharedVerifier().NoteDecoded(operation, state);
    if (Record::DrawActive()) Record::SharedHandoff().Offer(operation, state);
    if (Resolve::Active()) Resolve::Consume(operation, state);
}

bool recording() {
    return Record::VerifyActive() || Record::DrawActive() || Resolve::Active();
}

bool recordsDeviceWork(const DriverDetail::Submission& submission) {
    static const bool always = std::getenv("APS5_RECODE_CONSUME_EVERY_SUBMISSION") != nullptr;
    if (always || !submission.flips.empty()) return true;
    const auto& commands = submission.commands;
    for (std::size_t cursor = 0; cursor < commands.size();) {
        const auto header = commands[cursor];
        if (Pm4::FillerPacket(header)) {
            ++cursor;
            continue;
        }
        const auto count = Pm4::PacketWords(header);
        if (count > commands.size() - cursor) return true;
        if ((header >> 30u) == 3u) {
            const auto opcode = (header >> 8u) & 0xffu;
            if (Pm4::DrawOpcode(opcode) || opcode == 0x15u || opcode == 0x16u || opcode == 0x40u || opcode == 0x50u || opcode == 0x83u || opcode == 0x46u) return true;
        }
        cursor += count;
    }
    return false;
}

}

Front& Lanes() {
    static Front& shared = *new Front(recording() ? Front::Consumer(consumeDecoded) : Front::Consumer(), std::getenv("APS5_RECODE_RESOLVE") != nullptr || RouteMemory::DrawScope() || recording(), RouteMemory::DrawScope() ? UseSink(RouteMemory::RegisterUses) : UseSink());
    return shared;
}

bool Active() {
    static const bool active = std::getenv("APS5_RECODE_SHADOW") != nullptr;
    return active;
}

bool Takes() {
    static const bool teeing = Active() || RouteMemory::Active() || recording();
    return teeing;
}

std::shared_ptr<QueueState> RecordedState(const DriverDetail::Submission& submission, std::size_t packetOffset) {
    if (!Record::DrawActive()) return nullptr;
    return Record::SharedHandoff().Take(submission.queue, submission.serial, packetOffset, std::chrono::milliseconds(100));
}

void AttachDevice(const std::shared_ptr<VulkanDevice>& device) {
    Resolve::Attach(device);
    Mirror::Attach(device);
}

void NoteDriverDraw(const QueueState& queue, const DriverDetail::Submission& submission, std::size_t packetOffset) {
    if (!Record::VerifyActive()) return;
    Record::SharedVerifier().NoteDriverDraw(queue, submission.queue, submission.serial, packetOffset);
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
    for (const auto& [cursor, wait] : submission.renderingWaits) copy->renderingWaits.emplace(cursor, wait);
    return copy;
}

void Submit(const DriverDetail::Submission& submission) {
    const bool decoding = Active() || RouteMemory::DrawScope() || recording();
    if (submission.suspend && decoding) Lanes().NoteSuspend();
    if (submission.suspend || submission.commands.empty()) return;
    if (RouteMemory::Active() && !recordsDeviceWork(submission)) {
        RouteMemory::Counts().consumesSkipped.fetch_add(1, std::memory_order_relaxed);
    } else if (RouteMemory::Active()) {
        RouteMemory::ConsumeNow();
        if (!submission.flips.empty()) RouteMemory::AdvanceFrame();
    }
    if (decoding) Lanes().Push(Copy(submission));
    Reports::Report();
}

}
