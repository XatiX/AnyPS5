#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "SpirvBackend/SpirvModule.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

void transitionImage(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags destination, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, VK_REMAINING_MIP_LEVELS, 0u, VK_REMAINING_ARRAY_LAYERS};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void imageToGeneral(const Context& context, VkCommandBuffer commands, VkImage image, VkImageLayout from, VkAccessFlags source, VkAccessFlags destination) {
    transitionImage(context, commands, image, from, VK_IMAGE_LAYOUT_GENERAL, source, destination, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

std::vector<std::uint32_t> writeFirstLayerTexelShader(std::uint32_t value) {
    ShaderRecompiler::SpirvModule module;
    module.EmitCapability(spv::CapabilityShader);
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto u32 = module.Type(spv::OpTypeInt, 32u, 0u);
    const auto i32 = module.Type(spv::OpTypeInt, 32u, 1u);
    const auto uvec4 = module.Type(spv::OpTypeVector, u32, 4u);
    const auto ivec3 = module.Type(spv::OpTypeVector, i32, 3u);
    const auto voidType = module.Type(spv::OpTypeVoid);
    const auto imageType = module.Type(spv::OpTypeImage, u32, spv::Dim2D, 0u, 1u, 0u, 2u, spv::ImageFormatR32ui);
    const auto imagePtr = module.Type(spv::OpTypePointer, spv::StorageClassUniformConstant, imageType);
    const auto imageVar = module.DefineGlobalVariable(imagePtr, spv::StorageClassUniformConstant);
    module.AddAnnotation(spv::OpDecorate, imageVar, spv::DecorationDescriptorSet, 0u);
    module.AddAnnotation(spv::OpDecorate, imageVar, spv::DecorationBinding, 0u);
    const auto zeroI32 = module.Constant(spv::OpConstant, i32, 0u);
    const auto zeroU32 = module.Constant(spv::OpConstant, u32, 0u);
    const auto magicU32 = module.Constant(spv::OpConstant, u32, value);
    const auto coord = module.Constant(spv::OpConstantComposite, ivec3, zeroI32, zeroI32, zeroI32);
    const auto texel = module.Constant(spv::OpConstantComposite, uvec4, magicU32, zeroU32, zeroU32, zeroU32);
    const auto entry = module.AllocateId();
    module.AddFunction(spv::OpFunction, voidType, entry, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto loaded = module.AllocateId();
    module.AddFunction(spv::OpLoad, imageType, loaded, imageVar);
    module.AddFunction(spv::OpImageWrite, loaded, coord, texel);
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    module.EmitEntryPoint(spv::ExecutionModelGLCompute, entry, "main", {});
    module.AddExecutionMode(entry, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
    return module.Finalize();
}

struct ArrayStoragePipeline {
    explicit ArrayStoragePipeline(const Context& context, VkImageView view) : context(context) {
        try {
            const VkDescriptorSetLayoutBinding binding{0u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setLayoutInfo.bindingCount = 1u;
            setLayoutInfo.pBindings = &binding;
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setLayoutInfo, nullptr, &setLayout), "array storage descriptor layout");
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 1u;
            poolInfo.pPoolSizes = &poolSize;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "array storage descriptor pool");
            const VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1u, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocate, &set), "array storage descriptor set");
            const VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &image, nullptr, nullptr};
            context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1u, &write, 0u, nullptr);
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1u;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "array storage pipeline layout");
            const auto code = writeFirstLayerTexelShader(0x12345678u);
            VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            shaderInfo.codeSize = code.size() * sizeof(std::uint32_t);
            shaderInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &shaderInfo, nullptr, &module), "array storage shader module");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.layout = layout;
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline), "array storage pipeline");
        } catch (...) {
            release();
            throw;
        }
    }

    ~ArrayStoragePipeline() { release(); }
    ArrayStoragePipeline(const ArrayStoragePipeline&) = delete;
    ArrayStoragePipeline& operator=(const ArrayStoragePipeline&) = delete;

    void Run(VkCommandBuffer commands) const {
        context.BindComputePipeline(commands, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0u, 1u, &set, 0u, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1u, 1u, 1u);
    }

private:
    void release() noexcept {
        if (pipeline != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module != VK_NULL_HANDLE) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }

    Context context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

