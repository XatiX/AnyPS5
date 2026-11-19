#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGEDETILE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace AgcDriver::Graphics {
class Recorder;
class TextureDetiler;
}

namespace AgcDriver::Unified::Images {

class VulkanSurface;

struct DetileCounts {
    std::uint64_t surfaces = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t bytes = 0;
    std::uint64_t refused = 0;
};

class SurfaceDetiler {
public:
    explicit SurfaceDetiler(const Graphics::Context& context);
    ~SurfaceDetiler();
    SurfaceDetiler(const SurfaceDetiler&) = delete;
    SurfaceDetiler& operator=(const SurfaceDetiler&) = delete;

    using Refused = std::function<void(const std::string&)>;

    void Record(Graphics::Recorder& recorder, SurfaceTable& table, std::span<const std::shared_ptr<Surface>> surfaces, const Refused& refused);
    [[nodiscard]] DetileCounts TakeCounts();

private:
    void recordOne(Graphics::Recorder& recorder, VkCommandBuffer commands, SurfaceTable& table, Surface& surface, VulkanSurface& vulkan, VkBuffer buffer, std::uint64_t base, std::uint64_t stamp, std::uint64_t imageStamp);
    bool recordThroughBuffer(Graphics::Recorder& recorder, VkCommandBuffer commands, VulkanSurface& surface, VkBuffer source, std::uint64_t base, std::uint32_t layers, std::string& reason);

    const Graphics::Context& context;
    std::unique_ptr<Graphics::TextureDetiler> detiler;
    std::uint64_t batch = 0;
    DetileCounts counts;
};

}

#endif
