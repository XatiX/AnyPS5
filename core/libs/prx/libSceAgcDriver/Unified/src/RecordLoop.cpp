#include "prx/libSceAgcDriver/Unified/include/RecordLoop.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/LabelTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Unified/include/Releases.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Loop {

namespace {

constexpr std::size_t GpuStoreLimit = 65536;
constexpr auto WaitReport = std::chrono::seconds(5);

struct Totals {
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> dispatches{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> labels{0};
    std::atomic<std::uint64_t> labelsAtOnce{0};
    std::atomic<std::uint64_t> interrupts{0};
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> waitsFromTable{0};
    std::atomic<std::uint64_t> waitsFromGuest{0};
    std::atomic<std::uint64_t> waitPolls{0};
    std::atomic<std::uint64_t> copies{0};
    std::atomic<std::uint64_t> copiesOnCpu{0};
    std::atomic<std::uint64_t> flips{0};
    std::atomic<std::uint64_t> predicated{0};
    std::atomic<std::uint64_t> idleReaps{0};
    std::atomic<std::uint64_t> runTimeStores{0};
    std::atomic<std::uint64_t> runTimeRefused{0};
    std::atomic<std::uint64_t> sampleDumps{0};
    std::atomic<std::uint64_t> seals{0};
    std::atomic<std::uint64_t> constantRamDumps{0};
    std::atomic<std::uint64_t> refreshedSegments{0};
    std::atomic<std::uint64_t> flipsReleased{0};
    std::atomic<std::uint64_t> copiedBytes{0};
    std::atomic<std::uint64_t> copiesOnGpu{0};
    std::atomic<std::uint64_t> copiesFromMirror{0};
    std::atomic<std::uint64_t> flipsReleasedByWorker{0};
    std::atomic<std::uint64_t> guarded{0};
    std::atomic<std::uint64_t> skipped{0};
    std::atomic<std::uint64_t> conditionsRead{0};
};

Totals& totals() {
    static Totals held;
    return held;
}

struct Progress {
    std::mutex mutex;
    std::condition_variable advanced;
    std::uint64_t submission = 0;
    std::uint32_t offset = 0;
    bool any = false;
    std::atomic<std::int64_t> busySince{0};
    std::atomic<std::uint64_t> busySubmission{0};
    std::atomic<std::uint32_t> busyOffset{0};
    std::atomic<std::uint32_t> busyOpcode{0};
    std::atomic<const char*> stage{"idle"};
};

struct Hook {
    InterruptHook hook = nullptr;
    void* owner = nullptr;
};

std::atomic<Hook*> interruptHook{nullptr};

std::mutex progressMutex;
std::map<std::uint32_t, std::unique_ptr<Progress>> progresses;

Progress& progressFor(std::uint32_t queue) {
    std::lock_guard lock(progressMutex);
    auto& held = progresses[queue];
    if (held == nullptr) held = std::make_unique<Progress>();
    return *held;
}

std::int64_t nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string stuckLine() {
    std::string line;
    const auto now = nowNanoseconds();
    std::lock_guard lock(progressMutex);
    for (const auto& [queue, progress] : progresses) {
        const auto since = progress->busySince.load(std::memory_order_acquire);
        if (since == 0 || now - since < 5'000'000'000ll) continue;
        char text[256];
        std::snprintf(text, sizeof(text), "\n[recode-loop] queue 0x%x stuck %.1f s in %s on submission %llu offset %u opcode 0x%x", queue, static_cast<double>(now - since) / 1e9, progress->stage.load(std::memory_order_relaxed), static_cast<unsigned long long>(progress->busySubmission.load(std::memory_order_relaxed)), progress->busyOffset.load(std::memory_order_relaxed), progress->busyOpcode.load(std::memory_order_relaxed));
        line += text;
    }
    return line;
}

bool reached(const Progress& progress, std::uint64_t submission, std::uint32_t offset) {
    return progress.any && (progress.submission > submission || (progress.submission == submission && progress.offset >= offset));
}

struct PendingLabels {
    std::mutex mutex;
    std::multimap<std::uint64_t, std::uint64_t> ranges;
};

PendingLabels& pendingLabels() {
    static PendingLabels held;
    return held;
}

void notePendingLabel(std::uint64_t address, std::uint64_t bytes) {
    auto& shared = pendingLabels();
    std::lock_guard lock(shared.mutex);
    shared.ranges.emplace(address, address + bytes);
}

void dropPendingLabel(std::uint64_t address, std::uint64_t bytes) {
    auto& shared = pendingLabels();
    std::lock_guard lock(shared.mutex);
    const auto [first, last] = shared.ranges.equal_range(address);
    for (auto it = first; it != last; ++it) {
        if (it->second != address + bytes) continue;
        shared.ranges.erase(it);
        return;
    }
}

bool LabelsPending(std::uint64_t address, std::uint64_t bytes) {
    auto& shared = pendingLabels();
    std::lock_guard lock(shared.mutex);
    for (auto it = shared.ranges.begin(); it != shared.ranges.end() && it->first < address + bytes; ++it) {
        if (it->second > address) return true;
    }
    return false;
}

using FlipKey = std::tuple<std::uint32_t, std::uint64_t, std::uint32_t>;

struct FlipHandOver {
    std::mutex mutex;
    std::map<FlipKey, std::function<void()>> handed;
    std::set<FlipKey> reached;
};

FlipHandOver& flipHandOver() {
    static FlipHandOver held;
    return held;
}

void runFlipRelease(const std::function<void()>& release) {
    try {
        release();
    } catch (const std::exception& error) {
        Record::RefuseUnrecorded("loop flip", 0, std::string("the flip release threw: ") + error.what());
    }
}

void deliverInterrupt(std::uint32_t queue, std::uint32_t context) {
    const auto* held = interruptHook.load(std::memory_order_acquire);
    if (held == nullptr || held->hook == nullptr) return;
    held->hook(held->owner, queue, context);
    totals().interrupts.fetch_add(1, std::memory_order_relaxed);
}

class Runner {
public:
    explicit Runner(std::uint32_t queue) : queue(queue) {}

    void Run() {
        StartLoopSampler(queue);
        auto& items = QueueFor(queue);
        auto& progress = progressFor(queue);
        busy = &progress;
        auto reported = std::chrono::steady_clock::now();
        for (;;) {
            stage("waiting for operations");
            auto item = items.Pop(std::chrono::milliseconds(1));
            if (std::chrono::steady_clock::now() - reported > std::chrono::seconds(10)) {
                reported = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[recode-loop] queue 0x%x: %s%s\n[recode-loop] queue 0x%x stages (10 s): %s\n", queue, Line().c_str(), stuckLine().c_str(), queue, stageLine().c_str());
            }
            if (!item) {
                stage("reaping while idle");
                settleIdle();
                continue;
            }
            stage("operation");
            lastWork = std::chrono::steady_clock::now();
            progress.busySubmission.store(item->operation.submission, std::memory_order_relaxed);
            progress.busyOffset.store(item->operation.packetOffset, std::memory_order_relaxed);
            progress.busyOpcode.store(item->operation.opcode, std::memory_order_relaxed);
            progress.busySince.store(nowNanoseconds(), std::memory_order_release);
            try {
                if (!item->guards.empty()) stage("checking guards");
                if (item->guards.empty() || guardsPass(*item)) handle(*item);
                else trace(*item, "skip");
                stage("finishing an operation");
            } catch (const std::exception& error) {
                Record::RefuseUnrecorded("loop operation", 0, std::string("record loop threw: ") + error.what());
                totals().refused.fetch_add(1, std::memory_order_relaxed);
            }
            progress.busySince.store(0, std::memory_order_release);
            {
                std::lock_guard lock(progress.mutex);
                progress.submission = item->operation.submission;
                progress.offset = item->operation.packetOffset;
                progress.any = true;
            }
            progress.advanced.notify_all();
        }
    }

private:
    void reportIdle() {
        const auto now = std::chrono::steady_clock::now();
        if (lastWork.time_since_epoch().count() == 0 || now - lastWork < std::chrono::seconds(5) || now - lastIdleReport < std::chrono::seconds(5)) return;
        lastIdleReport = now;
        std::string line;
        for (const auto& stored : recent) {
            if (stored.address == 0u) continue;
            std::uint64_t guest = 0;
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(stored.address)), stored.bytes)) std::memcpy(&guest, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(stored.address)), stored.bytes);
            char text[96];
            std::snprintf(text, sizeof(text), " 0x%llx stored 0x%llx guest 0x%llx;", static_cast<unsigned long long>(stored.address), static_cast<unsigned long long>(stored.value), static_cast<unsigned long long>(guest));
            line += text;
        }
        const auto device = Resolve::AttachedDevice();
        const auto* recorder = device != nullptr ? device->graphicsContext().recorder : nullptr;
        std::fprintf(stderr, "[recode-loop] queue 0x%x idle %.1f s, recorder %s; last label stores:%s\n%s\n", queue, std::chrono::duration<double>(now - lastWork).count(), recorder == nullptr ? "none" : recorder->Idle() ? "idle" : "busy", line.c_str(), Releases::Line().c_str());
    }

    void settleIdle() {
        reportIdle();
        const auto device = Resolve::AttachedDevice();
        if (device == nullptr) return;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
        std::unique_lock lock(GuestMemory::GpuMutex(), std::try_to_lock);
        if (!lock.owns_lock()) return;
        auto* recorder = device->graphicsContext().recorder;
        if (recorder == nullptr || recorder->Idle()) return;
        recorder->Submit();
        if (recorder->Reap()) totals().idleReaps.fetch_add(1, std::memory_order_relaxed);
    }

    void trace(const Item& item, const char* stage) const {
        static const bool traced = std::getenv("APS5_RECODE_TRACE_LOOP") != nullptr;
        static std::atomic<std::uint32_t> lines{0};
        if (!traced || lines.fetch_add(1, std::memory_order_relaxed) >= 400u) return;
        const auto& operation = item.operation;
        const auto select = item.words.size() >= 8u && operation.opcode == 0x49u ? (item.words[2] >> 24u) & 7u : 0u;
        const auto context = item.words.size() >= 8u && operation.opcode == 0x49u ? item.words[7] : 0u;
        std::fprintf(stderr, "[recode-loop-trace] %s runner 0x%x q%u kind %u submission %llu offset %u opcode 0x%x address 0x%llx size %llu value 0x%llx function %u words %zu interrupt %u context 0x%x\n", stage, queue, operation.queue, static_cast<unsigned>(operation.kind), static_cast<unsigned long long>(operation.submission), operation.packetOffset, operation.opcode, static_cast<unsigned long long>(operation.address), static_cast<unsigned long long>(operation.size), static_cast<unsigned long long>(operation.value), static_cast<unsigned>(operation.function), item.words.size(), select, context);
    }

    void handle(const Item& item) {
        trace(item, "take");
        if (item.operation.predicated && item.guards.empty()) totals().predicated.fetch_add(1, std::memory_order_relaxed);
        if (item.operation.kind == OpKind::Flip) {
            releaseFlip(item);
            return;
        }
        const auto device = Resolve::AttachedDevice();
        if (device == nullptr) {
            if (item.operation.kind != OpKind::Wait && item.operation.kind != OpKind::Flip && item.operation.kind != OpKind::Marker) Record::RefuseUnrecorded("loop operation", 0, "the recode has no device yet");
            return;
        }
        switch (item.operation.kind) {
            case OpKind::Draw:
            case OpKind::DrawIndirect:
                stage("recording a draw");
                draw(*device, item);
                return;
            case OpKind::Dispatch:
            case OpKind::DispatchIndirect:
                stage("recording a dispatch");
                dispatch(*device, item);
                return;
            case OpKind::LabelStore:
            case OpKind::Copy:
            case OpKind::Fill:
                stage("storing");
                store(*device, item);
                return;
            case OpKind::Wait:
                stage("waiting on memory");
                wait(*device, item);
                return;
            case OpKind::Flip: return;
            case OpKind::Marker:
                stage("sealing pass writes");
                sealPassWrites(*device);
                return;
        }
    }

    struct Decided {
        std::uint64_t submission = 0;
        std::size_t start = 0;
        bool condition = false;
        bool passes = true;
    };

    bool guardsPass(const Item& item) {
        totals().guarded.fetch_add(1, std::memory_order_relaxed);
        for (const auto& guard : item.guards) {
            const auto known = std::find_if(decided.begin(), decided.end(), [&](const Decided& entry) { return entry.submission == guard.submission && entry.start == guard.start && entry.condition == guard.condition; });
            const bool passes = known != decided.end() ? known->passes : evaluate(guard);
            if (known == decided.end()) decided[decidedNext++ % decided.size()] = {guard.submission, guard.start, guard.condition, passes};
            if (!passes) {
                totals().skipped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        return true;
    }

    void settle(VulkanDevice& device, const char* why = "settling recorded work") {
        stage(why);
        for (;;) {
            bool idle = true;
            {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
                std::lock_guard lock(GuestMemory::GpuMutex());
                auto* recorder = device.graphicsContext().recorder;
                if (recorder != nullptr && !recorder->Idle()) {
                    recorder->Submit();
                    static_cast<void>(recorder->Reap());
                    idle = recorder->Idle();
                }
            }
            if (idle) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        Record::DrainHostCopies();
    }

    bool evaluate(const OperationGuard& guard) {
        totals().conditionsRead.fetch_add(1, std::memory_order_relaxed);
        const auto conditionAddress = guard.condition ? (static_cast<std::uint64_t>(guard.words[1]) | (static_cast<std::uint64_t>(guard.words[2]) << 32u)) : guard.predication.address;
        const auto conditionBytes = guard.condition ? 4u : Pm4::PredicationBytes(guard.predication);
        if (const auto device = Resolve::AttachedDevice(); device != nullptr && (guard.predication.operation == 1u || MemoryCore::Get().GpuWritesPending(conditionAddress, conditionBytes) || LabelsPending(conditionAddress, conditionBytes))) settle(*device, guard.predication.operation == 1u ? "settling for a Z-pass condition" : MemoryCore::Get().GpuWritesPending(conditionAddress, conditionBytes) ? "settling for a GPU-written condition" : "settling for a label condition");
        stage("reading a condition");
        if (guard.condition) {
            const auto address = static_cast<std::uint64_t>(guard.words[1]) | (static_cast<std::uint64_t>(guard.words[2]) << 32u);
            MemoryCore::Get().FlushGpuWrites(address, 4u);
            const auto value = Pm4::ReadCondition(guard.words);
            traceGuard(guard, address, value, value != 0u);
            return value != 0u;
        }
        thread_local QueueState probe;
        probe.predication = guard.predication;
        probe.sampleDumpBlocks = guard.sampleDumpBlocks;
        MemoryCore::Get().FlushGpuWrites(guard.predication.address, Pm4::PredicationBytes(guard.predication));
        const bool passes = Pm4::PredicationPasses(probe);
        std::uint64_t first = 0;
        if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guard.predication.address)), 16u)) std::memcpy(&first, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guard.predication.address + 8u)), 8u);
        traceGuard(guard, guard.predication.address, first, passes);
        return passes;
    }

    void traceGuard(const OperationGuard& guard, std::uint64_t address, std::uint64_t value, bool passes) const {
        static const bool traced = std::getenv("APS5_RECODE_TRACE_LOOP") != nullptr;
        static std::atomic<std::uint32_t> lines{0};
        if (!traced || lines.fetch_add(1, std::memory_order_relaxed) >= 300u) return;
        std::fprintf(stderr, "[recode-loop-trace] guard q%u submission %llu range %zu-%zu %s operation %u blocks 0x%x when-set %d at 0x%llx value 0x%llx: %s\n", queue, static_cast<unsigned long long>(guard.submission), guard.start, guard.end, guard.condition ? "COND_EXEC" : "predication", guard.predication.operation, guard.sampleDumpBlocks, guard.predication.executeWhenSet ? 1 : 0, static_cast<unsigned long long>(address), static_cast<unsigned long long>(value), passes ? "runs" : "skips");
    }

    void releaseFlip(const Item& item) {
        stage("releasing a flip");
        totals().flips.fetch_add(1, std::memory_order_relaxed);
        auto& shared = flipHandOver();
        const FlipKey key{item.operation.queue, item.operation.submission, item.operation.packetOffset};
        std::function<void()> release;
        {
            std::lock_guard lock(shared.mutex);
            const auto found = shared.handed.find(key);
            if (found == shared.handed.end()) {
                shared.reached.insert(key);
                return;
            }
            release = std::move(found->second);
            shared.handed.erase(found);
        }
        totals().flipsReleased.fetch_add(1, std::memory_order_relaxed);
        runFlipRelease(release);
    }

    static std::uint64_t codeOf(const Item& item) {
        return item.resolved != nullptr && !item.resolved->stages.empty() ? item.resolved->stages.back().program.codeAddress : 0u;
    }

    void draw(VulkanDevice& device, const Item& item) {
        if (item.resolved == nullptr || !item.parameters) {
            Record::RefuseUnrecorded("draw", codeOf(item), item.resolved == nullptr ? "" : "draw parameters not decoded");
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
        std::string reason;
        stage("draw precheck");
        if (!Record::RecordPrecheck(*item.resolved, *item.parameters, reads, reason)) {
            Record::RefuseUnrecorded("draw", codeOf(item), reason);
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        stage("GPU mutex wait");
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard lock(GuestMemory::GpuMutex());
        stage("recording a draw");
        if (Record::RecordResolved(device.graphicsContext(), *item.resolved, *item.parameters, reason)) totals().draws.fetch_add(1, std::memory_order_relaxed);
        drawnSinceSeal = true;
    }

    void sealPassWrites(VulkanDevice& device) {
        if (queue != 0u || !std::exchange(drawnSinceSeal, false)) return;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
        std::lock_guard lock(GuestMemory::GpuMutex());
        device.SealPassWriteOnly();
        totals().seals.fetch_add(1, std::memory_order_relaxed);
    }

    void dispatch(VulkanDevice& device, const Item& item) {
        if (item.resolved == nullptr) {
            Record::RefuseUnrecorded("dispatch", 0, "");
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto context = device.graphicsContext();
        std::string reason;
        stage("dispatch precheck");
        if (!Record::RecordDispatchPrecheck(context, device, *item.resolved, reason)) {
            Record::RefuseUnrecorded("dispatch", codeOf(item), reason);
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        stage("GPU mutex wait");
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard lock(GuestMemory::GpuMutex());
        stage("recording a dispatch");
        if (Record::RecordDispatch(device.graphicsContext(), *item.resolved, item.groups, reason)) totals().dispatches.fetch_add(1, std::memory_order_relaxed);
    }

    void storeBytes(const Graphics::Context& context, std::uint64_t address, std::span<const std::byte> bytes, bool inStream = false) {
        const auto stamp = Labels::Table().Note(address, bytes, queue);
        if (bytes.size() >= 4u) {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data(), std::min<std::size_t>(bytes.size(), sizeof(value)));
            recent[recentNext++ % recent.size()] = {address, std::min<std::size_t>(bytes.size(), 8u), value};
        }
        totals().labels.fetch_add(1, std::memory_order_relaxed);
        static const bool mirrorLabels = std::getenv("APS5_RECODE_LOOP_MIRROR_LABELS") != nullptr;
        if ((mirrorLabels || inStream) && bytes.size() <= GpuStoreLimit && address % 4u == 0u && bytes.size() % 4u == 0u && !bytes.empty()) {
            if (Record::StoreToMirror(context, *context.recorder, address, bytes, stamp, queue) == 0) return;
            GuestMemory::Write(address, bytes, 4);
            totals().labelsAtOnce.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::byte> held(bytes.begin(), bytes.end());
        notePendingLabel(address, bytes.size());
        if (context.recorder->AfterRecordedWork([address, held] {
                Record::AfterLandings([address, held] {
                    GuestMemory::Write(address, held, 1);
                    dropPendingLabel(address, held.size());
                });
            })) return;
        dropPendingLabel(address, bytes.size());
        GuestMemory::Write(address, bytes, 1);
        totals().labelsAtOnce.fetch_add(1, std::memory_order_relaxed);
    }

    static std::optional<Pm4::MemoryCopy> memoryCopyOf(std::span<const std::uint32_t> words) {
        if (auto copy = Pm4::DecodeMemoryCopy(words)) return copy;
        if (words.size() != 6u || ((words[0] >> 8u) & 0xffu) != 0x40u || Pm4::CopyDataToRegister(words)) return std::nullopt;
        const auto source = Pm4::CopyDataSource(words);
        if (source == 0u || source == Pm4::CopyDataSelectGds || source == Pm4::CopyDataGpuClockSource || Pm4::CopyDataImmediate(source)) return std::nullopt;
        const auto from = static_cast<std::uint64_t>(words[2]) | (static_cast<std::uint64_t>(words[3]) << 32u);
        const auto to = static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u);
        const auto bytes = Pm4::CopyDataBytes(words);
        if (from == 0u || to == 0u || (from < to + bytes && to < from + bytes)) return std::nullopt;
        return Pm4::MemoryCopy{from, to, bytes};
    }

    static void writeCopy(std::uint64_t destination, std::span<const std::byte> bytes) {
        if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(destination)), bytes.size(), true)) {
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(destination)), bytes.data(), bytes.size());
            Shadow::NoteDriverStore(destination, bytes.size());
            return;
        }
        GuestMemory::Write(destination, bytes, 1);
    }

    void landNow(VulkanDevice& device, std::span<const std::uint32_t> words) {
        auto copy = memoryCopyOf(words);
        if (!copy && words.size() == 7u && ((words[0] >> 8u) & 0xffu) == 0x50u) {
            const auto end = [&](std::uint32_t select, std::uint32_t low, std::uint32_t high) -> std::optional<std::uint64_t> {
                if (select == 0u || select == 3u) return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
                if (select == Pm4::DmaSelectGds) return Pm4::GdsAddress() + low;
                return std::nullopt;
            };
            const auto from = end(Pm4::DmaSource(words), words[2], words[3]);
            const auto to = end(Pm4::DmaDestination(words), words[4], words[5]);
            const auto bytes = Pm4::DmaBytes(words);
            if (from && to && bytes != 0u && (*from + bytes <= *to || *to + bytes <= *from)) copy = Pm4::MemoryCopy{*from, *to, bytes};
        }
        if (copy && copy->bytes <= GpuStoreLimit && MemoryCore::Get().GpuWritesPending(copy->source, copy->bytes)) {
            const std::array source{DirtyRange{copy->source, copy->bytes}};
            std::shared_ptr<Graphics::Buffer> landing;
            if (MemoryCore::Get().WithoutGpuWritten(source).empty()) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
                std::lock_guard lock(GuestMemory::GpuMutex());
                if (auto* recorder = device.graphicsContext().recorder) landing = Record::ReadMirrorAfterBatch(device.graphicsContext(), *recorder, copy->source, copy->bytes);
            }
            if (landing != nullptr) {
                settle(device, "settling for a copy read from the mirror");
                stage("writing a copy destination");
                landing->Invalidate();
                writeCopy(copy->destination, std::span<const std::byte>(landing->Bytes().data(), static_cast<std::size_t>(copy->bytes)));
                totals().copiesFromMirror.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (copy) {
            if (MemoryCore::Get().GpuWritesPending(copy->source, copy->bytes)) settle(device, "settling for a copy source");
            stage("landing a copy source");
            MemoryCore::Get().FlushGpuWrites(copy->source, copy->bytes);
            stage("reading a copy source");
            totals().copiedBytes.fetch_add(copy->bytes, std::memory_order_relaxed);
            std::vector<std::byte> bytes(copy->bytes);
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(copy->source)), bytes.size())) std::memcpy(bytes.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(copy->source)), bytes.size());
            else GuestMemory::Read(copy->source, bytes, 1);
            stage("writing a copy destination");
            writeCopy(copy->destination, bytes);
            return;
        }
        const auto storeOpcode = (words[0] >> 8u) & 0xffu;
        const auto clock = storeOpcode == 0x40u && words.size() >= 2u && Pm4::CopyDataSource(words) == Pm4::CopyDataGpuClockSource;
        stage(storeOpcode == 0x50u ? "run-time DMA on the CPU" : clock ? "GPU clock store on the CPU" : storeOpcode == 0x40u ? "run-time COPY_DATA on the CPU" : "run-time store on the CPU");
        static const QueueState unwritten{};
        if (const auto resolved = Pm4::ResolveStore(words, unwritten, std::numeric_limits<std::size_t>::max())) {
            const auto bytes = resolved->Bytes();
            if (!bytes.empty()) GuestMemory::Write(resolved->address, bytes, 1);
            return;
        }
        totals().runTimeRefused.fetch_add(1, std::memory_order_relaxed);
        Record::RefuseUnrecorded("loop store", 0, "a store computed at run time has no CPU form (opcode 0x" + std::to_string((words[0] >> 8u) & 0xffu) + ")");
    }

    void store(VulkanDevice& device, const Item& item) {
        if (item.words.empty()) return;
        const auto opcode = (item.words[0] >> 8u) & 0xffu;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::unique_lock lock(GuestMemory::GpuMutex());
        const auto context = device.graphicsContext();
        if (opcode == 0x37u || opcode == 0x49u) {
            if (const auto label = Pm4::DecodeLabelWrite(item.words)) {
                storeBytes(context, label->address, label->Bytes());
                context.recorder->Submit();
            }
            if (opcode == 0x49u && item.words.size() >= 8u) {
                const auto select = (item.words[2] >> 24u) & 7u;
                if (select != 0u && select != 3u) {
                    const auto interrupt = item.words[7] & 0x0fffffffu;
                    const auto target = queue;
                    if (!context.recorder->AfterRecordedWork([target, interrupt] { Record::AfterLandings([target, interrupt] { deliverInterrupt(target, interrupt); }); })) deliverInterrupt(target, interrupt);
                }
            }
            return;
        }
        if (opcode == 0x83u) {
            const auto dump = Pm4::DecodeConstantRamDump(item.words);
            if (dump.words == 0u) return;
            totals().constantRamDumps.fetch_add(1, std::memory_order_relaxed);
            if (item.payload.size() != static_cast<std::size_t>(dump.words) * 4u) {
                totals().refused.fetch_add(1, std::memory_order_relaxed);
                Record::RefuseUnrecorded("loop constant RAM dump", 0, "the dump's constant RAM words were not resolved at decode");
                return;
            }
            storeBytes(context, dump.destination, item.payload, true);
            return;
        }
        if (opcode == 0x46u) {
            if (!Pm4::SampleDump(item.words)) return;
            totals().sampleDumps.fetch_add(1, std::memory_order_relaxed);
            if (Record::DumpSamplesToMirror(context, *context.recorder, Pm4::SampleDumpAddress(item.words), Pm4::SampleDumpBytes, item.sampleDumpBlocks)) return;
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            Record::RefuseUnrecorded("loop sample dump", 0, "the occlusion counter dump was not recorded");
            return;
        }
        const auto pieces = Pm4::DecodeStores(item.words);
        bool onCpu = pieces.empty() && opcode == 0x50u && item.words.size() == 7u && Pm4::DmaDestination(item.words) == Pm4::DmaSelectGds;
        if (onCpu) totals().runTimeStores.fetch_add(1, std::memory_order_relaxed);
        for (const auto& piece : pieces) {
            if (piece.value == Pm4::PacketStore::Value::Bytes) {
                storeBytes(context, piece.address, piece.data);
                continue;
            }
            if (piece.value == Pm4::PacketStore::Value::Pattern) {
                std::vector<std::byte> filled(static_cast<std::size_t>(piece.bytes));
                for (std::size_t at = 0; at < filled.size() && !piece.data.empty(); at += piece.data.size()) std::memcpy(filled.data() + at, piece.data.data(), std::min(piece.data.size(), filled.size() - at));
                storeBytes(context, piece.address, filled);
                continue;
            }
            const auto copy = memoryCopyOf(item.words);
            if (copy) {
                totals().copies.fetch_add(1, std::memory_order_relaxed);
                if (Record::StoresToMirror(copy->bytes, copy->destination) && Record::CopyInMirror(context, *context.recorder, *copy)) {
                    totals().copiesOnGpu.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                totals().copiesOnCpu.fetch_add(1, std::memory_order_relaxed);
            } else {
                totals().runTimeStores.fetch_add(1, std::memory_order_relaxed);
            }
            onCpu = true;
        }
        lock.unlock();
        if (onCpu) landNow(device, item.words);
    }

    void wait(VulkanDevice& device, const Item& item) {
        const auto& operation = item.operation;
        if (operation.address == 0u || (operation.size != 4u && operation.size != 8u) || operation.function == 0u) return;
        totals().waits.fetch_add(1, std::memory_order_relaxed);
        stage("waiting on memory");
        const auto compares = [&](std::uint64_t value) { return Pm4::WaitCompares(operation.function, operation.value, operation.mask, value); };
        const auto started = std::chrono::steady_clock::now();
        auto warned = started;
        for (;;) {
            if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(operation.address)), static_cast<std::size_t>(operation.size)) && compares(Pm4::LoadLabel(operation.address, static_cast<std::size_t>(operation.size)))) {
                totals().waitsFromGuest.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (Labels::Table().IssuedAfter(operation.address, static_cast<std::size_t>(operation.size), 0u, compares)) {
                totals().waitsFromTable.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            totals().waitPolls.fetch_add(1, std::memory_order_relaxed);
            {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Wait);
                std::lock_guard lock(GuestMemory::GpuMutex());
                auto* recorder = device.graphicsContext().recorder;
                if (recorder != nullptr) {
                    recorder->Submit();
                    static_cast<void>(recorder->Reap());
                }
            }
            if (const auto now = std::chrono::steady_clock::now(); now - warned > WaitReport) {
                warned = now;
                std::fprintf(stderr, "[recode-loop] queue 0x%x waits %.1f s at 0x%llx (function %u ref 0x%llx mask 0x%llx)\n", queue, std::chrono::duration<double>(now - started).count(), static_cast<unsigned long long>(operation.address), static_cast<unsigned>(operation.function), static_cast<unsigned long long>(operation.value), static_cast<unsigned long long>(operation.mask));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    struct Stored {
        std::uint64_t address = 0;
        std::size_t bytes = 0;
        std::uint64_t value = 0;
    };

    void stage(const char* name) {
        if (busy != nullptr) busy->stage.store(name, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const auto spent = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - stageSince).count());
        stageSince = now;
        const auto found = std::find_if(stageTimes.begin(), stageTimes.end(), [&](const auto& entry) { return entry.first == stageName; });
        if (found != stageTimes.end()) found->second += spent;
        else stageTimes.emplace_back(stageName, spent);
        stageName = name;
    }

    std::string stageLine() {
        stage(stageName);
        std::uint64_t total = 0;
        for (const auto& entry : stageTimes) total += entry.second;
        std::string line;
        char text[128];
        for (const auto& [name, spent] : stageTimes) {
            if (spent * 1000u < total) continue;
            std::snprintf(text, sizeof(text), "%s%s %.0f ms", line.empty() ? "" : ", ", name, static_cast<double>(spent) / 1e6);
            line += text;
        }
        stageTimes.clear();
        return line;
    }

    std::uint32_t queue;
    Progress* busy = nullptr;
    bool drawnSinceSeal = false;
    const char* stageName = "idle";
    std::chrono::steady_clock::time_point stageSince = std::chrono::steady_clock::now();
    std::vector<std::pair<const char*, std::uint64_t>> stageTimes;
    std::array<Decided, 8> decided{};
    std::size_t decidedNext = 0;
    std::array<Stored, 6> recent{};
    std::size_t recentNext = 0;
    std::chrono::steady_clock::time_point lastWork{};
    std::chrono::steady_clock::time_point lastIdleReport{};
};

