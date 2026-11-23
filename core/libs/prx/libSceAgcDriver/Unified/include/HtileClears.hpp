#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_HTILECLEARS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_HTILECLEARS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace AgcDriver::Graphics {
class Recorder;
}

namespace AgcDriver::Unified::Images {

class DepthRecord;

struct HtileClear {
    VkBuffer tiles = VK_NULL_HANDLE;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    bool stencilInTiles = false;
    float clearDepth = 0.0f;
    std::uint8_t clearStencil = 0;
};

class HtileClears {
public:
    explicit HtileClears(const Graphics::Context& context);
    ~HtileClears();
    HtileClears(const HtileClears&) = delete;
    HtileClears& operator=(const HtileClears&) = delete;

    bool Apply(Graphics::Recorder& recorder, const std::shared_ptr<DepthRecord>& record, const HtileClear& clear, std::string& reason);

private:
    struct Pass {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline depth = VK_NULL_HANDLE;
        VkPipeline stencil = VK_NULL_HANDLE;
    };

    void createShared();
    Pass& passFor(VkFormat format, VkImageAspectFlags aspects);
    VkPipeline createPipeline(VkRenderPass renderPass, bool stencil);

    const Graphics::Context context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout scanLayout = VK_NULL_HANDLE;
    VkPipelineLayout clearLayout = VK_NULL_HANDLE;
    VkPipeline scan = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    std::map<VkFormat, Pass> passes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::uint64_t poolBatch = 0;
};

[[nodiscard]] bool HtileClearsOn();
[[nodiscard]] std::uint32_t ExpandedHtileWord(bool stencilInTiles);

}

#endif
