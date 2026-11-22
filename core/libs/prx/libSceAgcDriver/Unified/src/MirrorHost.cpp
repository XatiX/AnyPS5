#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include <array>
#include <numeric>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace AgcDriver::Unified::Mirror {

namespace {

constexpr std::uint64_t WaitNanoseconds = 2'000'000'000ull;
constexpr int LockTries = 400;
constexpr int SubmitTries = 4000;

std::atomic<std::uint64_t> skippedUploadBytes{0};

class Host final : public MemoryHost {
public:
    void Attach(const std::shared_ptr<VulkanDevice>& next) {
        std::lock_guard lock(mutex);
        device = next;
    }

    std::uint64_t Completed() override {
        const auto held = Device();
        return held != nullptr ? held->CompletedEpoch() : 0u;
    }

    bool WaitFor(std::uint64_t serial) override {
        const auto held = Device();
        if (held == nullptr) return false;
        if (held->CompletedEpoch() >= serial) return true;
        auto& gpu = GuestMemory::GpuMutex();
        for (int tries = 0; tries < SubmitTries && held->SubmittedEpoch() < serial; ++tries) {
            if (gpu.try_lock()) {
                if (held->SubmittedEpoch() < serial) held->SubmitRecorded(false);
                gpu.unlock();
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        if (held->SubmittedEpoch() < serial) return false;
        return held->WaitRecordedFor(serial, WaitNanoseconds);
    }

    bool ReadMirror(std::uint64_t address, std::span<std::byte> out) override {
        std::unique_lock<GuestMemory::GpuMutexType> gpu(GuestMemory::GpuMutex(), std::defer_lock);
        if (!ResidencyService::Get().ReadsOffGpuMutex() && !GuestMemory::GpuMutex().HeldByThisThread()) {
            bool locked = false;
            for (int tries = 0; tries < LockTries && !locked; ++tries) {
                locked = gpu.try_lock();
                if (!locked) std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            if (!locked) {
                if (!lockTimeoutSeen.exchange(true)) std::fprintf(stderr, "[recode-gpu-owned] a mirror read gave up waiting %d ms for the GPU mutex at 0x%llx; the page opens with its old bytes\n", LockTries / 2, static_cast<unsigned long long>(address));
                return false;
            }
        }
        const std::array ranges{std::pair{address, static_cast<std::uint64_t>(out.size())}};
        const auto pieces = ResidencyService::Get().ReadBack(ranges);
        std::uint64_t covered = 0;
        for (const auto& piece : pieces) {
            if (piece.guest < address || piece.guest + piece.bytes.size() > address + out.size()) continue;
            std::memcpy(out.data() + (piece.guest - address), piece.bytes.data(), piece.bytes.size());
            covered += piece.bytes.size();
        }
        return covered == out.size();
    }

    std::byte* View(std::uint64_t address, std::size_t bytes) override {
        auto* view = GuestMemory::OwnStoreTarget(address, bytes);
        return view == reinterpret_cast<void*>(address) ? nullptr : static_cast<std::byte*>(view);
    }

    void Upload(std::uint64_t address, std::uint64_t size) override {
        const std::array ranges{DirtyRange{address, size}};
        const auto kept = MemoryCore::Get().WithoutGpuWritten(ranges);
        if (kept.size() != 1u || kept.front().size != size) skippedUploadBytes.fetch_add(size - std::accumulate(kept.begin(), kept.end(), std::uint64_t{0}, [](std::uint64_t sum, const DirtyRange& range) { return sum + range.size; }), std::memory_order_relaxed);
        if (!kept.empty()) ResidencyService::Get().NoteDirty(kept);
    }

    bool Bypass() override {
        return GuestMemory::GpuMutex().HeldByThisThread();
    }

    bool ImagePage(std::uint64_t page) override {
        const auto* table = Resolve::Surfaces();
        return table != nullptr && !table->StaleOverlapping(page, PageBytes).empty();
    }

private:
    std::shared_ptr<VulkanDevice> Device() {
        std::lock_guard lock(mutex);
        return device.lock();
    }

    std::mutex mutex;
    std::weak_ptr<VulkanDevice> device;
    std::atomic<bool> lockTimeoutSeen{false};
};

Host& host() {
    static auto& shared = *new Host();
    return shared;
}

}

std::uint64_t SkippedUploadBytes() {
    return skippedUploadBytes.load(std::memory_order_relaxed);
}

bool WritesEnabled() {
    static const bool enabled = ResidencyService::Enabled() && std::getenv("APS5_RECODE_MIRROR_WRITES") != nullptr;
    return enabled;
}

void Attach(const std::shared_ptr<VulkanDevice>& device) {
    if (!WritesEnabled()) return;
    static std::atomic<VulkanDevice*> attached{nullptr};
    if (attached.exchange(device.get(), std::memory_order_acq_rel) == device.get()) return;
    host().Attach(device);
    MemoryCore::Get().SetHost(&host());
    ResidencyService::Get().SetMirrorSpaces(true);
    ResidencyService::Get().SetBatchedBinds(std::getenv("APS5_RECODE_SYNC_BINDS") == nullptr);
    ResidencyService::Get().SetKeep([](std::uint64_t address, std::uint64_t size) { return MemoryCore::Get().Written().Any(address, size); });
    GuestMemory::SetMirrorFlushHook([](std::uint64_t address, std::size_t bytes) { MemoryCore::Get().FlushGpuWrites(address, bytes); });
}

}
