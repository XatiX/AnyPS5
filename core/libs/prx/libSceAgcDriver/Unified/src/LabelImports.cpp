#include "prx/libSceAgcDriver/Unified/include/LabelImports.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace AgcDriver::Unified::LabelImports {

namespace {

constexpr std::uint64_t RegionBytes = 64u * 1024u;

struct Region {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool failed = false;
};

struct Table {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::map<std::uint64_t, Region> regions;
    std::atomic<std::uint64_t> created{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> served{0};
};

Table& table() {
    static auto& shared = *new Table();
    return shared;
}

std::optional<std::uint32_t> coherentType(const Graphics::Context& context, std::uint32_t types) {
    constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t index = 0; index < context.memory.memoryTypeCount; ++index) {
        if ((types & (1u << index)) != 0 && (context.memory.memoryTypes[index].propertyFlags & coherent) == coherent) return index;
    }
    return std::nullopt;
}

Region import(const Graphics::Context& context, std::uint64_t base) {
    Region region;
    region.failed = true;
    bool boundsRefused = false;
    void* host = GuestArena::GuestArenaMapAliasForImport_nid_no_patch(static_cast<std::uintptr_t>(base), static_cast<std::size_t>(RegionBytes), &boundsRefused);
    if (host == nullptr) return region;
    VkMemoryHostPointerPropertiesEXT pointer{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT")(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &pointer) != VK_SUCCESS) {
        GuestArena::GuestArenaUnmapAlias_nid_postfix(host);
        return region;
    }
    const VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    info.size = RegionBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) != VK_SUCCESS) return region;
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
    const auto type = coherentType(context, requirements.memoryTypeBits & pointer.memoryTypeBits);
    const auto destroy = context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    if (!type) {
        destroy(context.device, buffer, nullptr);
        return region;
    }
    const VkImportMemoryHostPointerInfoEXT pointerInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &pointerInfo};
    allocation.allocationSize = RegionBytes;
    allocation.memoryTypeIndex = *type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory) != VK_SUCCESS) {
        destroy(context.device, buffer, nullptr);
        return region;
    }
    if (context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0) != VK_SUCCESS) {
        destroy(context.device, buffer, nullptr);
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        return region;
    }
    region.buffer = buffer;
    region.memory = memory;
    region.failed = false;
    return region;
}

}

bool Enabled() {
    static const bool enabled = std::getenv("APS5_RECODE_NO_LABEL_IMPORTS") == nullptr;
    return enabled;
}

std::optional<Target> For(const Graphics::Context& context, std::uint64_t address, std::uint64_t bytes) {
    if (!context.hostPointerImport || bytes == 0) return std::nullopt;
    const auto base = address & ~(RegionBytes - 1u);
    if (address + bytes > base + RegionBytes) return std::nullopt;
    auto& shared = table();
    std::lock_guard lock(shared.mutex);
    if (shared.device != context.device) {
        shared.regions.clear();
        shared.device = context.device;
    }
    auto found = shared.regions.find(base);
    if (found == shared.regions.end()) {
        found = shared.regions.emplace(base, import(context, base)).first;
        (found->second.failed ? shared.refused : shared.created).fetch_add(1, std::memory_order_relaxed);
        if (found->second.failed && shared.refused.load(std::memory_order_relaxed) <= 8u) std::fprintf(stderr, "[recode-label-imports] could not import the label region 0x%llx\n", static_cast<unsigned long long>(base));
    }
    if (found->second.failed) return std::nullopt;
    shared.served.fetch_add(1, std::memory_order_relaxed);
    return Target{found->second.buffer, address - base};
}

std::string Line() {
    auto& shared = table();
    char text[160];
    std::snprintf(text, sizeof(text), "label imports %llu regions, %llu refused, %llu stores", static_cast<unsigned long long>(shared.created.load()), static_cast<unsigned long long>(shared.refused.load()), static_cast<unsigned long long>(shared.served.load()));
    return text;
}

}
