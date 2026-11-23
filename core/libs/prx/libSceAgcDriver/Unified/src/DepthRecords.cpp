#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Protector.hpp"

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

std::uint64_t planeBytes(const DepthKey& key) {
    const std::uint64_t texel = key.format == VK_FORMAT_D16_UNORM || key.format == VK_FORMAT_D16_UNORM_S8_UINT ? 2u : 4u;
    return static_cast<std::uint64_t>(key.width) * key.height * texel;
}

std::uint64_t stencilBytes(const DepthKey& key) {
    return static_cast<std::uint64_t>(key.width) * key.height;
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

bool DepthRecords::Overlaps(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    for (const auto& [key, record] : records) {
        if (address < key.address + planeBytes(key) && key.address < address + bytes) return true;
        if (key.stencilAddress != 0u && address < key.stencilAddress + stencilBytes(key) && key.stencilAddress < address + bytes) return true;
    }
    return false;
}

HtileState ReadHtile(std::uint64_t address, std::uint64_t bytes, bool depthOnly, bool stencil, float clearDepth) {
    if (address == 0u || bytes < 4u) return HtileState::Unknown;
    for (auto page = address & ~(PageBytes - 1u); page < address + bytes; page += PageBytes) {
        if (!Protector::Readable(page)) return HtileState::Unknown;
    }
    const auto* words = reinterpret_cast<const std::uint32_t*>(address);
    const auto count = bytes / 4u;
    for (std::uint64_t at = 0; at < count; ++at) {
        const auto word = words[at];
        if ((word & 15u) == 15u) return HtileState::Written;
        const auto depth = depthOnly ? word : word & 0xfffff00fu;
        const bool depthCleared = (depth == 0u && clearDepth == 0.0f) || (depth == (depthOnly ? 0xfffffff0u : 0xfffc0000u) && clearDepth == 1.0f);
        if (!depthCleared) return HtileState::Written;
        if (stencil && !depthOnly && ((word >> 8u) & 3u) != 0u) return HtileState::Written;
    }
    return HtileState::Cleared;
}

std::size_t DepthRecords::NoteHtileWrites(std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    std::size_t noted = 0;
    for (const auto& [key, record] : records) {
        if (key.htileAddress == 0u) continue;
        if (address < key.htileAddress + record->HtileBytes() && key.htileAddress < address + bytes) {
            record->NoteHtileWrite();
            ++noted;
        }
    }
    if (noted != 0u) counts.htileWrites++;
    return noted;
}

std::size_t DepthRecords::NoteGpuHtileWrites(std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    std::size_t noted = 0;
    for (const auto& [key, record] : records) {
        if (key.htileAddress == 0u) continue;
        if (address < key.htileAddress + record->HtileBytes() && key.htileAddress < address + bytes) {
            record->NoteGpuHtileWrite();
            ++noted;
        }
    }
    if (noted != 0u) counts.gpuHtileWrites++;
    return noted;
}

std::size_t DepthRecords::NoteCpuWrites(std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    std::size_t noted = 0;
    for (const auto& [key, record] : records) {
        const bool depth = address < key.address + planeBytes(key) && key.address < address + bytes;
        const bool stencil = key.stencilAddress != 0u && address < key.stencilAddress + stencilBytes(key) && key.stencilAddress < address + bytes;
        if (!depth && !stencil) continue;
        record->NotePlaneWrite();
        ++noted;
    }
    if (noted != 0u) counts.cpuPlaneWrites++;
    return noted;
}

std::vector<std::shared_ptr<DepthRecord>> DepthRecords::Overlapping(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<DepthRecord>> found;
    for (const auto& [key, record] : records) {
        const bool depth = address < key.address + planeBytes(key) && key.address < address + bytes;
        const bool stencil = key.stencilAddress != 0u && address < key.stencilAddress + stencilBytes(key) && key.stencilAddress < address + bytes;
        if (depth || stencil) found.push_back(record);
    }
    return found;
}

std::shared_ptr<DepthRecord> DepthRecords::At(std::uint64_t address, bool& stencil) {
    std::lock_guard lock(mutex);
    for (const auto& [key, record] : records) {
        if (key.slice == 0u && (key.address == address || (key.stencilAddress != 0u && key.stencilAddress == address))) {
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
    if (target.firstSlice != target.lastSlice) {
        reason = "layered depth target";
        std::lock_guard lock(mutex);
        counts.refused++;
        return nullptr;
    }
    const DepthKey key{target.address, target.stencilAddress, target.htileAddress, target.extent.width, target.extent.height, target.format, target.firstSlice};
    std::lock_guard lock(mutex);
    if (const auto found = records.find(key); found != records.end()) {
        counts.found++;
        return found->second;
    }
    for (auto at = records.begin(); at != records.end();) {
        const auto& held = at->first;
        if (held.address != key.address && held.address < key.address + planeBytes(key) && key.address < held.address + planeBytes(held)) {
            counts.superseded++;
            at = records.erase(at);
            continue;
        }
        ++at;
    }
    auto created = std::make_shared<DepthRecord>(context, key);
    created->SetTiling(target.depthSwizzle, target.pipeBankXor);
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
