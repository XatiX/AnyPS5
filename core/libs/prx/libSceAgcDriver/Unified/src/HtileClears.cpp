#include "prx/libSceAgcDriver/Unified/include/HtileClears.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Unified/include/DepthRecords.hpp"
#include "prx/libSceAgcDriver/Unified/shaders/HtileClearVert_spv.h"
#include "prx/libSceAgcDriver/Unified/shaders/HtileScan_spv.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <string_view>

namespace AgcDriver::Unified::Images {

namespace {

constexpr std::uint32_t GroupSize = 64;
constexpr std::uint32_t MaxGroups = 65535;
constexpr std::uint32_t SetsPerPool = 64;
constexpr std::array<VkDynamicState, 3> DynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};

struct ScanPush {
    std::uint32_t base = 0;
    std::uint32_t count = 0;
    std::uint32_t stencil = 0;
    std::uint32_t reserved = 0;
};

struct ClearPush {
    float depth = 0.0f;
    std::uint32_t bit = 0;
};

struct PoolHolder {
    const Graphics::Context* context = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    ~PoolHolder() {
        if (pool != VK_NULL_HANDLE) context->Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context->device, pool, nullptr);
    }
};

}

bool HtileClearsOn() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_RECODE_HTILE_CLEARS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return on;
}

std::uint32_t ExpandedHtileWord(bool stencilInTiles) {
    return stencilInTiles ? 0xfffff3ffu : 0xfffc000fu;
}

HtileClears::HtileClears(const Graphics::Context& context) : context(context) {}

HtileClears::~HtileClears() {
    const auto destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    for (auto& [format, pass] : passes) {
        if (pass.depth != VK_NULL_HANDLE) destroyPipeline(context.device, pass.depth, nullptr);
        if (pass.stencil != VK_NULL_HANDLE) destroyPipeline(context.device, pass.stencil, nullptr);
        if (pass.renderPass != VK_NULL_HANDLE) context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, pass.renderPass, nullptr);
    }
    if (scan != VK_NULL_HANDLE) destroyPipeline(context.device, scan, nullptr);
    if (vertex != VK_NULL_HANDLE) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, vertex, nullptr);
    if (scanLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, scanLayout, nullptr);
    if (clearLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, clearLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
}

void HtileClears::createShared() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout recode HTILE clear");
    const auto createLayout = context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    const VkPushConstantRange scanRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ScanPush)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &scanRange;
    Graphics::Check(createLayout(context.device, &pipelineLayoutInfo, nullptr, &scanLayout), "vkCreatePipelineLayout recode HTILE scan");
    const VkPushConstantRange clearRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ClearPush)};
    pipelineLayoutInfo.pPushConstantRanges = &clearRange;
    Graphics::Check(createLayout(context.device, &pipelineLayoutInfo, nullptr, &clearLayout), "vkCreatePipelineLayout recode HTILE clear");
    const auto module = Graphics::CreateShaderModule(context, HTILE_SCAN_SPV, sizeof(HTILE_SCAN_SPV), "vkCreateShaderModule recode HTILE scan");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = scanLayout;
    VkResult created;
    {
        auto pipelineCacheLock = context.LockPipelineCacheForUse();
        created = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &scan);
    }
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    Graphics::Check(created, "vkCreateComputePipelines recode HTILE scan");
    vertex = Graphics::CreateShaderModule(context, HTILE_CLEAR_VERT_SPV, sizeof(HTILE_CLEAR_VERT_SPV), "vkCreateShaderModule recode HTILE clear");
}

VkPipeline HtileClears::createPipeline(VkRenderPass renderPass, bool stencil) {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertex;
    stage.pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = stencil ? VK_FALSE : VK_TRUE;
    depthStencil.depthWriteEnable = stencil ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.stencilTestEnable = stencil ? VK_TRUE : VK_FALSE;
    depthStencil.front = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 0u};
    depthStencil.back = depthStencil.front;
    depthStencil.maxDepthBounds = 1.0f;
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(DynamicStates.size());
    dynamic.pDynamicStates = DynamicStates.data();
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &stage;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = clearLayout;
    pipelineInfo.renderPass = renderPass;
    VkPipeline made = VK_NULL_HANDLE;
    Graphics::Check(context.Function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &made), "vkCreateGraphicsPipelines recode HTILE clear");
    return made;
}

HtileClears::Pass& HtileClears::passFor(VkFormat format, VkImageAspectFlags aspects) {
    auto& pass = passes[format];
    if (pass.renderPass != VK_NULL_HANDLE) return pass;
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachmentAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkSubpassDependency dependencies[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, tests | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, attachmentAccess | VK_ACCESS_SHADER_READ_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, tests, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
    };
    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;
    Graphics::Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &renderPassInfo, nullptr, &pass.renderPass), "vkCreateRenderPass recode HTILE clear");
    if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u) pass.depth = createPipeline(pass.renderPass, false);
    if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0u) pass.stencil = createPipeline(pass.renderPass, true);
    return pass;
}

