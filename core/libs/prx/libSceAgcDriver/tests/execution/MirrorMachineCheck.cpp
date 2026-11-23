#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::Require;

constexpr VkDeviceSize ChunkBytes = 64u << 10u;
constexpr VkDeviceSize PoolBytes = 256u << 20u;
constexpr VkDeviceSize BufferBytes = 1ull << 30u;
constexpr int Samples = 50;
constexpr std::array<VkDeviceSize, 3> Sizes{4u << 10u, 64u << 10u, 1u << 20u};
constexpr std::array<std::uint32_t, 3> BindCounts{1, 16, 256};
constexpr std::uint32_t FillWord = 0xa5c3e1f0u;

double median(std::vector<double> values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
}

double microseconds(std::chrono::steady_clock::duration elapsed) {
    return std::chrono::duration<double, std::micro>(elapsed).count();
}

std::optional<std::uint32_t> findType(const Context& context, std::uint32_t bits, VkMemoryPropertyFlags wanted) {
    for (std::uint32_t index = 0; index < context.memory.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0 && (context.memory.memoryTypes[index].propertyFlags & wanted) == wanted) return index;
    }
    return std::nullopt;
}

class Check {
public:
    explicit Check(const Context& context) : context(context) {
        fence = createFence();
    }

    ~Check() {
        const auto device = context.device;
        if (mapped != nullptr) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(device, pool);
        if (sparse != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, sparse, nullptr);
        if (staging != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, staging, nullptr);
        if (stagingMemory != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(device, stagingMemory, nullptr);
        if (pool != VK_NULL_HANDLE) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(device, pool, nullptr);
        if (fence != VK_NULL_HANDLE) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
    }

    Check(const Check&) = delete;
    Check& operator=(const Check&) = delete;

    void Run() {
        createSparse();
        const auto hostType = findType(context, sparseTypes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        const auto deviceType = findType(context, sparseTypes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        std::printf("[mirror-check] 1 sparse buffer memoryTypeBits 0x%x; device-local host-visible type %s", sparseTypes, hostType ? "yes" : "NO");
        if (hostType) {
            const auto flags = context.memory.memoryTypes[*hostType].propertyFlags;
            const auto heap = context.memory.memoryTypes[*hostType].heapIndex;
            std::printf(" (type %u, heap %u of %llu MiB, coherent %s, cached %s)", *hostType, heap, static_cast<unsigned long long>(context.memory.memoryHeaps[heap].size >> 20u), (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0 ? "yes" : "no", (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? "yes" : "no");
        }
        std::printf("\n");
        Require(deviceType.has_value(), "the sparse buffer takes no device-local memory type");
        const auto type = hostType ? *hostType : *deviceType;
        coherent = (context.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        createPool(type, hostType.has_value());
        createStaging();
        Require(bind(0, BindCounts.back(), true), "binding the first chunks failed");
        if (mapped != nullptr) {
            mappedChecks();
            readTimes();
            writeTimes();
        } else {
            std::printf("[mirror-check] 2 no mapping: decision 8 not available on this device\n");
            readBackTimes();
        }
        bindTimes();
    }

private:
    VkFence createFence() {
        const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence made = VK_NULL_HANDLE;
        Require(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &made) == VK_SUCCESS, "vkCreateFence");
        return made;
    }

    void waitFence() {
        Require(context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "vkWaitForFences");
        Require(context.Function<PFN_vkResetFences>("vkResetFences")(context.device, 1, &fence) == VK_SUCCESS, "vkResetFences");
    }

    void createSparse() {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        info.size = BufferBytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::array<std::uint32_t, 2> families{context.queueFamily, context.sparseFamily};
        if (context.sparseQueue != VK_NULL_HANDLE && context.sparseFamily != context.queueFamily) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
            info.pQueueFamilyIndices = families.data();
        }
        Require(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &sparse) == VK_SUCCESS, "vkCreateBuffer sparse");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, sparse, &requirements);
        Require(requirements.alignment != 0 && ChunkBytes % requirements.alignment == 0, "sparse alignment does not divide the chunk");
        sparseTypes = requirements.memoryTypeBits;
    }

    void createPool(std::uint32_t type, bool map) {
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        info.allocationSize = PoolBytes;
        info.memoryTypeIndex = type;
        const auto allocated = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &info, nullptr, &pool);
        Require(allocated == VK_SUCCESS, "vkAllocateMemory pool");
        if (!map) return;
        void* pointer = nullptr;
        const auto result = context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, pool, 0, VK_WHOLE_SIZE, 0, &pointer);
        std::printf("[mirror-check] 2a a %llu MiB pool of that type %s\n", static_cast<unsigned long long>(PoolBytes >> 20u), result == VK_SUCCESS ? "maps" : "does NOT map");
        if (result == VK_SUCCESS) mapped = static_cast<std::byte*>(pointer);
    }

