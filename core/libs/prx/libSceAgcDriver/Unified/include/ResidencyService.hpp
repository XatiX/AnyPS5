#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCYSERVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCYSERVICE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanResidency.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Unified {

class ResidencyService {
public:
    static ResidencyService& Get();
    static bool Enabled();

    bool Attach(const Graphics::Context& context);
    [[nodiscard]] bool Attached() const;
    void Add(std::uint64_t address, std::uint64_t size);
    void NoteDirty(std::span<const DirtyRange> ranges);
    bool Prepare(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] std::optional<DeviceSpan> Translate(std::uint64_t address, std::uint64_t size) const;
    std::size_t Record(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept);
    void AdvanceFrame();
    [[nodiscard]] std::string Line() const;

private:
    ResidencyService() = default;

    mutable std::mutex mutex;
    std::optional<Graphics::Context> context;
    std::unique_ptr<VulkanResidency> device;
    std::unique_ptr<Residency> ledger;
    std::unique_ptr<ResidentUploads> uploads;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> queued;
    std::atomic<std::uint64_t> frame{1};
    std::atomic<bool> attached{false};
    std::atomic<bool> refused{false};
    std::uint64_t recordedUploads = 0;
    std::uint64_t recordedBytes = 0;
};

}

#endif
