#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {
class Recorder;
class TextureDetiler;
}

namespace AgcDriver::Unified::Images {

class VulkanSurface;
class DepthRecord;

inline constexpr std::uint64_t DetileTimingKey = 0x7ec0de01;

struct DetileCounts {
    std::uint64_t surfaces = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t bytes = 0;
    std::uint64_t refused = 0;
    std::uint64_t writeBacks = 0;
    std::uint64_t writeBackBytes = 0;
    std::uint64_t writeBackDispatches = 0;
    std::uint64_t depthWriteBacks = 0;
    std::uint64_t depthWriteBackBytes = 0;
    std::uint64_t depthSeeds = 0;
};

class SurfaceDetiler {
public:
    explicit SurfaceDetiler(const Graphics::Context& context);
    ~SurfaceDetiler();
    SurfaceDetiler(const SurfaceDetiler&) = delete;
    SurfaceDetiler& operator=(const SurfaceDetiler&) = delete;

    using Refused = std::function<void(const std::string&)>;

    void Record(Graphics::Recorder& recorder, SurfaceTable& table, std::span<const std::shared_ptr<Surface>> surfaces, const Refused& refused);
    bool WriteBack(Graphics::Recorder& recorder, const std::shared_ptr<Surface>& surface, std::string& reason);
    bool WriteBackDepth(Graphics::Recorder& recorder, const std::shared_ptr<DepthRecord>& record, std::vector<std::pair<std::uint64_t, std::uint64_t>>& written, std::string& reason);
    bool SeedDepth(Graphics::Recorder& recorder, const std::shared_ptr<DepthRecord>& record, std::string& reason);
    [[nodiscard]] DetileCounts TakeCounts();

private:
    struct DepthPlane {
        std::uint64_t address = 0;
        std::uint32_t format = 0;
        std::uint32_t elementBytes = 0;
        VkImageAspectFlagBits aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        Graphics::SurfaceGeometry geometry;
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint64_t base = 0;
        std::uint64_t staged = 0;
    };

    bool depthPlanes(const DepthRecord& record, std::vector<DepthPlane>& planes, std::uint64_t& stagingBytes, std::string& reason);
    VkCommandBuffer depthCommands(Graphics::Recorder& recorder);
    void recordOne(Graphics::Recorder& recorder, VkCommandBuffer commands, SurfaceTable& table, Surface& surface, VulkanSurface& vulkan, VkBuffer buffer, std::uint64_t base, std::uint64_t stamp, std::uint64_t imageStamp);
    bool recordThroughBuffer(Graphics::Recorder& recorder, VkCommandBuffer commands, VulkanSurface& surface, VkBuffer source, std::uint64_t base, std::uint32_t layers, std::string& reason);
    void writeBackLine(Graphics::Recorder& recorder, VkCommandBuffer commands, VulkanSurface& surface, VkBuffer destination, std::uint64_t base, std::uint32_t layers);

    const Graphics::Context context;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    std::uint64_t batch = 0;
    DetileCounts counts;
};

}

#endif