std::mutex runnersMutex;
std::map<std::uint32_t, std::thread> runners;

}

void SetInterruptHook(InterruptHook hook, void* owner) {
    static Hook held;
    held.hook = hook;
    held.owner = owner;
    interruptHook.store(&held, std::memory_order_release);
}

void TraceKept(std::span<const std::uint32_t> packet, std::uint32_t opcode, std::uint32_t queue, std::uint64_t submission, std::size_t offset, bool flip) {
    static const bool traced = std::getenv("APS5_RECODE_TRACE_LOOP") != nullptr;
    static std::atomic<std::uint32_t> lines{0};
    if (!traced || packet.empty() || lines.fetch_add(1, std::memory_order_relaxed) >= 400u) return;
    const auto word = [&](std::size_t at) { return at < packet.size() ? packet[at] : 0u; };
    std::fprintf(stderr, "[recode-loop-trace] kept q%u submission %llu offset %zu opcode 0x%x%s words %zu: %08x %08x %08x %08x %08x %08x %08x\n", queue, static_cast<unsigned long long>(submission), offset, opcode, flip ? " flip" : "", packet.size(), word(1), word(2), word(3), word(4), word(5), word(6), word(7));
}

void TraceInterrupt(std::uint32_t queue, std::uint32_t context) {
    static const bool traced = std::getenv("APS5_RECODE_TRACE_LOOP") != nullptr;
    static const auto started = std::chrono::steady_clock::now();
    if (traced) std::fprintf(stderr, "[recode-loop-trace] interrupt queue 0x%x context 0x%x at %.3f s\n", queue, context, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

void HandFlip(std::uint32_t queue, std::uint64_t submission, std::uint32_t packetOffset, std::function<void()> release) {
    auto& shared = flipHandOver();
    const FlipKey key{queue, submission, packetOffset};
    {
        std::lock_guard lock(shared.mutex);
        if (shared.reached.erase(key) == 0u) {
            shared.handed.emplace(key, std::move(release));
            return;
        }
    }
    totals().flipsReleasedByWorker.fetch_add(1, std::memory_order_relaxed);
    runFlipRelease(release);
}

void NoteRefreshedSegment(std::uint32_t queue, std::uint64_t submission, std::size_t offset) {
    const auto seen = totals().refreshedSegments.fetch_add(1, std::memory_order_relaxed);
    if (seen < 8u) std::fprintf(stderr, "[recode-segments] queue 0x%x submission %llu: the indirect buffer at offset %zu changed after its submission was copied and is read again where it runs%s\n", queue, static_cast<unsigned long long>(submission), offset, OwnsLoop() ? "; the record loop ran the copy decoded at submission" : "");
}

void Ensure(std::uint32_t queue) {
    std::lock_guard lock(runnersMutex);
    if (runners.contains(queue)) return;
    runners.emplace(queue, std::thread([queue] { Runner(queue).Run(); }));
    runners[queue].detach();
}

bool Takes(std::span<const std::uint32_t> packet, std::uint32_t opcode, std::uint32_t queue) {
    if (!OwnsLoop() || packet.empty()) return false;
    try {
        if (Pm4::DrawOpcode(opcode)) return true;
        switch (opcode) {
            case 0x15: case 0x16: return Resolve::DispatchesRoutedOn(queue);
            case 0x3c: case 0x93:
                static_cast<void>(Pm4::DecodeWait(packet));
                return true;
            case 0x37:
                if (Pm4::CopyDataToRegister(packet)) return false;
                return Pm4::DecodeLabelWrite(packet).has_value();
            case 0x49: return Pm4::DecodeLabelWrite(packet).has_value();
            case 0x40:
                if (packet.size() >= 6u && Pm4::CopyDataToRegister(packet)) return false;
                return !Pm4::DecodeStores(packet).empty();
            case 0x50: case 0x83: return true;
            case 0x46: case 0x58: return true;
            default: return false;
        }
    } catch (const std::exception&) {
        return false;
    }
}

bool Passed(std::uint32_t queue, std::uint64_t submission, std::uint32_t packetOffset) {
    auto& progress = progressFor(queue);
    std::lock_guard lock(progress.mutex);
    return reached(progress, submission, packetOffset);
}

bool WaitPassed(std::uint32_t queue, std::uint64_t submission, std::uint32_t packetOffset, std::chrono::milliseconds limit) {
    auto& progress = progressFor(queue);
    std::unique_lock lock(progress.mutex);
    return progress.advanced.wait_for(lock, limit, [&] { return reached(progress, submission, packetOffset); });
}

LoopCounts Counts() {
    const auto& held = totals();
    return {held.draws.load(), held.dispatches.load(), held.refused.load(), held.labels.load(), held.labelsAtOnce.load(), held.interrupts.load(), held.waits.load(), held.waitsFromTable.load(), held.waitsFromGuest.load(), held.waitPolls.load(), held.copies.load(), held.copiesOnCpu.load(), held.flips.load()};
}

std::string Line() {
    const auto counts = Counts();
    char text[1024];
    std::snprintf(text, sizeof(text), "draws %llu, dispatches %llu, refused %llu; label stores %llu (%llu at once on the CPU), interrupts %llu; waits %llu (%llu from the label table, %llu from guest memory, %llu polls); copies %llu (%llu on the CPU in stream order, %.1f MiB; %llu in the mirror; %llu read from the mirror after their batch); run-time stores %llu (%llu refused); occlusion dumps %llu; constant RAM dumps %llu; indirect buffers changed after decode %llu; pass write seals %llu; flips %llu (%llu released by the loop, %llu by the worker); guarded operations %llu (%llu skipped, %llu conditions read); predicated operations with no guard %llu; idle reaps %llu",
                  static_cast<unsigned long long>(counts.draws), static_cast<unsigned long long>(counts.dispatches), static_cast<unsigned long long>(counts.refused), static_cast<unsigned long long>(counts.labels), static_cast<unsigned long long>(counts.labelsAtOnce), static_cast<unsigned long long>(counts.interrupts), static_cast<unsigned long long>(counts.waits), static_cast<unsigned long long>(counts.waitsFromTable), static_cast<unsigned long long>(counts.waitsFromGuest), static_cast<unsigned long long>(counts.waitPolls), static_cast<unsigned long long>(counts.copies), static_cast<unsigned long long>(counts.copiesOnCpu), static_cast<double>(totals().copiedBytes.load()) / 1048576.0, static_cast<unsigned long long>(totals().copiesOnGpu.load()), static_cast<unsigned long long>(totals().copiesFromMirror.load()),static_cast<unsigned long long>(totals().runTimeStores.load()), static_cast<unsigned long long>(totals().runTimeRefused.load()), static_cast<unsigned long long>(totals().sampleDumps.load()), static_cast<unsigned long long>(totals().constantRamDumps.load()), static_cast<unsigned long long>(totals().refreshedSegments.load()), static_cast<unsigned long long>(totals().seals.load()), static_cast<unsigned long long>(counts.flips), static_cast<unsigned long long>(totals().flipsReleased.load()), static_cast<unsigned long long>(totals().flipsReleasedByWorker.load()), static_cast<unsigned long long>(totals().guarded.load()), static_cast<unsigned long long>(totals().skipped.load()), static_cast<unsigned long long>(totals().conditionsRead.load()), static_cast<unsigned long long>(totals().predicated.load()), static_cast<unsigned long long>(totals().idleReaps.load()));
    return text;
}

}
