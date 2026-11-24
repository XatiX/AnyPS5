#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEPOOL_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace AgcDriver::Unified::Images {

inline constexpr VkDeviceSize PoolBlockBytes = VkDeviceSize{64} << 20u;
inline constexpr VkDeviceSize PooledLimitBytes = VkDeviceSize{8} << 20u;

struct PooledMemory {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    std::uint32_t block = 0;
    bool dedicated = false;
};

struct PoolCounts {
    std::uint64_t blocks = 0;
    std::uint64_t blockBytes = 0;
    std::uint64_t pooled = 0;
    std::uint64_t pooledBytes = 0;
    std::uint64_t dedicated = 0;
    std::uint64_t dedicatedBytes = 0;
};

class ImagePool {
public:
    explicit ImagePool(const Graphics::Context& context);
    ~ImagePool();
    ImagePool(const ImagePool&) = delete;
    ImagePool& operator=(const ImagePool&) = delete;

    [[nodiscard]] VkResult Allocate(const VkMemoryRequirements& requirements, std::uint32_t memoryType, PooledMemory& out);
    void Free(const PooledMemory& allocation);
    [[nodiscard]] PoolCounts Counts() const;
    [[nodiscard]] VkDevice Device() const { return device; }

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint32_t memoryType = 0;
        std::map<VkDeviceSize, VkDeviceSize> free;
        VkDeviceSize used = 0;
    };

    [[nodiscard]] bool take(Block& block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
    static void giveBack(Block& block, VkDeviceSize offset, VkDeviceSize size);

    VkDevice device = VK_NULL_HANDLE;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Block>> blocks;
    PoolCounts counts;
};

[[nodiscard]] std::shared_ptr<ImagePool> PoolFor(const Graphics::Context& context);
[[nodiscard]] bool PoolsImages();

}

#endif
