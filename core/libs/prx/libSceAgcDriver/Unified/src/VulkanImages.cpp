#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <vector>

namespace AgcDriver::Unified::Images {

namespace {

constexpr std::uint32_t DimensionsPerClass = 7;

VkFormat linearStorageFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
        default: return format;
    }
}

VkFormat packedStorageView(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return VK_FORMAT_R16_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat uintStorageView(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_UINT;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_UINT;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_UINT;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_UINT;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_UINT;
        case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_UINT;
        case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_UINT;
        case VK_FORMAT_R16_SFLOAT: return VK_FORMAT_R16_UINT;
        case VK_FORMAT_R16G16_SFLOAT: return VK_FORMAT_R16G16_UINT;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool volumeTargets() {
    static const bool targets = std::getenv("APS5_RECODE_NO_VOLUME_TARGETS") == nullptr;
    return targets;
}

std::optional<VkComponentSwizzle> swizzleOf(std::uint32_t select) {
    switch (select) {
        case 0: return VK_COMPONENT_SWIZZLE_ZERO;
        case 1: return VK_COMPONENT_SWIZZLE_ONE;
        case 4: return VK_COMPONENT_SWIZZLE_R;
        case 5: return VK_COMPONENT_SWIZZLE_G;
        case 6: return VK_COMPONENT_SWIZZLE_B;
        case 7: return VK_COMPONENT_SWIZZLE_A;
        default: return std::nullopt;
    }
}

}

VulkanSurface::VulkanSurface(const Graphics::Context& context, const Graphics::GuestTextureResource& resource, Graphics::SurfaceGeometry geometry, VkFormat format) : context(context), resource(resource), geometry(std::move(geometry)), format(format) {
    const bool volume = resource.dimension == Graphics::TextureDimension::k3D;
    compressed = Graphics::BlockWidth(resource.format) != 1u;
    identity = Graphics::IsConvertedTextureFormat(resource.format) && format != VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = compressed ? 0u : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (volume && volumeTargets() ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0u);
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {std::max(resource.width, 1u), std::max(resource.height, 1u), volume ? std::max(this->geometry.imageDepth, 1u) : 1u};
    info.mipLevels = static_cast<std::uint32_t>(std::max<std::size_t>(this->geometry.mips.size(), 1u));
    info.arrayLayers = volume ? 1u : std::max(this->geometry.imageLayers, 1u);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    attachable = !compressed && (!volume || volumeTargets());
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (compressed ? 0u : VK_IMAGE_USAGE_STORAGE_BIT) | (attachable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u) | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Graphics::Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage recode surface");
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto allocated = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory);
    if (allocated != VK_SUCCESS) {
        context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        image = VK_NULL_HANDLE;
        Graphics::Check(allocated, "vkAllocateMemory recode surface");
    }
    Graphics::Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory recode surface");
}

