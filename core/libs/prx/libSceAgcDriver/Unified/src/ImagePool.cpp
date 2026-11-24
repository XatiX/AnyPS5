#include "prx/libSceAgcDriver/Unified/include/ImagePool.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace AgcDriver::Unified::Images {

ImagePool::ImagePool(const Graphics::Context& context) : device(context.device), allocateMemory(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")), freeMemory(context.Function<PFN_vkFreeMemory>("vkFreeMemory")) {}

ImagePool::~ImagePool() {
    std::lock_guard lock(mutex);
    for (auto& block : blocks) {
        if (block != nullptr && block->memory != VK_NULL_HANDLE) freeMemory(device, block->memory, nullptr);
    }
}

bool ImagePool::take(Block& block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    for (auto at = block.free.begin(); at != block.free.end(); ++at) {
        const auto begin = at->first;
        const auto end = begin + at->second;
        const auto aligned = (begin + alignment - 1u) / alignment * alignment;
        if (aligned + size > end) continue;
        block.free.erase(at);
        if (aligned > begin) block.free.emplace(begin, aligned - begin);
        if (aligned + size < end) block.free.emplace(aligned + size, end - aligned - size);
        block.used += size;
        offset = aligned;
        return true;
    }
    return false;
}

void ImagePool::giveBack(Block& block, VkDeviceSize offset, VkDeviceSize size) {
    block.used -= size;
    auto [at, inserted] = block.free.emplace(offset, size);
    if (!inserted) return;
    if (const auto next = std::next(at); next != block.free.end() && at->first + at->second == next->first) {
        at->second += next->second;
        block.free.erase(next);
    }
    if (at != block.free.begin()) {
        const auto previous = std::prev(at);
        if (previous->first + previous->second == at->first) {
            previous->second += at->second;
            block.free.erase(at);
        }
    }
}

VkResult ImagePool::Allocate(const VkMemoryRequirements& requirements, std::uint32_t memoryType, PooledMemory& out) {
    out = {};
    const auto alignment = std::max<VkDeviceSize>(requirements.alignment, 1u);
    if (requirements.size > PooledLimitBytes) {
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        const auto result = allocateMemory(device, &allocation, nullptr, &out.memory);
        if (result != VK_SUCCESS) return result;
        out.size = requirements.size;
        out.dedicated = true;
        std::lock_guard lock(mutex);
        counts.dedicated++;
        counts.dedicatedBytes += requirements.size;
        return VK_SUCCESS;
    }
    std::lock_guard lock(mutex);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        auto& block = blocks[index];
        if (block == nullptr || block->memoryType != memoryType) continue;
        VkDeviceSize offset = 0;
        if (!take(*block, requirements.size, alignment, offset)) continue;
        out = {block->memory, offset, requirements.size, static_cast<std::uint32_t>(index), false};
        counts.pooled++;
        counts.pooledBytes += requirements.size;
        return VK_SUCCESS;
    }
    auto block = std::make_unique<Block>();
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = PoolBlockBytes;
    allocation.memoryTypeIndex = memoryType;
    const auto result = allocateMemory(device, &allocation, nullptr, &block->memory);
    if (result != VK_SUCCESS) return result;
    block->memoryType = memoryType;
    block->free.emplace(0u, PoolBlockBytes);
    VkDeviceSize offset = 0;
    static_cast<void>(take(*block, requirements.size, alignment, offset));
    auto slot = std::find(blocks.begin(), blocks.end(), nullptr);
    if (slot == blocks.end()) slot = blocks.insert(blocks.end(), nullptr);
    out = {block->memory, offset, requirements.size, static_cast<std::uint32_t>(slot - blocks.begin()), false};
    *slot = std::move(block);
    counts.blocks++;
    counts.blockBytes += PoolBlockBytes;
    counts.pooled++;
    counts.pooledBytes += requirements.size;
    return VK_SUCCESS;
}

void ImagePool::Free(const PooledMemory& allocation) {
    if (allocation.memory == VK_NULL_HANDLE) return;
    if (allocation.dedicated) {
        freeMemory(device, allocation.memory, nullptr);
        std::lock_guard lock(mutex);
        counts.dedicated--;
        counts.dedicatedBytes -= allocation.size;
        return;
    }
    std::lock_guard lock(mutex);
    if (allocation.block >= blocks.size() || blocks[allocation.block] == nullptr || blocks[allocation.block]->memory != allocation.memory) return;
    auto& block = blocks[allocation.block];
    giveBack(*block, allocation.offset, allocation.size);
    counts.pooled--;
    counts.pooledBytes -= allocation.size;
    if (block->used != 0u) return;
    freeMemory(device, block->memory, nullptr);
    block.reset();
    counts.blocks--;
    counts.blockBytes -= PoolBlockBytes;
}

PoolCounts ImagePool::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

std::shared_ptr<ImagePool> PoolFor(const Graphics::Context& context) {
    static std::mutex poolsMutex;
    static std::unordered_map<VkDevice, std::weak_ptr<ImagePool>> pools;
    std::lock_guard lock(poolsMutex);
    auto& held = pools[context.device];
    if (auto pool = held.lock()) return pool;
    std::erase_if(pools, [](const auto& entry) { return entry.second.expired(); });
    auto pool = std::make_shared<ImagePool>(context);
    pools[context.device] = pool;
    return pool;
}

bool PoolsImages() {
    static const bool pooled = std::getenv("APS5_RECODE_NO_IMAGE_POOL") == nullptr;
    return pooled;
}

}
