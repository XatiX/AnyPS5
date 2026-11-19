#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANHEAP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_VULKANHEAP_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Heap.hpp"

namespace AgcDriver::Unified::Heap {

class VulkanHeapDevice final : public HeapDevice {
public:
    explicit VulkanHeapDevice(const Graphics::Context& context);
    ~VulkanHeapDevice() override;
    VulkanHeapDevice(const VulkanHeapDevice&) = delete;
    VulkanHeapDevice& operator=(const VulkanHeapDevice&) = delete;

    static bool Supported(const Graphics::Context& context);
    void WriteImage(std::uint32_t kind, std::uint32_t slot, std::uint64_t view, std::uint32_t layout) override;
    void WriteSampler(std::uint32_t slot, std::uint64_t sampler) override;

    [[nodiscard]] VkDescriptorSetLayout Layout() const { return layout; }
    [[nodiscard]] VkDescriptorSet Set() const { return set; }
    [[nodiscard]] std::uint32_t SampledSlotCount() const { return sampledSlots; }
    [[nodiscard]] std::uint32_t SamplerSlotCount() const { return samplerSlots; }
    [[nodiscard]] std::uint32_t StorageSlotCount() const { return storageSlots; }

private:
    void release() noexcept;

    Graphics::Context context;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t sampledSlots = 0;
    std::uint32_t samplerSlots = 0;
    std::uint32_t storageSlots = 0;
};

}

#endif