VulkanSurface::~VulkanSurface() {
    const auto destroyView = context.Function<PFN_vkDestroyImageView>("vkDestroyImageView");
    for (const auto& [key, view] : views) destroyView(context.device, view, nullptr);
    for (const auto& [key, view] : storageViews) destroyView(context.device, view, nullptr);
    for (const auto& [key, view] : shapedStorageViews) destroyView(context.device, view, nullptr);
    for (const auto& [key, view] : attachmentViews) destroyView(context.device, view, nullptr);
    if (image != VK_NULL_HANDLE) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

std::uint64_t VulkanSurface::View(const ViewKey& view, std::uint32_t kind, std::string& reason) {
    if (kind < ShaderRecompiler::RuntimeAbi::FirstImageBinding || kind >= ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding) {
        reason = "image view for a storage kind";
        return 0;
    }
    const auto dimension = (kind - ShaderRecompiler::RuntimeAbi::FirstImageBinding) % DimensionsPerClass;
    const bool volume = resource.dimension == Graphics::TextureDimension::k3D;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    switch (dimension) {
        case 2: type = VK_IMAGE_VIEW_TYPE_2D; break;
        case 3: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case 6: type = VK_IMAGE_VIEW_TYPE_3D; break;
        default: reason = "image view dimension " + std::to_string(dimension); return 0;
    }
    if ((type == VK_IMAGE_VIEW_TYPE_3D) != volume) {
        reason = "image view shape differs from the surface";
        return 0;
    }
    const auto levels = static_cast<std::uint32_t>(std::max<std::size_t>(geometry.mips.size(), 1u));
    const auto layers = volume ? 1u : std::max(geometry.imageLayers, 1u);
    const auto baseLevel = std::min(view.baseLevel, levels - 1u);
    const auto lastLevel = std::clamp(view.lastLevel, baseLevel, levels - 1u);
    const auto baseLayer = volume ? 0u : std::min(view.baseArray, layers - 1u);
    const auto layerCount = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? std::max(std::min(view.lastArray, layers - 1u), baseLayer) - baseLayer + 1u : 1u;
    const auto guestFormat = view.format != 0u && !compressed ? view.format : resource.format;
    auto viewFormat = format;
    auto viewIdentity = identity;
    if (guestFormat != resource.format) {
        viewFormat = Graphics::SampledTextureFormat(context, guestFormat);
        if (viewFormat == VK_FORMAT_UNDEFINED) {
            reason = "image view format";
            return 0;
        }
        viewIdentity = Graphics::IsConvertedTextureFormat(guestFormat) && viewFormat != VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    const auto key = std::make_tuple(baseLevel, lastLevel, baseLayer, layerCount, view.swizzle, static_cast<std::uint32_t>(type), static_cast<std::uint32_t>(viewFormat));
    std::lock_guard lock(mutex);
    if (const auto found = views.find(key); found != views.end()) return reinterpret_cast<std::uint64_t>(found->second);
    VkComponentMapping components{};
    VkComponentSwizzle* channels[] = {&components.r, &components.g, &components.b, &components.a};
    for (std::uint32_t channel = 0; channel < 4u && !viewIdentity; ++channel) {
        const auto select = swizzleOf((view.swizzle >> (channel * 4u)) & 0xfu);
        if (!select.has_value()) {
            reason = "invalid image channel selector";
            return 0;
        }
        *channels[channel] = Graphics::TextureComponentChannel(guestFormat, *select);
    }
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = compressed ? nullptr : &usage;
    info.image = image;
    info.viewType = type;
    info.format = viewFormat;
    info.components = components;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, lastLevel - baseLevel + 1u, baseLayer, layerCount};
    VkImageView created = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created) != VK_SUCCESS) {
        reason = "vkCreateImageView recode surface";
        return 0;
    }
    views.emplace(key, created);
    return reinterpret_cast<std::uint64_t>(created);
}

VkImageView VulkanSurface::StorageView(std::uint32_t level, std::uint32_t layer, std::uint32_t elementBytes) {
    const auto key = std::make_tuple(level, layer, elementBytes);
    std::lock_guard lock(mutex);
    if (const auto found = storageViews.find(key); found != storageViews.end()) return found->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = &usage;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = Graphics::TextureDetiler::ImageElementFormat(elementBytes);
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, layer, 1u};
    VkImageView created = VK_NULL_HANDLE;
    Graphics::Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created), "vkCreateImageView recode surface storage");
    storageViews.emplace(key, created);
    return created;
}

void VulkanSurface::EnsureGeneral(VkCommandBuffer commands) {
    if (initialized) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
    context.Resolved(&Graphics::DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized = true;
}

void CopyImageContent(const Graphics::Context& context, VkCommandBuffer commands, VkImage source, VkImage destination, std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t layers, std::uint32_t destinationLayer) {
    std::vector<VkImageCopy> regions;
    for (std::uint32_t level = 0; level < levels; ++level) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, layers};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, destinationLayer, layers};
        region.extent = {std::max(width >> level, 1u), std::max(height >> level, 1u), 1u};
        regions.push_back(region);
    }
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Function<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL, static_cast<std::uint32_t>(regions.size()), regions.data());
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

VkImageView VulkanSurface::AttachmentView(VkFormat viewFormat, std::uint32_t level, std::uint32_t layer, std::uint32_t layers, std::string& reason) {
    if (!attachable) {
        reason = "surface is not attachable";
        return VK_NULL_HANDLE;
    }
    const bool volume = resource.dimension == Graphics::TextureDimension::k3D;
    const auto levels = static_cast<std::uint32_t>(std::max<std::size_t>(geometry.mips.size(), 1u));
    const auto imageLayers = volume ? std::max(std::max(geometry.imageDepth, 1u) >> level, 1u) : std::max(geometry.imageLayers, 1u);
    if (level >= levels || layers == 0u || layer + layers > imageLayers) {
        reason = "attachment outside the surface";
        return VK_NULL_HANDLE;
    }
    const auto key = std::make_tuple(static_cast<std::uint32_t>(viewFormat), level, layer, layers);
    std::lock_guard lock(mutex);
    if (const auto found = attachmentViews.find(key); found != attachmentViews.end()) return found->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = &usage;
    info.image = image;
    info.viewType = layers > 1u || volume ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    info.format = viewFormat;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, layer, layers};
    VkImageView created = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created) != VK_SUCCESS) {
        reason = "vkCreateImageView recode attachment";
        return VK_NULL_HANDLE;
    }
    attachmentViews.emplace(key, created);
    return created;
}

