#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;

struct Case {
    const char* name;
    std::uint32_t format;
    TextureTileMode mode;
    TextureDimension dimension;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t layers;
    std::uint32_t mips;
};

void imageToGeneral(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout from, VkAccessFlags source, VkAccessFlags destination) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void roundTrip(const Context& context, TextureDetiler& detiler, const Case& one) {
    GuestTextureResource resource{};
    resource.baseAddress = 0x100000000ull;
    resource.width = one.width;
    resource.height = one.height;
    resource.depthOrLastArray = one.layers - 1u;
    resource.mipCount = one.mips;
    resource.lastLevel = one.mips - 1u;
    resource.tileMode = one.mode;
    resource.dimension = one.dimension;
    resource.format = one.format;
    resource.dstSelX = 4u;
    resource.dstSelY = 5u;
    resource.dstSelZ = 6u;
    resource.dstSelW = 7u;
    const auto geometry = DescribeSurface(resource);
    const auto format = SampledTextureFormat(context, one.format);
    Require(format != VK_FORMAT_UNDEFINED, std::string(one.name) + ": no Vulkan format");
    AgcDriver::Unified::Images::VulkanSurface surface(context, resource, geometry, format);
    const auto elementBytes = BytesPerElement(one.format);
    const auto bytes = static_cast<std::size_t>(geometry.guestBytes);
    const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Buffer original(context, bytes, usage);
    Buffer kept(context, bytes, usage);
    Buffer flipped(context, bytes, usage);
    std::uint32_t state = 0x9e3779b9u;
    for (std::size_t at = 0; at < bytes; ++at) {
        state = state * 1664525u + 1013904223u;
        original.Bytes()[at] = static_cast<std::byte>(state >> 24u);
        kept.Bytes()[at] = original.Bytes()[at];
        flipped.Bytes()[at] = ~original.Bytes()[at];
    }
    const auto layers = std::min(geometry.layers, std::max(geometry.imageLayers, 1u));
    DetileWindow window;
    CommandBatch batch(context);
    detiler.BeginBatch();
    imageToGeneral(context, batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_UNDEFINED, 0u, VK_ACCESS_SHADER_WRITE_BIT);
    std::uint64_t elements = 0;
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            detiler.DispatchImage(batch.Handle(), one.mode, elementBytes, original.Handle(), geometry.GuestOffset(layer, mip), surface.StorageView(level, layer, elementBytes), mip, false, layer, window);
            elements += static_cast<std::uint64_t>(std::max(one.width >> level, 1u)) * std::max(one.height >> level, 1u);
        }
    }
    imageToGeneral(context, batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    for (const auto* target : {&kept, &flipped}) {
        for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
            const auto& mip = geometry.mips[level];
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                if (!geometry.HasLayer(level, layer)) continue;
                detiler.DispatchImage(batch.Handle(), one.mode, elementBytes, target->Handle(), geometry.GuestOffset(layer, mip), surface.StorageView(level, layer, elementBytes), mip, true, layer, window);
            }
        }
    }
    const VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
    batch.SubmitAndWait();
    std::uint64_t touched = 0;
    std::uint64_t wrong = 0;
    std::size_t first = bytes;
    for (std::size_t at = 0; at < bytes; ++at) {
        if (kept.Bytes()[at] != flipped.Bytes()[at]) continue;
        ++touched;
        if (kept.Bytes()[at] == original.Bytes()[at]) continue;
        ++wrong;
        first = std::min(first, at);
    }
    const auto expected = elements * elementBytes;
    std::cout << one.name << ": " << geometry.mips.size() << " levels, " << layers << " layers, " << touched << " of " << bytes << " bytes written back (" << expected << " expected), " << wrong << " wrong" << '\n';
    Require(wrong == 0u, std::string(one.name) + ": " + std::to_string(wrong) + " bytes come back different, the first at 0x" + [&] { char text[24]; std::snprintf(text, sizeof(text), "%zx", first); return std::string(text); }());
    Require(touched >= expected, std::string(one.name) + ": the write-back covers " + std::to_string(touched) + " bytes, fewer than the " + std::to_string(expected) + " the levels hold");
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        {
            const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
            TextureDetiler detiler(context);
            using enum TextureTileMode;
            using enum TextureDimension;
            const Case cases[] = {
                {"R64KBX 2D 256x256 1 level, format 36", 36u, kR64KBX, k2D, 256u, 256u, 1u, 1u},
                {"R64KBX 2D 256x256 9 levels, format 36", 36u, kR64KBX, k2D, 256u, 256u, 1u, 9u},
                {"R64KBX cube 256x256 6 faces 9 levels, format 36", 36u, kR64KBX, kCube, 256u, 256u, 6u, 9u},
                {"R64KBX 2D array 256x256 4 layers 9 levels, format 71", 71u, kR64KBX, k2DArray, 256u, 256u, 4u, 9u},
                {"R64KBX 2D 2048x2048 12 levels, format 36", 36u, kR64KBX, k2D, 2048u, 2048u, 1u, 12u},
                {"D64KBX 2D 1024x1024 11 levels, format 22", 22u, kD64KBX, k2D, 1024u, 1024u, 1u, 11u},
            };
            int failed = 0;
            for (const auto& one : cases) {
                try {
                    roundTrip(context, detiler, one);
                } catch (const std::exception& error) {
                    std::cerr << "FAIL " << error.what() << '\n';
                    ++failed;
                }
            }
            device->WaitIdle();
            if (failed != 0) return 1;
        }
        device.reset();
        std::puts("recode image round-trip tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
