#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace AgcDriver::Unified::Images {

namespace {

bool hasStencil(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_S8_UINT;
}

bool hasDepth(VkFormat format) {
    return format != VK_FORMAT_S8_UINT;
}

}

DepthRecord::DepthRecord(const Graphics::Context& context, const DepthKey& key) : device(context.device), key(key) {
    createView = context.Function<PFN_vkCreateImageView>("vkCreateImageView");
    destroyView = context.Function<PFN_vkDestroyImageView>("vkDestroyImageView");
    destroyImage = context.Function<PFN_vkDestroyImage>("vkDestroyImage");
    freeMemory = context.Function<PFN_vkFreeMemory>("vkFreeMemory");
    pipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    clearImage = context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage");
    aspects = (hasDepth(key.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | (hasStencil(key.format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = key.format;
    info.extent = {std::max(key.width, 1u), std::max(key.height, 1u), 1u};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Graphics::Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage recode depth record");
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto allocated = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory);
    if (allocated != VK_SUCCESS) {
        context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        image = VK_NULL_HANDLE;
        Graphics::Check(allocated, "vkAllocateMemory recode depth record");
    }
    Graphics::Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory recode depth record");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = key.format;
    viewInfo.subresourceRange = {aspects, 0u, 1u, 0u, 1u};
    Graphics::Check(createView(device, &viewInfo, nullptr, &view), "vkCreateImageView recode depth record");
}

DepthRecord::~DepthRecord() {
    if (depthView != VK_NULL_HANDLE) destroyView(device, depthView, nullptr);
    if (stencilView != VK_NULL_HANDLE) destroyView(device, stencilView, nullptr);
    if (view != VK_NULL_HANDLE) destroyView(device, view, nullptr);
    if (image != VK_NULL_HANDLE) destroyImage(device, image, nullptr);
    if (memory != VK_NULL_HANDLE) freeMemory(device, memory, nullptr);
}

void DepthRecord::Initialize(VkCommandBuffer commands, float clearDepth, std::uint8_t clearStencil) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspects, 0u, 1u, 0u, 1u};
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearDepthStencilValue value{clearDepth, clearStencil};
    const VkImageSubresourceRange range{aspects, 0u, 1u, 0u, 1u};
    clearImage(commands, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized = true;
}

VkImageView DepthRecord::SampledView(bool stencil) {
    auto& made = stencil ? stencilView : depthView;
    const VkImageAspectFlags aspect = stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    if (made != VK_NULL_HANDLE || (aspects & aspect) == 0u) return made;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = key.format;
    viewInfo.subresourceRange = {aspect, 0u, 1u, 0u, 1u};
    Graphics::Check(createView(device, &viewInfo, nullptr, &made), "vkCreateImageView recode depth record sampled view");
    return made;
}

std::shared_ptr<DepthRecord> DepthRecords::At(std::uint64_t address, bool& stencil) {
    std::lock_guard lock(mutex);
    for (const auto& [key, record] : records) {
        if (key.address == address || (key.stencilAddress != 0u && key.stencilAddress == address)) {
            stencil = key.address != address;
            return record->Initialized() ? record : nullptr;
        }
    }
    return nullptr;
}

std::shared_ptr<DepthRecord> DepthRecords::Find(const Graphics::Context& context, const Graphics::DepthTarget& target, std::string& reason) {
    if (target.samples != 1u) {
        reason = "multisampled depth target";
        std::lock_guard lock(mutex);
        counts.refused++;
        return nullptr;
    }
    if (target.firstSlice != 0u || target.lastSlice != 0u) {
        reason = "layered depth target";
        std::lock_guard lock(mutex);
        counts.refused++;
        return nullptr;
    }
    const DepthKey key{target.address, target.stencilAddress, target.htileAddress, target.extent.width, target.extent.height, target.format};
    std::lock_guard lock(mutex);
    if (const auto found = records.find(key); found != records.end()) {
        counts.found++;
        return found->second;
    }
    auto created = std::make_shared<DepthRecord>(context, key);
    records.emplace(key, created);
    counts.created++;
    if (counts.created <= 16u) std::fprintf(stderr, "[recode-depth] record 0x%llx stencil 0x%llx htile 0x%llx %ux%u format %d clear %g/%u\n", static_cast<unsigned long long>(key.address), static_cast<unsigned long long>(key.stencilAddress), static_cast<unsigned long long>(key.htileAddress), key.width, key.height, static_cast<int>(key.format), static_cast<double>(target.clearDepth), static_cast<unsigned>(target.clearStencil));
    return created;
}

DepthRecords& Depths() {
    static auto* depths = new DepthRecords;
    return *depths;
}

bool DepthRecordsOn() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_RECODE_DEPTH_RECORDS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return on;
}

}