    void createStaging() {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = Sizes.back();
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Require(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &staging) == VK_SUCCESS, "vkCreateBuffer staging");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, staging, &requirements);
        auto type = findType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (!type) type = findType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Require(type.has_value(), "no host-visible staging type");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = *type;
        Require(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocate, nullptr, &stagingMemory) == VK_SUCCESS, "vkAllocateMemory staging");
        Require(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, staging, stagingMemory, 0) == VK_SUCCESS, "vkBindBufferMemory staging");
        void* pointer = nullptr;
        Require(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS, "vkMapMemory staging");
        stagingBytes = static_cast<std::byte*>(pointer);
    }

    bool bind(std::uint32_t first, std::uint32_t count, bool bound) {
        std::vector<VkSparseMemoryBind> binds;
        binds.reserve(count);
        for (std::uint32_t index = first; index < first + count; ++index) binds.push_back({index * ChunkBytes, ChunkBytes, bound ? pool : VK_NULL_HANDLE, bound ? index * ChunkBytes : 0, 0});
        const VkSparseBufferMemoryBindInfo bufferBinds{sparse, count, binds.data()};
        VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        info.bufferBindCount = 1;
        info.pBufferBinds = &bufferBinds;
        const auto queue = context.sparseQueue != VK_NULL_HANDLE ? context.sparseQueue : context.queue;
        if (context.Function<PFN_vkQueueBindSparse>("vkQueueBindSparse")(queue, 1, &info, fence) != VK_SUCCESS) return false;
        waitFence();
        return true;
    }

    void submit(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = context.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        Require(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocate, &commands) == VK_SUCCESS, "vkAllocateCommandBuffers");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        Require(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin) == VK_SUCCESS, "vkBeginCommandBuffer");
        record(commands);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        Require(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands) == VK_SUCCESS, "vkEndCommandBuffer");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &commands;
        Require(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &info, fence) == VK_SUCCESS, "vkQueueSubmit");
        waitFence();
        context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    }

    void invalidate() {
        if (coherent) return;
        const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, pool, 0, VK_WHOLE_SIZE};
        context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range);
    }

    void flush() {
        if (coherent) return;
        const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, pool, 0, VK_WHOLE_SIZE};
        context.Function<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(context.device, 1, &range);
    }

    void mappedChecks() {
        submit([&](VkCommandBuffer commands) { context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, sparse, ChunkBytes, ChunkBytes, FillWord); });
        invalidate();
        bool seen = true;
        for (VkDeviceSize at = 0; at < ChunkBytes; at += sizeof(FillWord)) {
            std::uint32_t word = 0;
            std::memcpy(&word, mapped + ChunkBytes + at, sizeof(word));
            seen = seen && word == FillWord;
        }
        std::printf("[mirror-check] 2b a GPU fill of a bound chunk %s through the mapping after the fence%s\n", seen ? "is seen" : "is NOT seen", coherent ? " (coherent, no further call)" : " (after an invalidate)");
        for (VkDeviceSize at = 0; at < ChunkBytes; ++at) mapped[2 * ChunkBytes + at] = static_cast<std::byte>(at * 7u + 3u);
        flush();
        submit([&](VkCommandBuffer commands) {
            const VkBufferCopy region{2 * ChunkBytes, 0, ChunkBytes};
            context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, sparse, staging, 1, &region);
        });
        bool copied = true;
        for (VkDeviceSize at = 0; at < ChunkBytes; ++at) copied = copied && stagingBytes[at] == static_cast<std::byte>(at * 7u + 3u);
        std::printf("[mirror-check] 2c a CPU write through the mapping %s by a GPU copy\n", copied ? "is seen" : "is NOT seen");
    }

    void readTimes() {
        std::vector<std::byte> host(Sizes.back());
        for (const auto size : Sizes) {
            std::vector<double> mappedTimes;
            std::vector<double> copyTimes;
            for (int sample = 0; sample < Samples; ++sample) {
                const auto start = std::chrono::steady_clock::now();
                invalidate();
                std::memcpy(host.data(), mapped + 4 * ChunkBytes, size);
                mappedTimes.push_back(microseconds(std::chrono::steady_clock::now() - start));
                copyTimes.push_back(readBack(size, host));
            }
            std::printf("[mirror-check] 3 read %llu KiB: mapping %.1f us, GPU copy to staging %.1f us (median of %d)\n", static_cast<unsigned long long>(size >> 10u), median(mappedTimes), median(copyTimes), Samples);
        }
    }

    void readBackTimes() {
        std::vector<std::byte> host(Sizes.back());
        for (const auto size : Sizes) {
            std::vector<double> copyTimes;
            for (int sample = 0; sample < Samples; ++sample) copyTimes.push_back(readBack(size, host));
            std::printf("[mirror-check] 3 read %llu KiB: GPU copy to staging %.1f us (median of %d)\n", static_cast<unsigned long long>(size >> 10u), median(copyTimes), Samples);
        }
    }

    double readBack(VkDeviceSize size, std::vector<std::byte>& host) {
        const auto start = std::chrono::steady_clock::now();
        submit([&](VkCommandBuffer commands) {
            const VkBufferCopy region{4 * ChunkBytes, 0, size};
            context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, sparse, staging, 1, &region);
        });
        std::memcpy(host.data(), stagingBytes, size);
        return microseconds(std::chrono::steady_clock::now() - start);
    }

    void writeTimes() {
        std::vector<std::byte> host(Sizes.back(), std::byte{0x5a});
        for (const auto size : Sizes) {
            std::vector<double> times;
            for (int sample = 0; sample < Samples; ++sample) {
                const auto start = std::chrono::steady_clock::now();
                std::memcpy(mapped + 4 * ChunkBytes, host.data(), size);
                flush();
                times.push_back(microseconds(std::chrono::steady_clock::now() - start));
            }
            std::printf("[mirror-check] 4 write %llu KiB through the mapping: %.1f us (median of %d)\n", static_cast<unsigned long long>(size >> 10u), median(times), Samples);
        }
    }

    void bindTimes() {
        Require(bind(0, BindCounts.back(), false), "unbinding the first chunks failed");
        for (const auto count : BindCounts) {
            std::vector<double> binds;
            std::vector<double> unbinds;
            for (int sample = 0; sample < Samples; ++sample) {
                auto start = std::chrono::steady_clock::now();
                Require(bind(0, count, true), "vkQueueBindSparse bind");
                binds.push_back(microseconds(std::chrono::steady_clock::now() - start));
                start = std::chrono::steady_clock::now();
                Require(bind(0, count, false), "vkQueueBindSparse unbind");
                unbinds.push_back(microseconds(std::chrono::steady_clock::now() - start));
            }
            std::printf("[mirror-check] 5 vkQueueBindSparse with fence wait, %u chunk%s: bind %.1f us, unbind %.1f us (median of %d, %s queue)\n", count, count == 1 ? "" : "s", median(binds), median(unbinds), Samples, context.sparseQueue != VK_NULL_HANDLE ? "sparse" : "main");
        }
    }

    const Context& context;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer sparse = VK_NULL_HANDLE;
    std::uint32_t sparseTypes = 0;
    VkDeviceMemory pool = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
    bool coherent = true;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    std::byte* stagingBytes = nullptr;
};

