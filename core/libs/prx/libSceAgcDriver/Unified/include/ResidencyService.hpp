#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCYSERVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCYSERVICE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/GpuPageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanResidency.hpp"
#include "BdaAbi.hpp"
#include <atomic>
#include <deque>
#include <map>
#include <span>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {
class Buffer;
}

namespace AgcDriver::Unified {

inline constexpr std::uint32_t PageTableLeafCapacity = 1024;

class ResidencyService {
public:
    static ResidencyService& Get();
    static bool Enabled();
    static bool PageTableEnabled();
    static VkMemoryPropertyFlags ShaderReadMemory(const Graphics::Context& context, VkDeviceSize bytes);

    bool Attach(const Graphics::Context& context);
    [[nodiscard]] bool Attached() const;
    void Add(std::uint64_t address, std::uint64_t size);
    void NoteDirty(std::span<const DirtyRange> ranges);
    bool Prepare(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] std::optional<DeviceSpan> Translate(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::uint64_t TranslateMapped(std::uint64_t address, std::uint64_t size) const;
    std::optional<DeviceSpan> Serve(std::uint64_t address, std::uint64_t size, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed);
    bool ServeFaultPage(std::uint64_t page, std::uint64_t allocation, std::uint64_t allocationBytes, std::span<const std::pair<std::uint64_t, std::uint64_t>> committed);
    void MapImports(std::uint64_t serial, std::shared_ptr<const std::vector<ShaderRecompiler::BdaAbi::Range>> ranges);
    void ForgetImport(std::uint64_t base, std::uint64_t bytes, std::uint64_t deviceBegin, std::uint64_t deviceEnd);
    [[nodiscard]] std::shared_ptr<Graphics::Buffer> PageTableBuffer() const;
    std::vector<PendingUpload> ReadBack(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    [[nodiscard]] bool Pending() const;
    std::size_t Record(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept);
    void AdvanceFrame();
    [[nodiscard]] std::uint64_t Frame() const { return frame.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t Removals() const;
    [[nodiscard]] std::string Line() const;

private:
    ResidencyService() = default;
    std::shared_ptr<Graphics::Buffer> staging(std::size_t bytes);

    mutable std::mutex mutex;
    std::optional<Graphics::Context> context;
    std::unique_ptr<VulkanResidency> device;
    std::unique_ptr<Residency> ledger;
    std::unique_ptr<ResidentUploads> uploads;
    std::unique_ptr<GpuPageTable> pages;
    std::shared_ptr<Graphics::Buffer> pageBuffer;
    std::shared_ptr<Graphics::Buffer> nullPage;
    std::uint64_t pageUpdates = 0;
    std::uint64_t pageUpdateBytes = 0;
    std::uint64_t pageLeavesDropped = 0;
    std::uint64_t faultPagesServed = 0;
    std::uint64_t faultPagesRefused = 0;
    std::uint64_t readBacks = 0;
    std::uint64_t readBackBytes = 0;
    std::uint64_t readBackMissedBytes = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> queued;
    std::map<std::uint64_t, std::uint64_t> served;
    std::map<std::uint64_t, std::uint64_t> importPages;
    std::deque<std::shared_ptr<const std::vector<ShaderRecompiler::BdaAbi::Range>>> importSpaces;
    std::uint64_t importSerial = 0;
    std::uint64_t importMaps = 0;
    std::uint64_t importPagesForgotten = 0;
    std::uint64_t faultPagesMapped = 0;
    std::uint64_t serveRefusals = 0;
    std::atomic<std::uint64_t> frame{1};
    std::atomic<bool> attached{false};
    std::atomic<bool> refused{false};
    std::uint64_t recordedUploads = 0;
    std::uint64_t recordedBytes = 0;
    std::vector<std::shared_ptr<Graphics::Buffer>> stagingPool;
    std::uint64_t stagingCreated = 0;
    std::uint64_t stagingReused = 0;
};

}

#endif
