#include "prx/libSceAgcDriver/Unified/include/ImageDetile.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include <algorithm>
#include <exception>
#include <vector>

namespace AgcDriver::Unified::Images {

namespace {

void imageBarrier(const Graphics::Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout from, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = from;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
    context.Resolved(&Graphics::DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}

SurfaceDetiler::SurfaceDetiler(const Graphics::Context& context) : context(context), detiler(std::make_unique<Graphics::TextureDetiler>(context)) {}

SurfaceDetiler::~SurfaceDetiler() = default;

bool SurfaceDetiler::Record(Graphics::Recorder& recorder, SurfaceTable& table, const std::shared_ptr<Surface>& surface, std::string& reason) {
    auto* vulkan = dynamic_cast<VulkanSurface*>(surface->device.get());
    if (vulkan == nullptr) {
        reason = "surface without a Vulkan image";
        counts.refused++;
        return false;
    }
    const auto& key = surface->key;
    const auto stamp = Events::Shared().Serial();
    const auto imageStamp = Events::Images().Serial();
    Shadow::OwnedSpan span;
    if (!Shadow::ServeOwned(context, key.address, key.bytes, &span) || span.handle == 0u || key.address < span.origin) {
        reason = "surface memory not resident";
        counts.refused++;
        return false;
    }
    try {
        const auto serial = recorder.OpenBatchSerial();
        if (serial != batch) {
            detiler->BeginBatch();
            batch = serial;
        }
        auto commands = recorder.Commands();
        std::vector<std::shared_ptr<void>> kept;
        if (Shadow::RecordOwnedUploads(commands, kept) != 0u) {
            for (auto& object : kept) recorder.Keep(std::move(object));
        }
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        imageBarrier(context, commands, vulkan->Image(), vulkan->Initialized() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        const auto& resource = vulkan->Resource();
        const auto& geometry = vulkan->Geometry();
        const auto elementBytes = Graphics::BytesPerElement(resource.format);
        const auto buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle));
        const auto base = key.address - span.origin;
        const auto layers = std::min(geometry.layers, std::max(geometry.imageLayers, 1u));
        for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
            const auto& mip = geometry.mips[level];
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                if (!geometry.HasLayer(level, layer)) continue;
                detiler->DispatchImage(commands, resource.tileMode, elementBytes, buffer, base + geometry.GuestOffset(layer, mip), vulkan->StorageView(level, layer, elementBytes), mip, false, 0);
                counts.dispatches++;
            }
        }
        imageBarrier(context, commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        recorder.Keep(surface);
        vulkan->SetInitialized();
    } catch (const std::exception& error) {
        reason = std::string("detile: ") + error.what();
        counts.refused++;
        return false;
    }
    table.MarkReady(*surface, stamp, imageStamp);
    counts.surfaces++;
    counts.bytes += key.bytes;
    return true;
}

DetileCounts SurfaceDetiler::TakeCounts() {
    const auto taken = counts;
    counts = {};
    return taken;
}

}