bool SparseFits(const Context& context, std::uint64_t bytes, int count) {
    std::vector<VkBuffer> made;
    bool all = true;
    for (int index = 0; index < count && all; ++index) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        all = context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) == VK_SUCCESS;
        if (all) made.push_back(buffer);
    }
    for (const auto buffer : made) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    return all;
}

void LargestSparse(const Context& context) {
    constexpr std::uint64_t Step = std::uint64_t{1} << 30;
    std::uint64_t low = 0;
    std::uint64_t high = std::uint64_t{1} << 40;
    while (high - low > Step) {
        const auto middle = (low + (high - low) / 2) & ~(Step - 1);
        (SparseFits(context, middle, 1) ? low : high) = middle;
    }
    std::uint32_t segments = 0;
    while (segments < 32 && SparseFits(context, std::uint64_t{1} << 36, static_cast<int>(segments) + 1)) ++segments;
    std::printf("largest single sparse buffer: %llu GiB; 64 GiB sparse buffers that fit at once: %u%s\n", static_cast<unsigned long long>(low >> 30), segments, segments == 32 ? " or more" : "");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        if (!context.sparseBinding || !context.sparseResidencyBuffer || !context.bufferDeviceAddress) {
            std::puts("skipped, the device has no sparse residency buffers or device addresses");
            return VulkanTestSkipped;
        }
        LargestSparse(context);
        Check check(context);
        check.Run();
        std::puts("mirror machine check finished");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mirror machine check failed: %s\n", error.what());
        return 1;
    }
}
