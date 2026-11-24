#include "prx/libSceAgcDriver/Unified/include/RecordLoop.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/LabelTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/RecordQueue.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
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

bool reached(const Progress& progress, std::uint64_t submission, std::uint32_t offset) {
    return progress.any && (progress.submission > submission || (progress.submission == submission && progress.offset >= offset));
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
        auto& items = QueueFor(queue);
        auto reported = std::chrono::steady_clock::now();
        for (;;) {
            auto item = items.Pop(std::chrono::milliseconds(1));
            if (std::chrono::steady_clock::now() - reported > std::chrono::seconds(10)) {
                reported = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[recode-loop] queue 0x%x: %s\n", queue, Line().c_str());
            }
            if (!item) {
                settleIdle();
                continue;
            }
            lastWork = std::chrono::steady_clock::now();
            try {
                handle(*item);
            } catch (const std::exception& error) {
                Record::RefuseUnrecorded("loop operation", 0, std::string("record loop threw: ") + error.what());
                totals().refused.fetch_add(1, std::memory_order_relaxed);
            }
            auto& progress = progressFor(queue);
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
        std::fprintf(stderr, "[recode-loop] queue 0x%x idle %.1f s, recorder %s; last label stores:%s\n", queue, std::chrono::duration<double>(now - lastWork).count(), recorder == nullptr ? "none" : recorder->Idle() ? "idle" : "busy", line.c_str());
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
        std::fprintf(stderr, "[recode-loop-trace] %s runner 0x%x q%u kind %u submission %llu offset %u opcode 0x%x address 0x%llx size %llu value 0x%llx function %u words %zu\n", stage, queue, operation.queue, static_cast<unsigned>(operation.kind), static_cast<unsigned long long>(operation.submission), operation.packetOffset, operation.opcode, static_cast<unsigned long long>(operation.address), static_cast<unsigned long long>(operation.size), static_cast<unsigned long long>(operation.value), static_cast<unsigned>(operation.function), item.words.size());
    }

    void handle(const Item& item) {
        trace(item, "take");
        if (item.operation.predicated) totals().predicated.fetch_add(1, std::memory_order_relaxed);
        const auto device = Resolve::AttachedDevice();
        if (device == nullptr) {
            if (item.operation.kind != OpKind::Wait && item.operation.kind != OpKind::Flip) Record::RefuseUnrecorded("loop operation", 0, "the recode has no device yet");
            return;
        }
        switch (item.operation.kind) {
            case OpKind::Draw:
            case OpKind::DrawIndirect: draw(*device, item); return;
            case OpKind::Dispatch:
            case OpKind::DispatchIndirect: dispatch(*device, item); return;
            case OpKind::LabelStore:
            case OpKind::Copy:
            case OpKind::Fill: store(*device, item); return;
            case OpKind::Wait: wait(*device, item); return;
            case OpKind::Flip: totals().flips.fetch_add(1, std::memory_order_relaxed); return;
            case OpKind::Marker: return;
        }
    }

    static std::uint64_t codeOf(const Item& item) {
        return item.resolved != nullptr && !item.resolved->stages.empty() ? item.resolved->stages.back().program.codeAddress : 0u;
    }

    void draw(VulkanDevice& device, const Item& item) {
        if (item.resolved == nullptr || !item.parameters) {
            Record::RefuseUnrecorded("draw", codeOf(item), item.resolved == nullptr ? "not resolved" : "draw parameters not decoded");
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
        std::string reason;
        if (!Record::RecordPrecheck(*item.resolved, *item.parameters, reads, reason)) {
            Record::RefuseUnrecorded("draw", codeOf(item), reason);
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard lock(GuestMemory::GpuMutex());
        if (Record::RecordResolved(device.graphicsContext(), *item.resolved, *item.parameters, reason)) totals().draws.fetch_add(1, std::memory_order_relaxed);
    }

    void dispatch(VulkanDevice& device, const Item& item) {
        if (item.resolved == nullptr) {
            Record::RefuseUnrecorded("dispatch", 0, "not resolved");
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto context = device.graphicsContext();
        std::string reason;
        if (!Record::RecordDispatchPrecheck(context, device, *item.resolved, reason)) {
            Record::RefuseUnrecorded("dispatch", codeOf(item), reason);
            totals().refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard lock(GuestMemory::GpuMutex());
        if (Record::RecordDispatch(device.graphicsContext(), *item.resolved, item.groups, reason)) totals().dispatches.fetch_add(1, std::memory_order_relaxed);
    }

    void storeBytes(const Graphics::Context& context, std::uint64_t address, std::span<const std::byte> bytes) {
        const auto stamp = Labels::Table().Note(address, bytes, queue);
        if (bytes.size() >= 4u) {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data(), std::min<std::size_t>(bytes.size(), sizeof(value)));
            recent[recentNext++ % recent.size()] = {address, std::min<std::size_t>(bytes.size(), 8u), value};
        }
        totals().labels.fetch_add(1, std::memory_order_relaxed);
        static const bool mirrorLabels = std::getenv("APS5_RECODE_LOOP_MIRROR_LABELS") != nullptr;
        if (mirrorLabels && bytes.size() <= GpuStoreLimit && address % 4u == 0u && bytes.size() % 4u == 0u && !bytes.empty()) {
            if (Record::StoreToMirror(context, *context.recorder, address, bytes, stamp, queue) == 0) return;
            GuestMemory::Write(address, bytes, 4);
            totals().labelsAtOnce.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::byte> held(bytes.begin(), bytes.end());
        if (context.recorder->AfterRecordedWork([address, held] { GuestMemory::Write(address, held, 1); })) return;
        GuestMemory::Write(address, bytes, 1);
        totals().labelsAtOnce.fetch_add(1, std::memory_order_relaxed);
    }

    void store(VulkanDevice& device, const Item& item) {
        if (item.words.empty()) return;
        const auto opcode = (item.words[0] >> 8u) & 0xffu;
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Label);
        std::lock_guard lock(GuestMemory::GpuMutex());
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
                    if (!context.recorder->AfterRecordedWork([target, interrupt] { deliverInterrupt(target, interrupt); })) deliverInterrupt(target, interrupt);
                }
            }
            return;
        }
        const auto stores = Pm4::DecodeStores(item.words);
        for (const auto& piece : stores) {
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
            const auto copy = Pm4::DecodeMemoryCopy(item.words);
            if (!copy) continue;
            totals().copies.fetch_add(1, std::memory_order_relaxed);
            if (Record::CopyInMirror(context, *context.recorder, *copy)) continue;
            totals().copiesOnCpu.fetch_add(1, std::memory_order_relaxed);
            const auto moved = *copy;
            const auto land = [moved] {
                MemoryCore::Get().FlushGpuWrites(moved.source, moved.bytes);
                std::vector<std::byte> bytes(moved.bytes);
                std::memcpy(bytes.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(moved.source)), moved.bytes);
                GuestMemory::Write(moved.destination, bytes, 1);
            };
            if (!context.recorder->AfterRecordedWork(land)) land();
        }
    }

    void wait(VulkanDevice& device, const Item& item) {
        const auto& operation = item.operation;
        if (operation.address == 0u || (operation.size != 4u && operation.size != 8u) || operation.function == 0u) return;
        totals().waits.fetch_add(1, std::memory_order_relaxed);
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

    std::uint32_t queue;
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
            case 0x50: case 0x83: return !Pm4::DecodeStores(packet).empty();
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
    char text[400];
    std::snprintf(text, sizeof(text), "draws %llu, dispatches %llu, refused %llu; label stores %llu (%llu at once on the CPU), interrupts %llu; waits %llu (%llu from the label table, %llu from guest memory, %llu polls); copies %llu (%llu on the CPU after their batch); flips %llu; predicated operations recorded unconditionally %llu; idle reaps %llu",
                  static_cast<unsigned long long>(counts.draws), static_cast<unsigned long long>(counts.dispatches), static_cast<unsigned long long>(counts.refused), static_cast<unsigned long long>(counts.labels), static_cast<unsigned long long>(counts.labelsAtOnce), static_cast<unsigned long long>(counts.interrupts), static_cast<unsigned long long>(counts.waits), static_cast<unsigned long long>(counts.waitsFromTable), static_cast<unsigned long long>(counts.waitsFromGuest), static_cast<unsigned long long>(counts.waitPolls), static_cast<unsigned long long>(counts.copies), static_cast<unsigned long long>(counts.copiesOnCpu), static_cast<unsigned long long>(counts.flips), static_cast<unsigned long long>(totals().predicated.load()), static_cast<unsigned long long>(totals().idleReaps.load()));
    return text;
}

}
