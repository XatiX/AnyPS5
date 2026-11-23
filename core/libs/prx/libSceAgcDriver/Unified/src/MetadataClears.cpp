#include "prx/libSceAgcDriver/Unified/include/MetadataClears.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanImages.hpp"
#include "prx/libSceAgcDriver/Unified/shaders/DccClear_spv.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>

namespace AgcDriver::Unified::Images {

namespace {

constexpr std::uint32_t GroupSize = 64;
constexpr std::uint32_t MaxGroups = 65535;
constexpr std::uint32_t SetsPerPool = 64;

struct Push {
    std::uint32_t base = 0;
    std::uint32_t count = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mode = 0;
    std::uint32_t reserved[3]{};
    std::uint32_t patterns[5][4]{};
};

struct PoolHolder {
    const Graphics::Context* context = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::uint32_t used = 0;

    ~PoolHolder() {
        if (pool != VK_NULL_HANDLE) context->Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context->device, pool, nullptr);
    }
};

void groups(std::uint64_t invocations, std::uint32_t& x, std::uint32_t& y) {
    const auto total = std::max<std::uint64_t>((invocations + GroupSize - 1u) / GroupSize, 1u);
    x = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, MaxGroups));
    y = static_cast<std::uint32_t>((total + x - 1u) / x);
}

bool pack(std::span<const std::byte> bytes, std::uint32_t (&pattern)[4]) {
    if (bytes.size() > 16u) return false;
    std::memset(pattern, 0, sizeof(pattern));
    std::memcpy(pattern, bytes.data(), bytes.size());
    return true;
}

}

bool MetadataClearsOn() {
    static const bool on = [] {
        const char* value = std::getenv("APS5_RECODE_METADATA_CLEARS");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return on;
}

MetadataClears::MetadataClears(const Graphics::Context& context) : context(context), detiler(std::make_unique<Graphics::TextureDetiler>(context)) {}

MetadataClears::~MetadataClears() {
    if (pipeline != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
}

void MetadataClears::NoteTarget(const Graphics::ColorTarget& color) {
    if (color.dccAddress == 0u && (color.cmaskAddress == 0u || !color.cmaskFastClear)) return;
    std::lock_guard lock(mutex);
    auto& target = targets[color.address];
    target.bytes = color.bytes;
    target.dcc = color.dccAddress;
    target.dccKeys = color.dccAddress != 0u ? Graphics::DccKeyBytes(color.bytes) : 0u;
    target.cmask = color.cmaskFastClear ? color.cmaskAddress : 0u;
    target.cmaskBytes = color.cmaskFastClear ? color.cmaskBytes : 0u;
    target.format = color.format;
    target.elementBytes = color.elementBytes;
    target.alphaOnMsb = color.dccAlphaOnMsb;
    target.clearWords = color.clearWords;
}

void MetadataClears::NoteWrite(std::uint64_t address, std::uint64_t size) {
    if (size == 0u) return;
    std::lock_guard lock(mutex);
    for (auto& [at, target] : targets) {
        if (target.dcc != 0u && address < target.dcc + target.dccKeys && target.dcc < address + size) {
            if (!target.dccPending) counts.pendingDcc++;
            target.dccPending = true;
        }
        if (target.cmask != 0u && address < target.cmask + target.cmaskBytes && target.cmask < address + size) {
            if (!target.cmaskPending) counts.pendingCmask++;
            target.cmaskPending = true;
        }
    }
}

bool MetadataClears::Pending(std::uint64_t target, std::uint64_t bytes) const {
    std::lock_guard lock(mutex);
    const auto found = targets.find(target);
    return found != targets.end() && found->second.bytes == bytes && (found->second.dccPending || found->second.cmaskPending);
}

bool MetadataClears::Apply(Graphics::Recorder& recorder, std::uint64_t address, VulkanSurface& surface, std::string& reason) {
    Target target;
    {
        std::lock_guard lock(mutex);
        const auto found = targets.find(address);
        if (found == targets.end() || (!found->second.dccPending && !found->second.cmaskPending)) return false;
        target = found->second;
        found->second.dccPending = false;
        found->second.cmaskPending = false;
    }
    if (surface.Compressed() || std::max(surface.Geometry().imageLayers, 1u) != 1u || surface.Geometry().imageDepth > 1u) {
        reason = "record is compressed, layered or a volume";
        std::lock_guard lock(mutex);
        counts.refused++;
        return false;
    }
    bool applied = false;
    try {
        if (target.cmaskPending) applied = applyCmask(recorder, target, surface, reason) || applied;
        if (target.dccPending) applied = applyDcc(recorder, target, surface, reason) || applied;
    } catch (const std::exception& error) {
        reason = std::string("metadata clear: ") + error.what();
        applied = false;
    }
    if (!reason.empty()) {
        std::lock_guard lock(mutex);
        counts.refused++;
    }
    return applied;
}

bool MetadataClears::applyCmask(Graphics::Recorder& recorder, const Target& target, VulkanSurface& surface, std::string& reason) {
    Shadow::OwnedSpan span;
    if (!Shadow::ServeOwned(context, target.cmask, target.cmaskBytes, &span) || span.handle == 0u || target.cmask < span.origin) {
        reason = "CMASK memory not resident";
        return false;
    }
    const auto serial = recorder.OpenBatchSerial();
    if (serial != batch) {
        detiler->BeginBatch();
        batch = serial;
    }
    const auto commands = recorder.Commands();
    surface.EnsureGeneral(commands);
    const auto& resource = surface.Resource();
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    detiler->DispatchCmaskClear(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle)), target.cmask - span.origin, target.cmaskBytes, surface.StorageView(0u, 0u, elementBytes), resource.width, resource.height, elementBytes, target.clearWords, true);
    std::lock_guard lock(mutex);
    counts.cmaskApplied++;
    return true;
}

