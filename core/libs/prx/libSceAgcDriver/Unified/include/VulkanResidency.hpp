#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANRESIDENCY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANRESIDENCY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include <mutex>
#include <optional>
#include <vector>

namespace AgcDriver::Unified {

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

private:
    Graphics::Context context;
    mutable std::mutex mutex;
    std::vector<VkDeviceMemory> poolMemory;
    std::optional<std::uint32_t> type;
};

}

#endif