void testArrayStorageBaseLayer(const Context& context) {
    using AgcDriver::Unified::Images::StorageShape;
    using AgcDriver::Unified::Images::VulkanSurface;
    GuestTextureResource cube{};
    cube.baseAddress = 0x200000000ull;
    cube.width = 16u;
    cube.height = 16u;
    cube.depthOrLastArray = 5u;
    cube.mipCount = 1u;
    cube.lastLevel = 0u;
    cube.tileMode = TextureTileMode::kR64KBX;
    cube.dimension = TextureDimension::kCube;
    cube.format = 20u;
    cube.dstSelX = 4u;
    cube.dstSelY = 5u;
    cube.dstSelZ = 6u;
    cube.dstSelW = 7u;
    const auto cubeFormat = SampledTextureFormat(context, cube.format);
    Require(cubeFormat != VK_FORMAT_UNDEFINED, "array storage base layer: no R32_UINT Vulkan format");
    VulkanSurface surface(context, cube, DescribeSurface(cube), cubeFormat);
    std::string reason;
    const auto view0 = surface.ShapedStorageView(0u, StorageShape::Array, false, false, reason, 0u);
    Require(view0 != VK_NULL_HANDLE, "array storage base layer: view0 failed: " + reason);
    const auto view3 = surface.ShapedStorageView(0u, StorageShape::Array, false, false, reason, 3u);
    Require(view3 != VK_NULL_HANDLE && view3 != view0, "array storage base layer: view3 collapsed or failed: " + reason);
    Require(surface.ShapedStorageView(0u, StorageShape::Array, false, false, reason, 6u) == VK_NULL_HANDLE && reason == "storage view past the layers", "array storage base layer: out-of-range cube layer accepted");

    GuestTextureResource lineArray{};
    lineArray.baseAddress = 0x210000000ull;
    lineArray.width = 16u;
    lineArray.height = 1u;
    lineArray.depthOrLastArray = 3u;
    lineArray.mipCount = 1u;
    lineArray.lastLevel = 0u;
    lineArray.tileMode = TextureTileMode::kLinear;
    lineArray.dimension = TextureDimension::k1DArray;
    lineArray.format = 20u;
    lineArray.dstSelX = 4u;
    lineArray.dstSelY = 5u;
    lineArray.dstSelZ = 6u;
    lineArray.dstSelW = 7u;
    VulkanSurface lineSurface(context, lineArray, DescribeSurface(lineArray), cubeFormat);
    reason.clear();
    const auto line0 = lineSurface.ShapedStorageView(0u, StorageShape::LineArray, false, false, reason, 0u);
    Require(line0 != VK_NULL_HANDLE, "line array storage base layer: line0 failed: " + reason);
    const auto line2 = lineSurface.ShapedStorageView(0u, StorageShape::LineArray, false, false, reason, 2u);
    Require(line2 != VK_NULL_HANDLE && line2 != line0, "line array storage base layer: line2 collapsed or failed: " + reason);
    Require(lineSurface.ShapedStorageView(0u, StorageShape::LineArray, false, false, reason, 4u) == VK_NULL_HANDLE && reason == "storage view past the layers", "line array storage base layer: out-of-range layer accepted");

    constexpr std::uint32_t side = 16u;
    constexpr std::uint32_t layers = 6u;
    constexpr std::size_t texelsPerLayer = side * side;
    Buffer readback(context, texelsPerLayer * layers * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memset(readback.Bytes().data(), 0xff, readback.Bytes().size());
    ArrayStoragePipeline writer(context, view3);
    CommandBatch batch(context);
    transitionImage(context, batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0u, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange allLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, layers};
    context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &allLayers);
    transitionImage(context, batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    writer.Run(batch.Handle());
    transitionImage(context, batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkBufferImageCopy region{0u, 0u, 0u, {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, layers}, {0, 0, 0}, {side, side, 1u}};
    context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(batch.Handle(), surface.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle(), 1u, &region);
    const VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
    batch.SubmitAndWait();

    std::array<std::uint32_t, texelsPerLayer * layers> texels{};
    std::memcpy(texels.data(), readback.Bytes().data(), texels.size() * sizeof(std::uint32_t));
    for (std::uint32_t layer = 0u; layer < layers; ++layer) {
        for (std::uint32_t offset = 0u; offset < texelsPerLayer; ++offset) {
            const std::uint32_t expected = (layer == 3u && offset == 0u) ? 0x12345678u : 0u;
            const std::uint32_t actual = texels[layer * texelsPerLayer + offset];
            Require(actual == expected, "array storage base layer: layer " + std::to_string(layer) + " texel " + std::to_string(offset) + " was " + std::to_string(actual));
        }
    }
    std::puts("array storage base layer test passed");
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
            try {
                testArrayStorageBaseLayer(context);
            } catch (const std::exception& error) {
                std::cerr << "FAIL " << error.what() << '\n';
                ++failed;
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
