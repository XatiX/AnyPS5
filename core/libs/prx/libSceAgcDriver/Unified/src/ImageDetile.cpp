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
#include <stdexcept>
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

std::uint32_t copyLayers(const Graphics::SurfaceGeometry& geometry) {
    if (geometry.imageDepth > 1u) return std::max(geometry.layers, geometry.imageDepth);
    return std::min(geometry.layers, std::max(geometry.imageLayers, 1u));
}

}

SurfaceDetiler::SurfaceDetiler(const Graphics::Context& context) : context(context), detiler(std::make_unique<Graphics::TextureDetiler>(context)) {}

SurfaceDetiler::~SurfaceDetiler() = default;

void SurfaceDetiler::Record(Graphics::Recorder& recorder, SurfaceTable& table, std::span<const std::shared_ptr<Surface>> surfaces, const Refused& refused) {
    struct Served {
        const std::shared_ptr<Surface>* surface = nullptr;
        VulkanSurface* vulkan = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint64_t base = 0;
    };
    const auto stamp = Events::Shared().Serial();
    const auto imageStamp = Events::GpuWrites().Serial();
    std::vector<Served> served;
    served.reserve(surfaces.size());
    for (const auto& surface : surfaces) {
        auto* vulkan = dynamic_cast<VulkanSurface*>(surface->device.get());
        if (vulkan == nullptr) {
            counts.refused++;
            refused("surface without a Vulkan image");
            continue;
        }
        const auto& key = surface->key;
        Shadow::OwnedSpan span;
        if (!Shadow::ServeOwned(context, key.address, key.bytes, &span) || span.handle == 0u || key.address < span.origin) {
            counts.refused++;
            refused("surface memory not resident");
            continue;
        }
        served.push_back({&surface, vulkan, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle)), key.address - span.origin});
    }
    if (served.empty()) return;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    auto timing = Graphics::Recorder::NoTiming;
    try {
        const auto serial = recorder.OpenBatchSerial();
        if (serial != batch) {
            detiler->BeginBatch();
            batch = serial;
        }
        commands = recorder.Commands();
        timing = recorder.BeginGpuTiming(DetileTimingKey);
        std::vector<std::shared_ptr<void>> kept;
        if (Shadow::RecordOwnedUploads(commands, kept) != 0u) {
            for (auto& object : kept) recorder.Keep(std::move(object));
        }
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } catch (const std::exception& error) {
        recorder.EndGpuTiming(timing);
        counts.refused += served.size();
        refused(std::string("detile: ") + error.what());
        return;
    }
    for (const auto& one : served) {
        try {
            recordOne(recorder, commands, table, **one.surface, *one.vulkan, one.buffer, one.base, stamp, imageStamp);
            recorder.Keep(*one.surface);
        } catch (const std::exception& error) {
            counts.refused++;
            refused(std::string("detile: ") + error.what());
        }
    }
    recorder.EndGpuTiming(timing);
}

