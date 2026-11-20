#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <exception>

namespace AgcDriver::Unified::Images {

namespace {

constexpr std::uint32_t DimensionsPerClass = 7;

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
    info.flags = compressed ? 0u : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {std::max(resource.width, 1u), std::max(resource.height, 1u), volume ? std::max(this->geometry.imageDepth, 1u) : 1u};
    info.mipLevels = static_cast<std::uint32_t>(std::max<std::size_t>(this->geometry.mips.size(), 1u));
    info.arrayLayers = volume ? 1u : std::max(this->geometry.imageLayers, 1u);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (compressed ? 0u : VK_IMAGE_USAGE_STORAGE_BIT) | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    const auto key = std::make_tuple(baseLevel, lastLevel, baseLayer, layerCount, view.swizzle, static_cast<std::uint32_t>(type));
    std::lock_guard lock(mutex);
    if (const auto found = views.find(key); found != views.end()) return reinterpret_cast<std::uint64_t>(found->second);
    VkComponentMapping components{};
    VkComponentSwizzle* channels[] = {&components.r, &components.g, &components.b, &components.a};
    for (std::uint32_t channel = 0; channel < 4u && !identity; ++channel) {
        const auto select = swizzleOf((view.swizzle >> (channel * 4u)) & 0xfu);
        if (!select.has_value()) {
            reason = "invalid image channel selector";
            return 0;
        }
        *channels[channel] = Graphics::TextureComponentChannel(resource.format, *select);
    }
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = compressed ? nullptr : &usage;
    info.image = image;
    info.viewType = type;
    info.format = format;
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

VkImageView VulkanSurface::ShapedStorageView(std::uint32_t level, bool array, bool uintView, std::string& reason) {
    if (compressed) {
        reason = "storage view of a compressed surface";
        return VK_NULL_HANDLE;
    }
    if (!uintView) {
        reason = "typed storage view";
        return VK_NULL_HANDLE;
    }
    const bool volume = resource.dimension == Graphics::TextureDimension::k3D;
    if (volume) {
        reason = "storage view of a volume";
        return VK_NULL_HANDLE;
    }
    const auto levels = static_cast<std::uint32_t>(std::max<std::size_t>(geometry.mips.size(), 1u));
    if (level >= levels) {
        reason = "storage view past the levels";
        return VK_NULL_HANDLE;
    }
    const auto key = std::make_tuple(level, array, uintView);
    std::lock_guard lock(mutex);
    if (const auto found = shapedStorageViews.find(key); found != shapedStorageViews.end()) return found->second;
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = &usage;
    info.image = image;
    info.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    info.format = uintView ? Graphics::TextureDetiler::ImageElementFormat(Graphics::BytesPerElement(resource.format)) : format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, 0u, array ? std::max(geometry.imageLayers, 1u) : 1u};
    VkImageView created = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created) != VK_SUCCESS) {
        reason = "vkCreateImageView recode storage";
        return VK_NULL_HANDLE;
    }
    shapedStorageViews.emplace(key, created);
    return created;
}

std::shared_ptr<DeviceSurface> VulkanSurfaceDevice::Create(const SurfaceKey&, std::span<const std::uint32_t, 8> words, std::string& reason) {
    try {
        const auto resource = Graphics::DecodeTextureResource(words);
        if (resource.dimension != Graphics::TextureDimension::k2D && resource.dimension != Graphics::TextureDimension::k2DArray && resource.dimension != Graphics::TextureDimension::kCube && resource.dimension != Graphics::TextureDimension::k3D) {
            reason = "surface dimension";
            return nullptr;
        }
        if (Graphics::IsRepackedTextureFormat(resource.format)) {
            reason = "repacked surface";
            return nullptr;
        }
        auto geometry = Graphics::DescribeSurface(resource);
        if (geometry.thick) {
            reason = "thick surface";
            return nullptr;
        }
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
