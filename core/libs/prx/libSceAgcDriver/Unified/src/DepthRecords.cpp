#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"

#include <algorithm>
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

DepthRecord::DepthRecord(const Graphics::Context& context, const DepthKey& key) : context(context), key(key) {
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
    Graphics::Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView recode depth record");
}

DepthRecord::~DepthRecord() {
    if (view != VK_NULL_HANDLE) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image != VK_NULL_HANDLE) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
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
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearDepthStencilValue value{clearDepth, clearStencil};
    const VkImageSubresourceRange range{aspects, 0u, 1u, 0u, 1u};
    context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized = true;
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
    return created;
}

DepthRecords& Depths() {
    static auto* depths = new DepthRecords;
    return *depths;
}

bool DepthRecordsOn() {
    static const bool on = std::getenv("APS5_RECODE_DEPTH_RECORDS") != nullptr;
    return on;
}

}
