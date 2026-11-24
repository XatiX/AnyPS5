#pragma once

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Images {

struct DepthKey {
    std::uint64_t address = 0;
    std::uint64_t stencilAddress = 0;
    std::uint64_t htileAddress = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t slice = 0;

    bool operator<(const DepthKey& other) const {
        return std::tie(address, stencilAddress, htileAddress, width, height, format, slice) < std::tie(other.address, other.stencilAddress, other.htileAddress, other.width, other.height, other.format, other.slice);
    }
};

[[nodiscard]] constexpr std::uint64_t HtileBlockBytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::uint64_t>((width + 1023u) / 1024u) * ((height + 511u) / 512u) * 32768u;
}

class DepthRecord {
public:
    DepthRecord(const Graphics::Context& context, const DepthKey& key);
    ~DepthRecord();
    DepthRecord(const DepthRecord&) = delete;
    DepthRecord& operator=(const DepthRecord&) = delete;

    [[nodiscard]] const DepthKey& Key() const { return key; }
    [[nodiscard]] VkImage Image() const { return image; }
    [[nodiscard]] VkImageView View() const { return view; }
    [[nodiscard]] bool Initialized() const { return initialized; }
    void Initialize(VkCommandBuffer commands, float clearDepth, std::uint8_t clearStencil);
    [[nodiscard]] VkImageView SampledView(bool stencil);
    [[nodiscard]] VkImageAspectFlags Aspects() const { return aspects; }
    void SetTiling(std::uint32_t swizzleMode, std::uint32_t xorBits) {
        swizzle = swizzleMode;
        pipeBankXor = xorBits;
    }
    [[nodiscard]] std::uint32_t Swizzle() const { return swizzle; }
    [[nodiscard]] std::uint32_t PipeBankXor() const { return pipeBankXor; }
    void MarkDrawn() { drawn.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool TakeDrawn() { return drawn.exchange(false, std::memory_order_relaxed); }
    void SetInitialized() { initialized = true; }
    void NoteHtileWrite() { htileWritten.store(true, std::memory_order_release); }
    [[nodiscard]] bool TakeHtileWrite() { return htileWritten.exchange(false, std::memory_order_acq_rel); }
    void NoteGpuHtileWrite() { htileGpuWritten.store(true, std::memory_order_release); }
    [[nodiscard]] bool TakeGpuHtileWrite() { return htileGpuWritten.exchange(false, std::memory_order_acq_rel); }
    void NotePlaneWrite() { planeWritten.store(true, std::memory_order_release); }
    [[nodiscard]] bool TakePlaneWrite() { return planeWritten.exchange(false, std::memory_order_acq_rel); }
    void SetHtileSaysCleared(bool cleared) { htileSaysCleared = cleared; }
    [[nodiscard]] bool HtileSaysCleared() const { return htileSaysCleared; }
    [[nodiscard]] std::uint64_t HtileBytes() const { return HtileBlockBytes(key.width, key.height); }
    void SetPredecessors(std::vector<std::shared_ptr<DepthRecord>> records) { predecessors = std::move(records); }
    [[nodiscard]] std::vector<std::shared_ptr<DepthRecord>> TakePredecessors() { return std::exchange(predecessors, {}); }

private:
    std::vector<std::shared_ptr<DepthRecord>> predecessors;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkCreateImageView createView = nullptr;
    PFN_vkDestroyImageView destroyView = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkCmdPipelineBarrier pipelineBarrier = nullptr;
    PFN_vkCmdClearDepthStencilImage clearImage = nullptr;
    DepthKey key;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImageView stencilView = VK_NULL_HANDLE;
    VkImageAspectFlags aspects = 0;
    bool initialized = false;
    std::uint32_t swizzle = 0;
    std::uint32_t pipeBankXor = 0;
    std::atomic<bool> drawn{false};
    std::atomic<bool> htileWritten{false};
    std::atomic<bool> htileGpuWritten{false};
    std::atomic<bool> planeWritten{false};
    bool htileSaysCleared = false;
};

enum class HtileState {
    Cleared,
    Written,
    Unknown,
};

[[nodiscard]] HtileState ReadHtile(std::uint64_t address, std::uint64_t bytes, bool depthOnly, bool stencil, float clearDepth);

struct DepthCounts {
    std::uint64_t created = 0;
    std::uint64_t found = 0;
    std::uint64_t refused = 0;
    std::uint64_t cleared = 0;
    std::uint64_t htileWrites = 0;
    std::uint64_t gpuHtileWrites = 0;
    std::uint64_t cpuPlaneWrites = 0;
    std::uint64_t superseded = 0;
};

class DepthRecords {
public:
    [[nodiscard]] std::shared_ptr<DepthRecord> Find(const Graphics::Context& context, const Graphics::DepthTarget& target, std::string& reason);
    [[nodiscard]] std::shared_ptr<DepthRecord> At(std::uint64_t address, bool& stencil);
    [[nodiscard]] bool Overlaps(std::uint64_t address, std::uint64_t bytes) const;
    [[nodiscard]] std::vector<std::shared_ptr<DepthRecord>> Overlapping(std::uint64_t address, std::uint64_t bytes) const;
    std::size_t NoteHtileWrites(std::uint64_t address, std::uint64_t bytes);
    std::size_t NoteGpuHtileWrites(std::uint64_t address, std::uint64_t bytes);
    std::size_t NoteCpuWrites(std::uint64_t address, std::uint64_t bytes);
    void NoteCleared() {
        std::lock_guard lock(mutex);
        counts.cleared++;
    }
    [[nodiscard]] DepthCounts Counts() const {
        std::lock_guard lock(mutex);
        return counts;
    }
    void Clear() {
        std::lock_guard lock(mutex);
        records.clear();
    }
    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex);
        return records.size();
    }

private:
    mutable std::mutex mutex;
    std::map<DepthKey, std::shared_ptr<DepthRecord>> records;
    DepthCounts counts;
};

DepthRecords& Depths();
[[nodiscard]] bool DepthRecordsOn();

}
