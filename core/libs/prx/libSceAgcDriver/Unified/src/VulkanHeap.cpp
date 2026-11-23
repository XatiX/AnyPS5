#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PlaceholderImages.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace AgcDriver::Unified::Heap {

namespace {

namespace Abi = ShaderRecompiler::RuntimeAbi;

constexpr std::uint32_t SampledKinds = LastSampledKind - FirstImageKind + 1u;
constexpr std::uint32_t StorageKinds = LastStorageKind - LastSampledKind;

static_assert(Abi::DirectHeapSet == 4u && SamplerKind == static_cast<std::uint32_t>(Abi::Binding::Samplers) && SamplerKind < Abi::DirectHeapBindingCount);
static_assert(FirstImageKind == Abi::FirstImageBinding && LastSampledKind + 1u == Abi::FirstStorageImageBinding && LastStorageKind == Abi::FirstImageBinding + Abi::ImageBindingCount - 1u);

std::uint32_t limited(std::uint32_t wanted, std::uint32_t limit, std::uint32_t kinds) {
    return limit == 0u ? wanted : std::min(wanted, limit / kinds);
}

VkDescriptorType typeOf(std::uint32_t kind) {
    return kind <= LastSampledKind ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

}

bool VulkanHeapDevice::Supported(const Graphics::Context& context) {
    return context.descriptorIndexing && context.runtimeDescriptorArray && context.partiallyBound && context.sampledImageUpdateAfterBind;
}

VulkanHeapDevice::VulkanHeapDevice(const Graphics::Context& context) : context(context) {
    if (!Supported(context)) throw std::runtime_error("descriptor heap: the device lacks update-after-bind descriptor indexing");
    const auto& limits = context.descriptorIndexingLimits;
    sampledSlots = limited(SampledSlots, limits.maxDescriptorSetUpdateAfterBindSampledImages, SampledKinds);
    samplerSlots = limited(SamplerSlots, limits.maxDescriptorSetUpdateAfterBindSamplers, 1u);
    storageSlots = context.storageImageUpdateAfterBind ? limited(StorageSlots, limits.maxDescriptorSetUpdateAfterBindStorageImages, StorageKinds) : 0u;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT | (context.meshShader ? VkShaderStageFlags{VK_SHADER_STAGE_MESH_BIT_EXT} : VkShaderStageFlags{0});
    constexpr VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (auto kind = FirstImageKind; kind <= LastStorageKind; ++kind) {
        const auto count = kind <= LastSampledKind ? sampledSlots : storageSlots;
        if (count != 0u) bindings.push_back({Abi::DirectHeapBindingBase + kind, typeOf(kind), count, stages, nullptr});
    }
    bindings.push_back({Abi::DirectHeapBindingBase + SamplerKind, VK_DESCRIPTOR_TYPE_SAMPLER, samplerSlots, stages, nullptr});
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), bindingFlags);
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagInfo.bindingCount = static_cast<std::uint32_t>(flags.size());
    flagInfo.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flagInfo};
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledSlots * SampledKinds}, {VK_DESCRIPTOR_TYPE_SAMPLER, samplerSlots}};
    if (storageSlots != 0u) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageSlots * StorageKinds});
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    try {
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreateDescriptorSetLayout descriptor heap");
        Graphics::Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool descriptor heap");
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool;
        allocateInfo.descriptorSetCount = 1u;
        allocateInfo.pSetLayouts = &layout;
        Graphics::Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocateInfo, &set), "vkAllocateDescriptorSets descriptor heap");
    } catch (...) {
        release();
        throw;
    }
    if (context.nullDescriptors || context.placeholderImages == nullptr) return;
    for (auto kind = FirstImageKind; kind <= LastStorageKind; ++kind) {
        if ((kind <= LastSampledKind ? sampledSlots : storageSlots) == 0u) continue;
        try {
            WriteImage(kind, 0u, reinterpret_cast<std::uint64_t>(context.placeholderImages->View(kind)), VK_IMAGE_LAYOUT_GENERAL);
        } catch (const std::exception&) {
        }
    }
}

VulkanHeapDevice::~VulkanHeapDevice() {
    release();
}

void VulkanHeapDevice::release() noexcept {
    if (pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
    if (layout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, layout, nullptr);
    pool = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    set = VK_NULL_HANDLE;
}

void VulkanHeapDevice::WriteImage(std::uint32_t kind, std::uint32_t slot, std::uint64_t view, std::uint32_t imageLayout) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, reinterpret_cast<VkImageView>(view), static_cast<VkImageLayout>(imageLayout)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = Abi::DirectHeapBindingBase + kind;
    write.dstArrayElement = slot;
    write.descriptorCount = 1u;
    write.descriptorType = typeOf(kind);
    write.pImageInfo = &info;
    context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, 1u, &write, 0u, nullptr);
}

void VulkanHeapDevice::WriteSampler(std::uint32_t slot, std::uint64_t sampler) {
    const VkDescriptorImageInfo info{reinterpret_cast<VkSampler>(sampler), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = Abi::DirectHeapBindingBase + SamplerKind;
    write.dstArrayElement = slot;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, 1u, &write, 0u, nullptr);
}

}