bool HtileClears::Apply(Graphics::Recorder& recorder, const std::shared_ptr<DepthRecord>& record, const HtileClear& clear, std::string& reason) {
    const auto words = clear.bytes / 4u;
    if (clear.tiles == VK_NULL_HANDLE || words == 0u || (clear.offset & 3u) != 0u) {
        reason = "HTILE range not word aligned or empty";
        return false;
    }
    try {
        if (setLayout == VK_NULL_HANDLE) createShared();
        auto& pass = passFor(record->Key().format, record->Aspects());
        const auto serial = recorder.OpenBatchSerial();
        if (pool == VK_NULL_HANDLE || poolBatch != serial) {
            auto holder = std::make_shared<PoolHolder>();
            holder->context = &context;
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SetsPerPool * 2u};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = SetsPerPool;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            Graphics::Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &holder->pool), "vkCreateDescriptorPool recode HTILE clear");
            pool = holder->pool;
            poolBatch = serial;
            recorder.Keep(std::static_pointer_cast<void>(holder));
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocate, &set) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            reason = "HTILE clear descriptor pool full";
            return false;
        }
        auto result = std::make_shared<Graphics::DeviceBuffer>(context, 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
        const auto descriptorOffset = clear.offset - clear.offset % alignment;
        const auto base = clear.offset - descriptorOffset;
        const VkDescriptorBufferInfo tileInfo{clear.tiles, descriptorOffset, base + words * 4u};
        const VkDescriptorBufferInfo resultInfo{result->Handle(), 0, 16};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::uint32_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = set;
            writes[index].dstBinding = index;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pBufferInfo = &tileInfo;
        writes[1].pBufferInfo = &resultInfo;
        context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        const VkExtent2D extent{std::max(record->Key().width, 1u), std::max(record->Key().height, 1u)};
        const std::array<VkImageView, 1> views{record->View()};
        auto framebuffer = std::make_shared<Graphics::Framebuffer>(context, pass.renderPass, views, extent);
        const auto commands = recorder.Commands();
        context.Resolved(&Graphics::DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, result->Handle(), 0, 16, 0u);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        context.BindComputePipeline(commands, scan);
        context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scanLayout, 0, 1, &set, 0, nullptr);
        ScanPush scanPush;
        scanPush.base = static_cast<std::uint32_t>(base / 4u);
        scanPush.count = static_cast<std::uint32_t>(words);
        scanPush.stencil = clear.stencilInTiles ? 1u : 0u;
        context.Resolved(&Graphics::DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, scanLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(scanPush), &scanPush);
        const auto total = std::max<std::uint64_t>((words + GroupSize - 1u) / GroupSize, 1u);
        const auto x = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, MaxGroups));
        const auto y = static_cast<std::uint32_t>((total + x - 1u) / x);
        context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, x, y, 1);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        const bool clearsStencil = pass.stencil != VK_NULL_HANDLE && clear.stencilInTiles;
        recorder.RecordUncounted([&](Graphics::CommandState& recording) {
            const auto drawCommands = recording.Commands();
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = pass.renderPass;
            begin.framebuffer = framebuffer->Handle();
            begin.renderArea = {{0, 0}, extent};
            context.Resolved(&Graphics::DeviceFunctions::cmdBeginRenderPass, "vkCmdBeginRenderPass")(drawCommands, &begin, VK_SUBPASS_CONTENTS_INLINE);
            const auto slots = Graphics::CommandState::DynamicSlots(DynamicStates);
            const auto draw = [&](VkPipeline pipeline, std::uint32_t bit) {
                recording.BindPipeline(context, pipeline, slots);
                recording.SetViewport(context, VkViewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
                recording.SetScissor(context, VkRect2D{{0, 0}, extent});
                recording.SetStencilReference(context, VK_STENCIL_FACE_FRONT_AND_BACK, clear.clearStencil);
                context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(drawCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, clearLayout, 0, 1, &set, 0, nullptr);
                const ClearPush push{clear.clearDepth, bit};
                context.Resolved(&Graphics::DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(drawCommands, clearLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                context.Resolved(&Graphics::DeviceFunctions::cmdDraw, "vkCmdDraw")(drawCommands, 3, 1, 0, 0);
            };
            if (pass.depth != VK_NULL_HANDLE) draw(pass.depth, 1u);
            if (clearsStencil) draw(pass.stencil, 2u);
            context.Resolved(&Graphics::DeviceFunctions::cmdEndRenderPass, "vkCmdEndRenderPass")(drawCommands);
        });
        recorder.Keep(std::static_pointer_cast<void>(result));
        recorder.Keep(std::static_pointer_cast<void>(framebuffer));
        recorder.Keep(record);
    } catch (const std::exception& error) {
        reason = std::string("HTILE clear: ") + error.what();
        return false;
    }
    return true;
}

}
