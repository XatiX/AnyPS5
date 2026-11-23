#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include <algorithm>
#include <array>
#include <numeric>
#include <unordered_set>
#include <vector>
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

constexpr int ViewSpins = 64;
constexpr auto ViewWaitLimit = std::chrono::milliseconds(10);
constexpr std::uint64_t UnviewedRegionBytes = 64u * 1024u;
constexpr std::uint64_t PieceHolePages = 64;

std::atomic<std::uint64_t> skippedUploadBytes{0};
std::atomic<std::uint64_t> viewWaited{0};
std::atomic<std::uint64_t> viewFound{0};
std::atomic<std::uint64_t> viewWaitMicroseconds{0};

struct UnviewedRegions {
    std::mutex mutex;
    std::unordered_set<std::uint64_t> regions;
};

UnviewedRegions& unviewedRegions() {
    static auto& shared = *new UnviewedRegions();
    return shared;
}

bool knownUnviewed(std::uint64_t address) {
    auto& known = unviewedRegions();
    std::lock_guard lock(known.mutex);
    return known.regions.contains(address / UnviewedRegionBytes);
}

void noteUnviewed(std::uint64_t address) {
    auto& known = unviewedRegions();
    std::lock_guard lock(known.mutex);
    known.regions.insert(address / UnviewedRegionBytes);
}

std::byte* lookUpView(std::uint64_t address, std::size_t bytes) {
    auto* view = GuestMemory::OwnStoreTarget(address, bytes);
    return view == reinterpret_cast<void*>(address) ? nullptr : static_cast<std::byte*>(view);
}

std::size_t firstPageBytes(std::uint64_t address, std::uint64_t bytes) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(bytes, PageBytes - (address & (PageBytes - 1))));
}

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
        return lookUpView(address, bytes);
    }

    std::byte* PatientView(std::uint64_t address, std::size_t bytes) override {
        return Mirror::PatientView(address, bytes);
    }

    std::vector<DirtyRange> ViewedPieces(std::uint64_t address, std::uint64_t size) override {
        return Mirror::ViewedPieces(address, size);
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

std::byte* PatientView(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0) return nullptr;
    if (auto* view = lookUpView(address, bytes); view != nullptr) return view;
    const auto first = firstPageBytes(address, bytes);
    if (lookUpView(address, first) != nullptr) return lookUpView(address, bytes);
    if (!Protector::SectionBacked(address) || knownUnviewed(address)) return nullptr;
    viewWaited.fetch_add(1, std::memory_order_relaxed);
    const auto began = std::chrono::steady_clock::now();
    bool found = false;
    for (int tries = 0; !found; ++tries) {
        if (tries < ViewSpins) std::this_thread::yield();
        else if (std::chrono::steady_clock::now() - began >= ViewWaitLimit) break;
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
        found = lookUpView(address, first) != nullptr;
    }
    viewWaitMicroseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count()), std::memory_order_relaxed);
    if (!found) {
        noteUnviewed(address);
        return nullptr;
    }
    viewFound.fetch_add(1, std::memory_order_relaxed);
    return lookUpView(address, bytes);
}

std::vector<DirtyRange> ViewedPieces(std::uint64_t address, std::uint64_t size) {
    std::vector<DirtyRange> pieces;
    const auto end = address + size;
    std::uint64_t holes = 0;
    for (auto cursor = address; cursor < end && holes < PieceHolePages;) {
        const auto pageEnd = cursor + firstPageBytes(cursor, end - cursor);
        if (PatientView(cursor, static_cast<std::size_t>(pageEnd - cursor)) == nullptr) {
            ++holes;
            cursor = pageEnd;
            continue;
        }
        auto viewed = pageEnd;
        if (lookUpView(cursor, static_cast<std::size_t>(end - cursor)) != nullptr) {
            viewed = end;
        } else {
            auto unviewed = end;
            while (unviewed - viewed > PageBytes) {
                const auto middle = viewed + std::max<std::uint64_t>(((unviewed - viewed) / 2) & ~(PageBytes - 1), PageBytes);
                if (lookUpView(cursor, static_cast<std::size_t>(middle - cursor)) != nullptr) viewed = middle;
                else unviewed = middle;
            }
        }
        pieces.push_back({cursor, viewed - cursor});
        cursor = viewed;
    }
    return pieces;
}

ViewWaitCounts ViewWaits() {
    return {viewWaited.load(std::memory_order_relaxed), viewFound.load(std::memory_order_relaxed), viewWaitMicroseconds.load(std::memory_order_relaxed)};
}

bool WritesEnabled() {
    static const bool enabled = ResidencyService::MirrorAddressing();
    return enabled;
}

void Attach(const std::shared_ptr<VulkanDevice>& device) {
    if (!WritesEnabled()) return;
    static std::atomic<VulkanDevice*> attached{nullptr};
    if (attached.exchange(device.get(), std::memory_order_acq_rel) == device.get()) return;
    host().Attach(device);
    MemoryCore::Get().SetHost(&host());
    ResidencyService::Get().SetBatchedBinds(std::getenv("APS5_RECODE_SYNC_BINDS") == nullptr);
    ResidencyService::Get().SetKeep([](std::uint64_t address, std::uint64_t size) { return MemoryCore::Get().Written().Any(address, size); });
    GuestMemory::SetMirrorFlushHook([](std::uint64_t address, std::size_t bytes) { MemoryCore::Get().FlushGpuWrites(address, bytes); });
}

}