void SurfaceDetiler::recordOne(Graphics::Recorder& recorder, VkCommandBuffer commands, SurfaceTable& table, Surface& surface, VulkanSurface& vulkan, VkBuffer buffer, std::uint64_t base, std::uint64_t stamp, std::uint64_t imageStamp) {
    imageBarrier(context, commands, vulkan.Image(), vulkan.Initialized() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    const auto& resource = vulkan.Resource();
    const auto& geometry = vulkan.Geometry();
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    const auto layers = copyLayers(geometry);
    if (vulkan.Compressed() || geometry.thick || geometry.imageDepth > 1u) {
        std::string reason;
        if (!recordThroughBuffer(recorder, commands, vulkan, buffer, base, layers, reason)) throw std::runtime_error(reason);
    } else {
        Graphics::DetileWindow window;
        window.pipeBankXor = resource.pipeBankXor;
        for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
            const auto& mip = geometry.mips[level];
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                if (!geometry.HasLayer(level, layer)) continue;
                detiler->DispatchImage(commands, resource.tileMode, elementBytes, buffer, base + geometry.GuestOffset(layer, mip), vulkan.StorageView(level, layer, elementBytes), mip, false, layer, window);
                counts.dispatches++;
            }
        }
        imageBarrier(context, commands, vulkan.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    vulkan.SetInitialized();
    table.MarkReady(surface, stamp, imageStamp);
    counts.surfaces++;
    counts.bytes += surface.key.bytes;
}

bool SurfaceDetiler::recordThroughBuffer(Graphics::Recorder& recorder, VkCommandBuffer commands, VulkanSurface& surface, VkBuffer source, std::uint64_t base, std::uint32_t layers, std::string& reason) {
    const auto& resource = surface.Resource();
    const auto& geometry = surface.Geometry();
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    Graphics::DetileWindow window;
    window.pipeBankXor = resource.pipeBankXor;
    const auto blockWidth = std::max(Graphics::BlockWidth(resource.format), 1u);
    std::uint64_t total = 0;
    for (const auto& mip : geometry.mips) total = std::max(total, mip.linearOffset + mip.linearSize);
    const auto layerBytes = std::max<std::uint64_t>(geometry.sliceLinearBytes, total);
    std::vector<VkBufferImageCopy> regions;
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        if (mip.pitchBytes == 0u || mip.pitchBytes % elementBytes != 0u) {
            reason = "linear mip pitch";
            return false;
        }
        const auto rowTexels = mip.pitchBytes / elementBytes * blockWidth;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<std::uint64_t>(layer) * layerBytes + mip.linearOffset;
            region.bufferRowLength = rowTexels;
            region.bufferImageHeight = 0u;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, geometry.CopyLayer(layer), 1u};
            region.imageOffset = {0, 0, geometry.CopyDepth(layer)};
            region.imageExtent = {std::max(resource.width >> level, 1u), std::max(resource.height >> level, 1u), 1u};
            regions.push_back(region);
        }
    }
    auto scratch = std::make_shared<Graphics::DeviceBuffer>(context, static_cast<std::size_t>(layerBytes * layers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::size_t index = 0;
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            detiler->Dispatch(commands, resource.tileMode, elementBytes, source, base + geometry.GuestOffset(layer, mip), scratch->Handle(), regions[index++].bufferOffset, mip, false, layer, geometry.thick, window);
            counts.dispatches++;
        }
    }
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    imageBarrier(context, commands, surface.Image(), surface.Initialized() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    context.Resolved(&Graphics::DeviceFunctions::cmdCopyBufferToImage, "vkCmdCopyBufferToImage")(commands, scratch->Handle(), surface.Image(), VK_IMAGE_LAYOUT_GENERAL, static_cast<std::uint32_t>(regions.size()), regions.data());
    imageBarrier(context, commands, surface.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    recorder.Keep(scratch);
    return true;
}

bool SurfaceDetiler::WriteBack(Graphics::Recorder& recorder, const std::shared_ptr<Surface>& surface, std::string& reason) {
    auto* vulkan = dynamic_cast<VulkanSurface*>(surface->device.get());
    if (vulkan == nullptr) {
        reason = "record without a Vulkan image";
        return false;
    }
    if (!vulkan->Initialized()) {
        reason = "record never filled";
        return false;
    }
    if (vulkan->Compressed()) {
        reason = "block-compressed record";
        return false;
    }
    if (vulkan->Geometry().thick || vulkan->Geometry().imageDepth > 1u) {
        reason = "thick or volume record";
        return false;
    }
    const auto& key = surface->key;
    Shadow::OwnedSpan span;
    if (!Shadow::ServeOwned(context, key.address, key.bytes, &span) || span.handle == 0u || key.address < span.origin) {
        reason = "record memory not resident";
        return false;
    }
    const auto buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle));
    const auto base = key.address - span.origin;
    const auto serial = recorder.OpenBatchSerial();
    if (serial != batch) {
        detiler->BeginBatch();
        batch = serial;
    }
    const auto commands = recorder.Commands();
    const auto timing = recorder.BeginGpuTiming(DetileTimingKey);
    std::vector<std::shared_ptr<void>> kept;
    if (Shadow::RecordOwnedUploads(commands, kept) != 0u) {
        for (auto& object : kept) recorder.Keep(std::move(object));
    }
    imageBarrier(context, commands, vulkan->Image(), VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    const auto& resource = vulkan->Resource();
    const auto& geometry = vulkan->Geometry();
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    const auto layers = std::min(geometry.layers, std::max(geometry.imageLayers, 1u));
    Graphics::DetileWindow window;
    window.pipeBankXor = resource.pipeBankXor;
    try {
        for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
            const auto& mip = geometry.mips[level];
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                if (!geometry.HasLayer(level, layer)) continue;
                detiler->DispatchImage(commands, resource.tileMode, elementBytes, buffer, base + geometry.GuestOffset(layer, mip), vulkan->StorageView(level, layer, elementBytes), mip, true, layer, window);
                counts.writeBackDispatches++;
            }
        }
    } catch (const std::exception& error) {
        recorder.EndGpuTiming(timing);
        reason = std::string("write-back: ") + error.what();
        return false;
    }
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    recorder.EndGpuTiming(timing);
    recorder.Keep(surface);
    counts.writeBacks++;
    counts.writeBackBytes += key.bytes;
    return true;
}

DetileCounts SurfaceDetiler::TakeCounts() {
    const auto taken = counts;
    counts = {};
    return taken;
}

}
