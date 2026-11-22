#pragma once

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

namespace AgcDriver::Unified::Images {

struct DepthKey {
    std::uint64_t address = 0;
    std::uint64_t stencilAddress = 0;
    std::uint64_t htileAddress = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool operator<(const DepthKey& other) const {
        return std::tie(address, stencilAddress, htileAddress, width, height, format) < std::tie(other.address, other.stencilAddress, other.htileAddress, other.width, other.height, other.format);
    }
};

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

private:
    const Graphics::Context& context;
    DepthKey key;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageAspectFlags aspects = 0;
    bool initialized = false;
};

struct DepthCounts {
    std::uint64_t created = 0;
    std::uint64_t found = 0;
    std::uint64_t refused = 0;
    std::uint64_t cleared = 0;
};

class DepthRecords {
public:
    [[nodiscard]] std::shared_ptr<DepthRecord> Find(const Graphics::Context& context, const Graphics::DepthTarget& target, std::string& reason);
    void NoteCleared() {
        std::lock_guard lock(mutex);
        counts.cleared++;
    }
    [[nodiscard]] DepthCounts Counts() const {
        std::lock_guard lock(mutex);
        return counts;
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
