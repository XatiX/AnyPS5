#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TARGETS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TARGETS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>

namespace AgcDriver::Graphics {
class StorageTexture;
}

namespace AgcDriver::Unified::Images {

struct TargetCounts {
    std::uint64_t noted = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t cpuWritten = 0;
};

class TargetTable {
public:
    void Note(const std::shared_ptr<Graphics::StorageTexture>& texture);
    void Note(std::weak_ptr<const Graphics::StorageTexture> texture, std::uint64_t address);
    [[nodiscard]] std::shared_ptr<Graphics::StorageTexture> Find(std::uint64_t address, std::uint64_t bytes, std::span<const std::uint32_t, 8> words, std::string& reason);
    [[nodiscard]] TargetCounts TakeCounts();

private:
    struct Entry {
        std::weak_ptr<const Graphics::StorageTexture> texture;
        std::uint64_t stamp = 0;
    };

    std::mutex mutex;
    std::unordered_map<std::uint64_t, Entry> entries;
    TargetCounts counts;
};

TargetTable& Targets();

class TargetView {
public:
    TargetView(const Graphics::Context& context, std::shared_ptr<Graphics::StorageTexture> texture, VkImageView view) : context(context), texture(std::move(texture)), view(view) {}
    ~TargetView();
    TargetView(const TargetView&) = delete;
    TargetView& operator=(const TargetView&) = delete;

    [[nodiscard]] VkImageView View() const { return view; }

private:
    const Graphics::Context& context;
    std::shared_ptr<Graphics::StorageTexture> texture;
    VkImageView view = VK_NULL_HANDLE;
};

class TargetViews {
public:
    explicit TargetViews(const Graphics::Context& context) : context(context) {}

    [[nodiscard]] std::shared_ptr<TargetView> View(const std::shared_ptr<Graphics::StorageTexture>& texture, std::span<const std::uint32_t, 8> words, std::uint32_t kind, std::string& reason);

private:
    const Graphics::Context& context;
    std::map<std::tuple<const Graphics::StorageTexture*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::weak_ptr<TargetView>> views;
};

}

#endif