VkImageView VulkanSurface::ShapedStorageView(std::uint32_t level, StorageShape shape, bool uintView, bool atomic, std::string& reason, std::uint32_t layer) {
    if (compressed) {
        reason = "storage view of a compressed surface";
        return VK_NULL_HANDLE;
    }
    const bool volume = resource.dimension == Graphics::TextureDimension::k3D;
    if (volume != (shape == StorageShape::Volume)) {
        reason = volume ? "flat storage view of a volume" : "volume storage view of a flat surface";
        return VK_NULL_HANDLE;
    }
    const auto levels = static_cast<std::uint32_t>(std::max<std::size_t>(geometry.mips.size(), 1u));
    if (level >= levels) {
        reason = "storage view past the levels";
        return VK_NULL_HANDLE;
    }
    const auto storageFormat = linearStorageFormat(Graphics::ResolveTextureFormat(resource.format));
    if (atomic && storageFormat != VK_FORMAT_R32_UINT && storageFormat != VK_FORMAT_R32_SINT && storageFormat != VK_FORMAT_R32_SFLOAT) {
        reason = "atomic storage view of a format that is not 32-bit single-channel";
        return VK_NULL_HANDLE;
    }
    if (shape != StorageShape::Flat) layer = 0;
    if (layer >= std::max(geometry.imageLayers, 1u)) {
        reason = "storage view past the layers";
        return VK_NULL_HANDLE;
    }
    const auto key = std::make_tuple(level, static_cast<std::uint32_t>(shape), uintView, atomic, layer);
    std::lock_guard lock(mutex);
    if (const auto found = shapedStorageViews.find(key); found != shapedStorageViews.end()) return found->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = &usage;
    info.image = image;
    info.viewType = shape == StorageShape::Volume ? VK_IMAGE_VIEW_TYPE_3D : shape == StorageShape::Array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    const auto packed = packedStorageView(storageFormat);
    const auto uintFormat = uintView ? uintStorageView(storageFormat) : VK_FORMAT_UNDEFINED;
    info.format = atomic ? VK_FORMAT_R32_UINT : packed != VK_FORMAT_UNDEFINED ? packed : uintFormat != VK_FORMAT_UNDEFINED ? uintFormat : storageFormat;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, layer, shape == StorageShape::Array ? std::max(geometry.imageLayers, 1u) : 1u};
    VkImageView created = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created) != VK_SUCCESS) {
        reason = "vkCreateImageView recode storage";
        return VK_NULL_HANDLE;
    }
    shapedStorageViews.emplace(key, created);
    return created;
}

std::shared_ptr<DeviceSurface> VulkanSurfaceDevice::Create(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) {
    try {
        return CreateFromResource(key, Graphics::DecodeTextureResource(words), reason);
    } catch (const std::exception& error) {
        reason = std::string("surface: ") + error.what();
        return nullptr;
    }
}

std::shared_ptr<DeviceSurface> VulkanSurfaceDevice::CreateFromResource(const SurfaceKey&, const Graphics::GuestTextureResource& resource, std::string& reason) {
    try {
        if (resource.dimension != Graphics::TextureDimension::k2D && resource.dimension != Graphics::TextureDimension::k2DArray && resource.dimension != Graphics::TextureDimension::kCube && resource.dimension != Graphics::TextureDimension::k3D) {
            reason = "surface dimension";
            return nullptr;
        }
        if (Graphics::IsRepackedTextureFormat(resource.format)) {
            reason = "repacked surface";
            return nullptr;
        }
        auto geometry = Graphics::DescribeSurface(resource);
        const auto format = Graphics::SampledTextureFormat(context, resource.format);
        if (format == VK_FORMAT_UNDEFINED) {
            reason = "surface format";
            return nullptr;
        }
        return std::make_shared<VulkanSurface>(context, resource, std::move(geometry), format);
    } catch (const std::exception& error) {
        reason = std::string("surface: ") + error.what();
        return nullptr;
    }
}

}