void MetadataClears::createPipeline() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = index == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout recode DCC clear");
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    Graphics::Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout recode DCC clear");
    const auto module = Graphics::CreateShaderModule(context, DCC_CLEAR_SPV, sizeof(DCC_CLEAR_SPV), "vkCreateShaderModule recode DCC clear");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    VkResult created;
    {
        auto pipelineCacheLock = context.LockPipelineCacheForUse();
        created = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
    }
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    Graphics::Check(created, "vkCreateComputePipelines recode DCC clear");
}

bool MetadataClears::applyDcc(Graphics::Recorder& recorder, const Target& target, VulkanSurface& surface, std::string& reason) {
    const auto& resource = surface.Resource();
    const auto elementBytes = Graphics::BytesPerElement(resource.format);
    if (elementBytes == 0u || elementBytes > 16u || target.dccKeys == 0u) {
        reason = "DCC clear of this element size";
        return false;
    }
    Push push;
    const Graphics::DccKeys codes[4] = {Graphics::DccKeys::Clear0000, Graphics::DccKeys::Clear0001, Graphics::DccKeys::Clear1110, Graphics::DccKeys::Clear1111};
    std::array<std::byte, 16> texel{};
    for (std::size_t slot = 0; slot < 4u; ++slot) {
        if (!Graphics::FillDccClear(target.format, codes[slot], target.alphaOnMsb, std::span(texel.data(), elementBytes)) || !pack(std::span(texel.data(), elementBytes), push.patterns[slot])) {
            reason = "DCC clear of this format";
            return false;
        }
    }
    std::memcpy(texel.data(), target.clearWords.data(), std::min<std::size_t>(sizeof(target.clearWords), elementBytes));
    static_cast<void>(pack(std::span(texel.data(), std::min<std::size_t>(elementBytes, sizeof(target.clearWords))), push.patterns[4]));
    Shadow::OwnedSpan span;
    if (!Shadow::ServeOwned(context, target.dcc, target.dccKeys, &span) || span.handle == 0u || target.dcc < span.origin) {
        reason = "DCC memory not resident";
        return false;
    }
    if (pipeline == VK_NULL_HANDLE) createPipeline();
    const auto commands = recorder.Commands();
    surface.EnsureGeneral(commands);
    const auto serial = recorder.OpenBatchSerial();
    if (pool == VK_NULL_HANDLE || poolBatch != serial) {
        auto holder = std::make_shared<PoolHolder>();
        holder->context = &context;
        const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SetsPerPool * 2u}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SetsPerPool}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = SetsPerPool;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        Graphics::Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &holder->pool), "vkCreateDescriptorPool recode DCC clear");
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
        reason = "DCC clear descriptor pool full";
        return false;
    }
    auto result = std::make_shared<Graphics::Buffer>(context, 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto buffer = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle));
    const auto offset = target.dcc - span.origin;
    const auto alignment = std::max<VkDeviceSize>(context.limits.minStorageBufferOffsetAlignment, 4);
    const auto descriptorOffset = offset - offset % alignment;
    const auto base = offset - descriptorOffset;
    const VkDescriptorBufferInfo keyInfo{buffer, descriptorOffset, (base + target.dccKeys + 3u) & ~std::uint64_t{3u}};
    const VkDescriptorBufferInfo resultInfo{result->Handle(), 0, 16};
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, surface.StorageView(0u, 0u, elementBytes), VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (std::uint32_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = index == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo = &keyInfo;
    writes[1].pBufferInfo = &resultInfo;
    writes[2].pImageInfo = &imageInfo;
    context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    context.Resolved(&Graphics::DeviceFunctions::cmdFillBuffer, "vkCmdFillBuffer")(commands, result->Handle(), 0, 16, 0u);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    context.BindComputePipeline(commands, pipeline);
    context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    push.base = static_cast<std::uint32_t>(base);
    push.count = static_cast<std::uint32_t>(target.dccKeys);
    push.width = resource.width;
    push.height = resource.height;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    push.mode = 0u;
    context.Resolved(&Graphics::DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    groups(target.dccKeys, x, y);
    context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, x, y, 1);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    push.mode = 1u;
    context.Resolved(&Graphics::DeviceFunctions::cmdPushConstants, "vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    groups(static_cast<std::uint64_t>(resource.width) * resource.height, x, y);
    context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, x, y, 1);
    Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
    recorder.Keep(std::static_pointer_cast<void>(result));
    recorder.OnComplete([result, mixed = mixedKeys] {
        std::uint32_t state = 0;
        std::memcpy(&state, result->Bytes().data(), sizeof(state));
        if (state != 0u) mixed->fetch_add(1, std::memory_order_relaxed);
    });
    std::lock_guard lock(mutex);
    counts.dccApplied++;
    return true;
}

MetadataCounts MetadataClears::TakeCounts() {
    std::lock_guard lock(mutex);
    auto taken = counts;
    taken.dccMixed = mixedKeys->exchange(0, std::memory_order_relaxed);
    counts = {};
    return taken;
}

}
