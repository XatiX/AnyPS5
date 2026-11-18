#include "prx/libSceAgcDriver/Unified/include/VulkanResidency.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>

namespace AgcDriver::Unified {

namespace {

constexpr VkDeviceSize HeadroomBytes = VkDeviceSize{512} << 20u;

VkBuffer bufferOf(std::uint64_t handle) {
    return reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle));
}

}

VulkanResidency::VulkanResidency(const Graphics::Context& context) : context(context) {}

VulkanResidency::~VulkanResidency() {
    for (const auto memory : poolMemory) {
        if (memory != VK_NULL_HANDLE) Graphics::MemoryAccounting::Free(context, memory);
    }
}

bool VulkanResidency::Supported(const Graphics::Context& context) {
    return context.sparseBinding && context.sparseResidencyBuffer && context.bufferDeviceAddress;
}

bool VulkanResidency::CreatePool(std::uint32_t pool) {
    std::lock_guard lock(mutex);
    if (!type) return false;
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    info.allocationSize = PoolBytes;
    info.memoryTypeIndex = *type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (Graphics::MemoryAccounting::Allocate(context, info, "recode residency", &memory) != VK_SUCCESS) return false;
    if (poolMemory.size() <= pool) poolMemory.resize(pool + 1, VK_NULL_HANDLE);
    poolMemory[pool] = memory;
    return true;
}

void VulkanResidency::DestroyPool(std::uint32_t pool) {
    std::lock_guard lock(mutex);
    if (pool >= poolMemory.size() || poolMemory[pool] == VK_NULL_HANDLE) return;
    Graphics::MemoryAccounting::Free(context, poolMemory[pool]);
    poolMemory[pool] = VK_NULL_HANDLE;
}

std::optional<DeviceBuffer> VulkanResidency::CreateBuffer(std::uint64_t bytes) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const std::array<std::uint32_t, 2> families{context.queueFamily, context.sparseFamily};
    if (context.sparseQueue != VK_NULL_HANDLE && context.sparseFamily != context.queueFamily) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) != VK_SUCCESS) return std::nullopt;
    VkMemoryRequirements requirements{};
    context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
    const bool aligned = requirements.alignment != 0 && requirements.alignment <= ChunkBytes && ChunkBytes % requirements.alignment == 0;
    {
        std::lock_guard lock(mutex);
        if (aligned && !type) type = context.DeviceMemoryType(requirements.memoryTypeBits, PoolBytes, "a recode residency pool");
        if (!aligned || (requirements.memoryTypeBits & (1u << *type)) == 0) {
            context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
            return std::nullopt;
        }
    }
    const VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, buffer};
    const auto address = context.Function<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR")(context.device, &addressInfo);
    if (address == 0) {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        return std::nullopt;
    }
    return DeviceBuffer{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer)), address};
}

void VulkanResidency::DestroyBuffer(std::uint64_t handle) {
    if (handle != 0) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, bufferOf(handle), nullptr);
}

bool VulkanResidency::Bind(std::uint64_t handle, std::span<const ChunkBind> binds) {
    if (binds.empty()) return true;
    std::vector<VkSparseMemoryBind> sparse;
    sparse.reserve(binds.size());
    {
        std::lock_guard lock(mutex);
        for (const auto& bind : binds) {
            if (bind.bound && (bind.chunk.pool >= poolMemory.size() || poolMemory[bind.chunk.pool] == VK_NULL_HANDLE)) return false;
            const auto memory = bind.bound ? poolMemory[bind.chunk.pool] : VK_NULL_HANDLE;
            sparse.push_back({bind.offset, ChunkBytes, memory, bind.bound ? bind.chunk.index * ChunkBytes : 0, 0});
        }
    }
    const VkSparseBufferMemoryBindInfo bufferBinds{bufferOf(handle), static_cast<std::uint32_t>(sparse.size()), sparse.data()};
    VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bind.bufferBindCount = 1;
    bind.pBufferBinds = &bufferBinds;
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
    const bool ownQueue = context.sparseQueue != VK_NULL_HANDLE && context.sparseQueueMutex != nullptr;
    VkResult submitted = VK_SUCCESS;
    VkResult waited = VK_SUCCESS;
    {
        std::unique_lock<std::mutex> queueLock;
        std::unique_lock<GuestMemory::GpuMutexType> gpuLock;
        if (ownQueue) queueLock = std::unique_lock(*context.sparseQueueMutex);
        else gpuLock = std::unique_lock(GuestMemory::GpuMutex());
        submitted = context.Function<PFN_vkQueueBindSparse>("vkQueueBindSparse")(ownQueue ? context.sparseQueue : context.queue, 1, &bind, fence);
        if (submitted == VK_SUCCESS) waited = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
    return submitted == VK_SUCCESS && waited == VK_SUCCESS;
}

std::uint64_t VulkanResidency::BudgetBytes() const {
    std::uint32_t known = 0;
    std::uint64_t owned = 0;
    {
        std::lock_guard lock(mutex);
        if (!type) return 0;
        known = *type;
        for (const auto memory : poolMemory) owned += memory != VK_NULL_HANDLE ? PoolBytes : 0;
    }
    const auto report = context.ReportedHeap(context.memory.memoryTypes[known].heapIndex);
    if (report.budget == 0) return 0;
    const auto spare = report.usage < report.budget ? report.budget - report.usage : 0;
    const auto room = owned + spare;
    return room > HeadroomBytes ? room - HeadroomBytes : PoolBytes;
}

}
