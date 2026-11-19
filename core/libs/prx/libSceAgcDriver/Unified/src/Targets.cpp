#include "prx/libSceAgcDriver/Unified/include/Targets.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <exception>
#include <optional>

namespace AgcDriver::Unified::Images {

namespace {

constexpr std::uint32_t DimensionsPerClass = 7;
constexpr std::size_t PruneEvery = 256;

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

bool plainColor(std::uint32_t format) {
    return !Graphics::IsBlockCompressed(format) && !Graphics::IsRepackedTextureFormat(format);
}

bool sameSurface(const Graphics::GuestTextureResource& target, const Graphics::GuestTextureResource& request, std::string& reason) {
    if (request.dimension != Graphics::TextureDimension::k2D && request.dimension != Graphics::TextureDimension::k2DArray) {
        reason = "target read with another dimension";
        return false;
    }
    if (request.width != target.width || request.height != target.height || request.tileMode != target.tileMode || request.pipeBankXor != target.pipeBankXor || request.samples != 1u) {
        reason = "target read with another shape";
        return false;
    }
    if (request.dccAddress != 0u) {
        reason = "target read through DCC";
        return false;
    }
    if (!plainColor(request.format) || !plainColor(target.format) || Graphics::BytesPerElement(request.format) != Graphics::BytesPerElement(target.format)) {
        reason = "target read with another element";
        return false;
    }
    if (request.lastLevel >= std::max(target.mipCount, 1u) || request.baseArray != 0u || request.depthOrLastArray != 0u) {
        reason = "target read past its levels or layers";
        return false;
    }
    return true;
}

}

void TargetTable::Note(const std::shared_ptr<Graphics::StorageTexture>& texture) {
    if (texture == nullptr) return;
    const auto address = texture->Descriptor().baseAddress;
    const auto stamp = Events::Shared().Serial();
    std::lock_guard lock(mutex);
    auto& entry = entries[address];
    entry.texture = texture;
    entry.stamp = stamp;
    counts.noted++;
    if (entries.size() % PruneEvery == 0u) std::erase_if(entries, [](const auto& item) { return item.second.texture.expired(); });
}

std::shared_ptr<Graphics::StorageTexture> TargetTable::Find(std::uint64_t address, std::uint64_t bytes, std::span<const std::uint32_t, 8> words, std::string& reason) {
    std::lock_guard lock(mutex);
    const auto found = entries.find(address);
    if (found == entries.end()) {
        counts.misses++;
        reason = "no target at the address";
        return nullptr;
    }
    auto texture = found->second.texture.lock();
    if (texture == nullptr) {
        entries.erase(found);
        counts.misses++;
        reason = "no target at the address";
        return nullptr;
    }
    if (Events::Shared().ChangedSince(address, bytes, found->second.stamp)) {
        entries.erase(found);
        counts.cpuWritten++;
        reason = "target written by the CPU";
        return nullptr;
    }
    try {
        if (!sameSurface(texture->Descriptor(), Graphics::DecodeTextureResource(words), reason)) {
            counts.mismatched++;
            return nullptr;
        }
    } catch (const std::exception& error) {
        reason = std::string("target: ") + error.what();
        counts.mismatched++;
        return nullptr;
    }
    counts.hits++;
    return texture;
}

TargetCounts TargetTable::TakeCounts() {
    std::lock_guard lock(mutex);
    const auto taken = counts;
    counts = {};
    return taken;
}

TargetTable& Targets() {
    static TargetTable table;
    return table;
}

TargetView::~TargetView() {
    if (view != VK_NULL_HANDLE) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
}

std::shared_ptr<TargetView> TargetViews::View(const std::shared_ptr<Graphics::StorageTexture>& texture, std::span<const std::uint32_t, 8> words, std::uint32_t kind, std::string& reason) {
    if (kind < ShaderRecompiler::RuntimeAbi::FirstImageBinding || kind >= ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding) {
        reason = "target view for a storage kind";
        return nullptr;
    }
    const auto dimension = (kind - ShaderRecompiler::RuntimeAbi::FirstImageBinding) % DimensionsPerClass;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    switch (dimension) {
        case 2: type = VK_IMAGE_VIEW_TYPE_2D; break;
        case 3: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        default: reason = "target view dimension " + std::to_string(dimension); return nullptr;
    }
    const auto resource = Graphics::DecodeTextureResource(words);
    const auto format = Graphics::SampledTextureFormat(context, resource.format);
    if (format == VK_FORMAT_UNDEFINED) {
        reason = "target view format";
        return nullptr;
    }
    const std::uint32_t selects[] = {resource.dstSelX, resource.dstSelY, resource.dstSelZ, resource.dstSelW};
    std::uint32_t swizzle = 0;
    for (std::uint32_t channel = 0; channel < 4u; ++channel) swizzle |= (selects[channel] & 0xfu) << (channel * 4u);
    const auto baseLevel = std::min(resource.baseLevel, resource.lastLevel);
    const auto key = std::make_tuple(texture.get(), baseLevel, resource.lastLevel, swizzle, static_cast<std::uint32_t>(format), static_cast<std::uint32_t>(type));
    if (const auto found = views.find(key); found != views.end()) {
        if (auto view = found->second.lock()) return view;
        views.erase(found);
    }
    const bool identity = Graphics::IsConvertedTextureFormat(resource.format) && format != VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    VkComponentMapping components{};
    VkComponentSwizzle* channels[] = {&components.r, &components.g, &components.b, &components.a};
    for (std::uint32_t channel = 0; channel < 4u && !identity; ++channel) {
        const auto select = swizzleOf(selects[channel]);
        if (!select.has_value()) {
            reason = "invalid image channel selector";
            return nullptr;
        }
        *channels[channel] = Graphics::TextureComponentChannel(resource.format, *select);
    }
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.pNext = &usage;
    info.image = texture->Image();
    info.viewType = type;
    info.format = format;
    info.components = components;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, resource.lastLevel - baseLevel + 1u, 0u, 1u};
    VkImageView created = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &info, nullptr, &created) != VK_SUCCESS) {
        reason = "vkCreateImageView recode target";
        return nullptr;
    }
    auto view = std::make_shared<TargetView>(context, texture, created);
    if (views.size() % PruneEvery == 0u) std::erase_if(views, [](const auto& item) { return item.second.expired(); });
    views.emplace(key, view);
    return view;
}

}
