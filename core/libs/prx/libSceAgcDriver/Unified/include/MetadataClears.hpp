#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_METADATACLEARS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_METADATACLEARS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace AgcDriver::Graphics {
class Recorder;
class TextureDetiler;
}

namespace AgcDriver::Unified::Images {

class VulkanSurface;

struct MetadataCounts {
    std::uint64_t pendingDcc = 0;
    std::uint64_t pendingCmask = 0;
    std::uint64_t dccApplied = 0;
    std::uint64_t cmaskApplied = 0;
    std::uint64_t refused = 0;
};

class MetadataClears {
public:
    explicit MetadataClears(const Graphics::Context& context);
    ~MetadataClears();
    MetadataClears(const MetadataClears&) = delete;
    MetadataClears& operator=(const MetadataClears&) = delete;

    void NoteTarget(const Graphics::ColorTarget& color);
    void NoteWrite(std::uint64_t address, std::uint64_t size);
    [[nodiscard]] bool Pending(std::uint64_t target) const;
    bool Apply(Graphics::Recorder& recorder, std::uint64_t target, VulkanSurface& surface, std::string& reason);
    [[nodiscard]] MetadataCounts TakeCounts();

private:
    struct Target {
        std::uint64_t dcc = 0;
        std::uint64_t dccKeys = 0;
        std::uint64_t cmask = 0;
        std::uint64_t cmaskBytes = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::uint32_t elementBytes = 0;
        bool alphaOnMsb = false;
        std::array<std::uint32_t, 2> clearWords{};
        bool dccPending = false;
        bool cmaskPending = false;
    };

    bool applyDcc(Graphics::Recorder& recorder, const Target& target, VulkanSurface& surface, std::string& reason);
    bool applyCmask(Graphics::Recorder& recorder, const Target& target, VulkanSurface& surface, std::string& reason);
    void createPipeline();

    const Graphics::Context context;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    mutable std::mutex mutex;
    std::map<std::uint64_t, Target> targets;
    std::uint64_t batch = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::uint64_t poolBatch = 0;
    MetadataCounts counts;
};

[[nodiscard]] bool MetadataClearsOn();

}

#endif
