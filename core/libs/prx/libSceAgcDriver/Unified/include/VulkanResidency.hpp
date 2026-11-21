#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANRESIDENCY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANRESIDENCY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace AgcDriver::Unified {

struct BindCounts {
    std::uint64_t calls = 0;
    std::uint64_t chunks = 0;
    std::uint64_t nanoseconds = 0;
    std::uint64_t queued = 0;
    std::uint64_t syncCalls = 0;
};

class VulkanResidency final : public ResidencyDevice {
public:
    explicit VulkanResidency(const Graphics::Context& context);
    ~VulkanResidency() override;
    VulkanResidency(const VulkanResidency&) = delete;
    VulkanResidency& operator=(const VulkanResidency&) = delete;

    static bool Supported(const Graphics::Context& context);
    bool CreatePool(std::uint32_t pool) override;
    void DestroyPool(std::uint32_t pool) override;
    std::optional<DeviceBuffer> CreateBuffer(std::uint64_t bytes) override;
    void DestroyBuffer(std::uint64_t handle) override;
    bool Bind(std::uint64_t handle, std::span<const ChunkBind> binds) override;
    [[nodiscard]] std::uint64_t BudgetBytes() const;
    void SetBatched(bool enabled);
    [[nodiscard]] bool Queued() const;
    std::optional<std::pair<VkSemaphore, std::uint64_t>> Flush();
    [[nodiscard]] BindCounts TakeCounts();

private:
    bool submit(std::span<const VkSparseBufferMemoryBindInfo> buffers, VkSemaphore signal, std::uint64_t value);
    VkSemaphore timeline();

    Graphics::Context context;
    mutable std::mutex mutex;
    std::vector<VkDeviceMemory> poolMemory;
    std::optional<std::uint32_t> type;
    bool batched = false;
    std::map<std::uint64_t, std::vector<VkSparseMemoryBind>> queued;
    VkSemaphore bindTimeline = VK_NULL_HANDLE;
    std::uint64_t bindValue = 0;
    BindCounts counts;
};

}

#endif
