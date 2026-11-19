#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Unified/include/DataRing.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "BdaAbi.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace AgcDriver::Unified::Record {

namespace {

using Graphics::Context;

constexpr std::uint64_t RingBytes = 64ull << 20u;
constexpr std::uint32_t PoolSets = 512;
constexpr std::uint32_t PoolBuffers = 4096;
constexpr std::uint64_t MapBudgetBytes = 256ull << 20u;

struct MapBuffer {
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t serial = 0;
};

struct Totals {
    std::uint64_t recorded = 0;
    std::uint64_t refused = 0;
    std::uint64_t existing = 0;
    std::uint64_t linearStores = 0;
    std::uint64_t indirect = 0;
    std::uint64_t snapshotArguments = 0;
    std::uint64_t pools = 0;
    std::uint64_t mapHits = 0;
    std::uint64_t mapUploads = 0;
    std::uint64_t mapUploadBytes = 0;
    std::uint64_t mapRetired = 0;
    std::uint64_t nanoseconds = 0;
    std::map<std::string, std::uint64_t> reasons;
};

std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
}

bool writesStencil(const Graphics::State& state) {
    const auto keeps = [](const VkStencilOpState& face) {
        return face.writeMask == 0u || (face.failOp == VK_STENCIL_OP_KEEP && face.passOp == VK_STENCIL_OP_KEEP && face.depthFailOp == VK_STENCIL_OP_KEEP);
    };
    return state.stencilTest && (!keeps(state.stencilFront) || !keeps(state.stencilBack));
}

std::optional<Graphics::State> maskedOutputs(const Graphics::State& state, std::uint32_t outputs) {
    std::optional<Graphics::State> masked;
    for (std::size_t index = 0; index < state.blends.size(); ++index) {
        if ((index < 32u && ((outputs >> index) & 1u) != 0u) || state.blends[index].colorWriteMask == 0u) continue;
        if (!masked.has_value()) masked = state;
        masked->blends[index].colorWriteMask = 0;
    }
    return masked;
}

std::uint64_t vertexAddress(const ShaderRecompiler::VertexAttribute& attribute) {
    const auto& fields = attribute.resource.fields;
    return fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
}

bool precheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>* reads, std::string& reason) {
    if (draw.state == nullptr || draw.stages.empty()) {
        reason = "no state or stages";
        return false;
    }
    if (!draw.complete) {
        reason = "placeholder images";
        return false;
    }
    const auto& state = *draw.state;
    if (state.stages.mesh || state.stages.tessellation || state.tessellationCapture != nullptr) {
        reason = "mesh or tessellation";
        return false;
    }
    for (const auto& stage : draw.stages) {
        if (stage.program.result->readsDepthRemapPush) {
            reason = "depth remap push";
            return false;
        }
    }
    for (const auto& color : state.colors) {
        if (color.samples > 1u || color.mipTail || color.layers != 1u) {
            reason = "color target shape";
            return false;
        }
    }
    if (state.depth && state.depth->samples > 1u) {
        reason = "multisampled depth";
        return false;
    }
    for (const auto& binding : draw.layout.bindings) {
        if (binding.count != 1u) {
            reason = "buffer array binding";
            return false;
        }
        if (binding.source == Bindings::Source::Guest || binding.source == Bindings::Source::Gds) {
            reason = "guest or GDS buffer binding";
            return false;
        }
        if (binding.source == Bindings::Source::TableMap && draw.stages.at(binding.stage).tableMap == nullptr) {
            reason = "table map without a mirror";
            return false;
        }
    }
    for (const auto& attribute : draw.stages.front().program.vertexAttributes) {
        if (Graphics::NullVertexDescriptor(attribute) || Graphics::VertexFetchOutOfRange(attribute)) {
            reason = "null or out-of-range vertex descriptor";
            return false;
        }
        if (attribute.fetchIndex > 1u) {
            reason = "vertex fetch index";
            return false;
        }
        const auto address = vertexAddress(attribute);
        if (reads != nullptr) reads->emplace_back(address, address + Graphics::VertexBufferExtent(attribute));
    }
    if (parameters.indexed && parameters.indexSize != 2u && parameters.indexSize != 4u) {
        reason = "index size";
        return false;
    }
    if (parameters.indirect) {
        const auto& args = *parameters.indirect;
        if (args.drawIndexEnabled || args.baseVertexSgpr >= 0 || args.startInstanceSgpr >= 0 || args.drawIndexSgpr >= 0) {
            reason = "indirect draw feeds user data";
            return false;
        }
        if (args.RangeBytes() == 0u) {
            reason = "empty indirect draw";
            return false;
        }
    }
    return true;
}

std::span<const std::uint32_t> wordsOf(const Resolve::ResolvedStage& stage, std::uint32_t index) {
    for (const auto& [position, words] : stage.program.words.bindings) {
        if (position == index) return words;
    }
    return stage.program.result->bindings.at(index).guestDescriptor;
}

class DrawRecorder {
public:
    bool Precheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason) {
        if (precheck(draw, parameters, &reads, reason)) return true;
        std::lock_guard lock(mutex);
        totals.existing++;
        const auto key = "existing path: " + reason.substr(0, 45);
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        return false;
    }

    bool Record(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        std::lock_guard lock(mutex);
        const auto started = std::chrono::steady_clock::now();
        bool recorded = false;
        try {
            recorded = record(context, draw, parameters, reason);
        } catch (const std::exception& error) {
            reason = std::string("record failed: ") + error.what();
        }
        totals.nanoseconds += elapsed(started);
        if (recorded) {
            totals.recorded++;
            return true;
        }
        refuse(reason);
        return false;
    }

    std::string Line() {
        std::lock_guard lock(mutex);
        char text[384];
        const auto draws = totals.recorded + totals.refused;
        const auto ring = space.has_value() ? space->TakeCounts() : RingCounts{};
        std::snprintf(text, sizeof(text), "[recode-record] %llu recorded, %llu dropped, %llu to the existing path, %.1f us per draw; %llu indirect (%llu argument snapshots); ring %llu allocations, %.1f MiB, %llu wraps, %llu full; %llu descriptor pools", static_cast<unsigned long long>(totals.recorded), static_cast<unsigned long long>(totals.refused), static_cast<unsigned long long>(totals.existing), draws == 0 ? 0.0 : static_cast<double>(totals.nanoseconds) / 1000.0 / static_cast<double>(draws), static_cast<unsigned long long>(totals.indirect), static_cast<unsigned long long>(totals.snapshotArguments), static_cast<unsigned long long>(ring.allocations), static_cast<double>(ring.bytes) / 1048576.0, static_cast<unsigned long long>(ring.wraps), static_cast<unsigned long long>(ring.full), static_cast<unsigned long long>(totals.pools));
        std::string line = text;
        line += "; linear target stores " + std::to_string(totals.linearStores);
        std::snprintf(text, sizeof(text), "; table maps %llu hits, %llu uploads (%.1f MiB), %llu retired, %zu held (%.1f MiB)", static_cast<unsigned long long>(totals.mapHits), static_cast<unsigned long long>(totals.mapUploads), static_cast<double>(totals.mapUploadBytes) / 1048576.0, static_cast<unsigned long long>(totals.mapRetired), maps.size(), static_cast<double>(mapBytes) / 1048576.0);
        line += text;
        if (fault != nullptr) {
            const auto bytes = fault->Bytes();
            line += std::ranges::any_of(bytes, [](std::byte value) { return value != std::byte{0}; }) ? "; fault buffer written" : "; fault buffer clean";
        }
        for (const auto& [why, count] : totals.reasons) line += "; " + why + " " + std::to_string(count);
        const auto pools = totals.pools;
        totals = {};
        totals.pools = pools;
        return line;
    }

private:
    bool attach(const Context& context, std::string& reason) {
        if (device == context.device && ring != nullptr) return true;
        if (device != VK_NULL_HANDLE) {
            reason = "a second device";
            return false;
        }
        if (ResidencyService::PageTableEnabled() && !ResidencyService::Get().Attach(context)) {
            reason = "residency service did not attach";
            return false;
        }
        device = context.device;
        alignment = std::max<std::uint64_t>(context.limits.minStorageBufferOffsetAlignment, 16u);
        ring = std::make_shared<Graphics::Buffer>(context, RingBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        fault = std::make_shared<Graphics::Buffer>(context, ShaderRecompiler::BdaAbi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(fault->Bytes().data(), 0, fault->Bytes().size());
        space.emplace(RingBytes, alignment);
        std::fprintf(stderr, "[recode-record] ring %llu MiB, alignment %llu\n", static_cast<unsigned long long>(RingBytes >> 20u), static_cast<unsigned long long>(alignment));
        return true;
    }

    void refuse(const std::string& reason) {
        totals.refused++;
        auto key = "dropped: " + reason.substr(0, 50);
        if (seen.insert(key).second) std::fprintf(stderr, "[recode-record] first drop: %s\n", reason.c_str());
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
    }

    std::shared_ptr<Graphics::Buffer> mapBuffer(const Context& context, const Tables::Mirror& mirror, std::uint64_t serial, std::uint64_t completed) {
        if (const auto found = maps.find(mirror.id); found != maps.end()) {
            found->second.serial = serial;
            totals.mapHits++;
            return found->second.buffer;
        }
        if (mapBytes > MapBudgetBytes) {
            for (auto at = maps.begin(); at != maps.end() && mapBytes > MapBudgetBytes / 2u;) {
                if (at->second.serial > completed) {
                    ++at;
                    continue;
                }
                mapBytes -= at->second.buffer->Bytes().size();
                at = maps.erase(at);
                totals.mapRetired++;
            }
        }
        const auto bytes = std::max<std::size_t>(mirror.map.size() * sizeof(std::uint32_t), 16u);
        auto buffer = std::make_shared<Graphics::Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(buffer->Bytes().data(), mirror.map.data(), mirror.map.size() * sizeof(std::uint32_t));
        mapBytes += buffer->Bytes().size();
        maps[mirror.id] = {buffer, serial};
        totals.mapUploads++;
        totals.mapUploadBytes += bytes;
        return buffer;
    }

    std::optional<std::uint64_t> ringWrite(std::span<const std::byte> bytes, std::uint64_t serial, std::uint64_t completed) {
        const auto offset = space->Allocate(bytes.size(), serial, completed);
        if (!offset.has_value()) return std::nullopt;
        std::memcpy(ring->Bytes().data() + *offset, bytes.data(), bytes.size());
        return offset;
    }

    VkDescriptorSetLayout setLayout(const Context& context, const Resolve::ResolvedDraw& draw, std::uint32_t set) {
        const auto& key = draw.layout.keys[set];
        if (const auto found = layouts.find(key); found != layouts.end()) return found->second;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const auto& binding : draw.layout.bindings) {
            if (binding.set != set) continue;
            VkShaderStageFlags stages = 0;
            for (const auto& stage : draw.stages) {
                if ((binding.stages & Bindings::StageBit(stage.program.stage)) != 0u) stages |= Graphics::VulkanStage(stage.program.stage);
            }
            bindings.push_back({binding.binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding.count, stages, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout recode draw");
        layouts.emplace(key, layout);
        return layout;
    }

    VkDescriptorPool createPool(const Context& context) {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, PoolBuffers};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = PoolSets;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        Graphics::Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &info, nullptr, &pool), "vkCreateDescriptorPool recode draw");
        totals.pools++;
        return pool;
    }

    VkDescriptorPool poolFor(const Context& context, std::uint64_t serial, std::uint64_t completed, bool fresh) {
        while (!pools.empty() && pools.front().second <= completed) {
            context.Function<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")(context.device, pools.front().first, 0);
            spare.push_back(pools.front().first);
            pools.pop_front();
        }
        if (!fresh && !pools.empty() && pools.back().second == serial) return pools.back().first;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!spare.empty()) {
            pool = spare.back();
            spare.pop_back();
        } else {
            pool = createPool(context);
        }
        pools.emplace_back(pool, serial);
        return pool;
    }

    VkDescriptorSet allocateSet(const Context& context, VkDescriptorSetLayout layout, std::uint64_t serial, std::uint64_t completed) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        info.descriptorPool = poolFor(context, serial, completed, false);
        if (context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &info, &set) == VK_SUCCESS) return set;
        info.descriptorPool = poolFor(context, serial, completed, true);
        Graphics::Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &info, &set), "vkAllocateDescriptorSets recode draw");
        return set;
    }

    std::uint32_t fragmentOutputs(std::span<const Graphics::CompiledShader> shaders) {
        std::uint32_t mask = 0;
        for (const auto& shader : shaders) {
            if (shader.stage != ShaderRecompiler::ShaderStage::Fragment) continue;
            const auto hash = shader.program->spirv.ContentHash();
            const std::pair key{hash.low, hash.high};
            auto found = outputs.find(key);
            if (found == outputs.end()) found = outputs.emplace(key, Bindings::OutputLocations(shader.program->spirv.Words())).first;
            mask |= found->second;
        }
        return mask;
    }

    bool record(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        auto* recorder = context.recorder;
        const auto* heap = Resolve::HeapDevice();
        if (recorder == nullptr || heap == nullptr || draw.state == nullptr) {
            reason = "no recorder or heap";
            return false;
        }
        if (!attach(context, reason) || !precheck(draw, parameters, nullptr, reason)) return false;
        const auto& state = *draw.state;
        std::vector<Graphics::CompiledShader> shaders;
        for (const auto& stage : draw.stages) shaders.push_back({stage.program.stage, stage.program.result.get(), stage.program.pushOffset});
        const auto& attributes = draw.stages.front().program.vertexAttributes;
        Graphics::VertexInputLayout vertexInput;
        std::vector<VkBuffer> vertexBuffers;
        std::vector<VkDeviceSize> vertexOffsets;
        if (!attributes.empty()) {
            vertexInput = Graphics::BuildVertexInputLayout(context, attributes);
            for (std::size_t index = 0; index < attributes.size(); ++index) {
                const auto& attribute = attributes[index];
                if (vertexInput.attributes[index].format != Graphics::DecodeVertexFormat(attribute).format) {
                    reason = "widened vertex format";
                    return false;
                }
                const auto address = vertexAddress(attribute);
                const auto bytes = Graphics::VertexBufferExtent(attribute);
                const auto* import = Graphics::HostImportFor(context, address, bytes);
                if (import == nullptr) {
                    reason = "vertex buffer not imported";
                    return false;
                }
                recorder->NotePendingRead(address, bytes, Graphics::Recorder::ReadKind::DrawInput);
                vertexBuffers.push_back(import->buffer);
                vertexOffsets.push_back(Graphics::HostImportOffset(*import, address));
            }
        }
        std::vector<VkImageView> views;
        std::vector<std::shared_ptr<void>> owners;
        std::vector<std::shared_ptr<Graphics::StorageTexture>> residents;
        for (const auto& color : state.colors) {
            if (Graphics::AttachmentProxyFormat(context, color.format) != VK_FORMAT_UNDEFINED) {
                reason = "proxied color format";
                return false;
            }
            auto resident = Graphics::CachedStorageSurface(context, Graphics::SurfaceForTarget(color));
            if (resident == nullptr || !resident->Attachable()) {
                reason = "color target not resident";
                return false;
            }
            views.push_back(resident->AttachmentView(color.format, color.mip, color.depthSlice, color.layers, color.arraySlice));
            owners.push_back(resident);
            residents.push_back(std::move(resident));
        }
        if (state.depth) {
            views.push_back(Graphics::DepthSurfaceViewWithStencilWriteIntent(context, *state.depth, state.depthWrite, state.stencilTest, writesStencil(state)));
        }
        Graphics::PipelineSetLayouts sets;
        sets.main = setLayout(context, draw, 0);
        sets.split = draw.layout.split;
        if (sets.split) sets.fragment = setLayout(context, draw, 1);
        sets.mainKey = draw.layout.keys[0];
        sets.fragmentKey = draw.layout.keys[1];
        sets.heap = heap->Layout();
        auto pipeline = Graphics::CachedPipeline(context, state, vertexInput, sets, shaders, VK_IMAGE_LAYOUT_GENERAL);
        if (pipeline == nullptr) {
            reason = "no pipeline";
            return false;
        }
        auto framebuffer = pipeline->AcquireFramebuffer(views, owners, state.renderExtent, state.renderLayers);
        const auto masked = maskedOutputs(state, fragmentOutputs(shaders));

        const auto serial = recorder->OpenBatchSerial();
        const auto completed = recorder->CompletedThrough();
        std::array<VkDescriptorSet, 2> descriptorSets{};
        std::shared_ptr<Graphics::Buffer> pageTable;
        for (std::uint32_t set = 0; set < (sets.split ? 2u : 1u); ++set) {
            descriptorSets[set] = allocateSet(context, set == 0u ? sets.main : sets.fragment, serial, completed);
            std::vector<VkDescriptorBufferInfo> infos;
            std::vector<VkWriteDescriptorSet> writes;
            infos.reserve(draw.layout.bindings.size());
            for (const auto& binding : draw.layout.bindings) {
                if (binding.set != set) continue;
                VkDescriptorBufferInfo info{};
                switch (binding.source) {
                    case Bindings::Source::Words: {
                        const auto words = wordsOf(draw.stages.at(binding.stage), binding.index);
                        std::vector<std::byte> bytes(std::max<std::size_t>(words.size_bytes(), 16u));
                        std::memcpy(bytes.data(), words.data(), words.size_bytes());
                        const auto offset = ringWrite(bytes, serial, completed);
                        if (!offset.has_value()) {
                            reason = "ring full";
                            return false;
                        }
                        info = {ring->Handle(), *offset, bytes.size()};
                        break;
                    }
                    case Bindings::Source::PageTable:
                        if (pageTable == nullptr) pageTable = ResidencyService::Get().PageTableBuffer();
                        if (pageTable == nullptr) {
                            reason = "no GPU page table";
                            return false;
                        }
                        info = {pageTable->Handle(), 0, VK_WHOLE_SIZE};
                        break;
                    case Bindings::Source::Fault: info = {fault->Handle(), 0, VK_WHOLE_SIZE}; break;
                    case Bindings::Source::TableMap: {
                        const auto& mirror = draw.stages.at(binding.stage).tableMap;
                        if (mirror == nullptr) {
                            reason = "table map without a mirror";
                            return false;
                        }
                        info = {mapBuffer(context, *mirror, serial, completed)->Handle(), 0, VK_WHOLE_SIZE};
                        break;
                    }
                    default: reason = "guest or GDS buffer binding"; return false;
                }
                infos.push_back(info);
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptorSets[set];
                write.dstBinding = binding.binding;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &infos.back();
                writes.push_back(write);
            }
            if (!writes.empty()) context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
        }

        std::array<std::byte, Graphics::PipelinePushConstantBytes> block{};
        for (std::size_t index = 0; index < draw.stages.size(); ++index) {
            const auto& push = draw.stages[index].program.words.pushConstants;
            if (push.empty()) continue;
            if (shaders[index].pushConstantOffset + push.size() > block.size()) {
                reason = "push data outside the block";
                return false;
            }
            std::memcpy(block.data() + shaders[index].pushConstantOffset, push.data(), push.size());
        }

        VkBuffer argumentBuffer = VK_NULL_HANDLE;
        VkDeviceSize argumentOffset = 0;
        const Graphics::HostImport* countImport = nullptr;
        if (parameters.indirect) {
            using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
            const auto& args = *parameters.indirect;
            const auto bytes = static_cast<std::size_t>(args.RangeBytes());
            const bool rewritten = args.vertexRule == Rule::Constant || args.instanceRule == Rule::Constant;
            const auto* source = reinterpret_cast<const std::byte*>(args.arguments);
            if (!args.countIndirect && !Graphics::Recorder::SnapshotWriteOverlaps(args.arguments, bytes) && recorder->PendingLeaseSerial(args.arguments, bytes) == 0u && GuestMemory::Accessible(source, bytes, false)) {
                std::vector<std::byte> copy(source, source + bytes);
                for (std::uint32_t record = 0; record < args.count; ++record) {
                    const auto base = static_cast<std::size_t>(record) * args.stride;
                    if (args.vertexRule == Rule::Constant) std::memcpy(copy.data() + base + args.VertexDwordOffset(), &args.vertexConstant, 4);
                    if (args.instanceRule == Rule::Constant) std::memcpy(copy.data() + base + args.InstanceDwordOffset(), &args.instanceConstant, 4);
                }
                const auto offset = ringWrite(copy, serial, completed);
                if (!offset.has_value()) {
                    reason = "ring full";
                    return false;
                }
                argumentBuffer = ring->Handle();
                argumentOffset = *offset;
                totals.snapshotArguments++;
            } else {
                if (rewritten) {
                    reason = "constant rule on GPU-written arguments";
                    return false;
                }
                const auto* import = Graphics::HostImportFor(context, args.arguments, bytes);
                if (import == nullptr) {
                    reason = "indirect arguments not imported";
                    return false;
                }
                if (args.countIndirect) {
                    if (!context.drawIndirectCount) {
                        reason = "no indirect count";
                        return false;
                    }
                    countImport = Graphics::HostImportFor(context, args.countAddress, sizeof(std::uint32_t));
                    if (countImport == nullptr) {
                        reason = "indirect count not imported";
                        return false;
                    }
                    recorder->NotePendingRead(args.countAddress, sizeof(std::uint32_t), Graphics::Recorder::ReadKind::Indirect);
                }
                recorder->NotePendingRead(args.arguments, bytes, Graphics::Recorder::ReadKind::Indirect);
                argumentBuffer = import->buffer;
                argumentOffset = Graphics::HostImportOffset(*import, args.arguments);
            }
            totals.indirect++;
        }
        const Graphics::HostImport* indexImport = nullptr;
        if (parameters.indexed) {
            const auto bytes = static_cast<std::size_t>(parameters.indexCount) * parameters.indexSize;
            indexImport = bytes == 0u ? nullptr : Graphics::HostImportFor(context, parameters.indexAddress, bytes);
            if (indexImport == nullptr) {
                reason = "indices not imported";
                return false;
            }
            recorder->NotePendingRead(parameters.indexAddress, bytes, Graphics::Recorder::ReadKind::DrawInput);
        }

        if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
        if (recorder->HasQueuedStores()) recorder->FlushStores();
        const auto key = Graphics::DrawRenderPassKey(context, state, views);
        Graphics::PassAccess access;
        access.anyReads = true;
        access.anyWrites = true;
        const bool fenced = state.depth.has_value();
        const auto start = recorder->StartDrawPass(key, true, fenced, access);
        const auto commands = start.commands;
        auto& recording = recorder->GraphicsState();
        recorder->PrepareSampleSlot();
        if (start.barrier) {
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
        }
        if (parameters.indirect) Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        const auto* maskedState = masked.has_value() ? &*masked : nullptr;
        pipeline->Begin(recording, *framebuffer, state.renderExtent, state, maskedState);

        const auto bind = context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DescriptorSet, 1, &descriptorSets[0], 0, nullptr);
        if (sets.split) bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &descriptorSets[1], 0, nullptr);
        const VkDescriptorSet heapSet = heap->Set();
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DirectHeapSet, 1, &heapSet, 0, nullptr);
        pipeline->PushConstants(commands, Graphics::PushConstantStages(shaders), block);

        if (!vertexBuffers.empty()) context.Resolved(&Graphics::DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(vertexBuffers.size()), vertexBuffers.data(), vertexOffsets.data());
        if (indexImport != nullptr) context.Resolved(&Graphics::DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, indexImport->buffer, Graphics::HostImportOffset(*indexImport, parameters.indexAddress), parameters.indexSize == 2u ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        if (parameters.indirect) {
            const auto& args = *parameters.indirect;
            if (countImport != nullptr) {
                if (parameters.indexed) context.Function<PFN_vkCmdDrawIndexedIndirectCountKHR>("vkCmdDrawIndexedIndirectCountKHR")(commands, argumentBuffer, argumentOffset, countImport->buffer, Graphics::HostImportOffset(*countImport, args.countAddress), args.count, args.stride);
                else context.Function<PFN_vkCmdDrawIndirectCountKHR>("vkCmdDrawIndirectCountKHR")(commands, argumentBuffer, argumentOffset, countImport->buffer, Graphics::HostImportOffset(*countImport, args.countAddress), args.count, args.stride);
            } else if (args.count <= 1u || context.multiDrawIndirect) {
                if (parameters.indexed) context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, argumentOffset, args.count, args.stride);
                else context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, argumentOffset, args.count, args.stride);
            } else {
                for (std::uint32_t record = 0; record < args.count; ++record) {
                    const auto offset = argumentOffset + static_cast<VkDeviceSize>(record) * args.stride;
                    if (parameters.indexed) context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect")(commands, argumentBuffer, offset, 1, args.stride);
                    else context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndirect, "vkCmdDrawIndirect")(commands, argumentBuffer, offset, 1, args.stride);
                }
            }
        } else if (parameters.indexed) {
            context.Resolved(&Graphics::DeviceFunctions::cmdDrawIndexed, "vkCmdDrawIndexed")(commands, parameters.indexCount, parameters.instanceCount, 0, static_cast<std::int32_t>(parameters.firstVertex), parameters.firstInstance);
        } else {
            context.Resolved(&Graphics::DeviceFunctions::cmdDraw, "vkCmdDraw")(commands, parameters.indexCount, parameters.instanceCount, parameters.firstVertex, parameters.firstInstance);
        }

        recorder->LeaveRenderPassOpen(key, 0, fenced, access);
        recorder->Keep(pipeline);
        recorder->Keep(framebuffer);
        if (pageTable != nullptr) recorder->Keep(pageTable);
        std::vector<bool> storeNow(residents.size(), false);
        for (std::size_t index = 0; index < residents.size(); ++index) {
            auto& resident = residents[index];
            if (!resident->Cached()) recorder->KeepBytes(resident.get(), static_cast<std::size_t>(resident->BudgetBytes()));
            if (resident->Descriptor().tileMode == Graphics::TextureTileMode::kLinear) storeNow[index] = !resident->MarkDrawn(state.colors[index].arraySlice, 1u, state.scissor) || !resident->Cached();
            else resident->MarkSliceDirty(state.colors[index].arraySlice, 1u);
            recorder->Keep(resident);
        }
        for (std::size_t index = 0; index < residents.size(); ++index) {
            if (storeNow[index]) Graphics::StorageTexture::FlushPending(residents[index]->Descriptor().baseAddress, static_cast<std::size_t>(residents[index]->GuestBytes()), nullptr, "linear target");
        }
        totals.linearStores += static_cast<std::uint64_t>(std::ranges::count(storeNow, true));
        return true;
    }

    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t alignment = 16;
    std::shared_ptr<Graphics::Buffer> ring;
    std::shared_ptr<Graphics::Buffer> fault;
    std::optional<RingSpace> space;
    std::map<std::vector<std::uint32_t>, VkDescriptorSetLayout> layouts;
    std::deque<std::pair<VkDescriptorPool, std::uint64_t>> pools;
    std::vector<VkDescriptorPool> spare;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> outputs;
    std::map<std::uint64_t, MapBuffer> maps;
    std::uint64_t mapBytes = 0;
    std::set<std::string> seen;
    Totals totals;
};

DrawRecorder& shared() {
    static auto* recorder = new DrawRecorder();
    return *recorder;
}

}

bool RecordActive() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_RECODE_DRAW");
        return value != nullptr && std::string_view(value) == "record";
    }();
    return enabled;
}

bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason) {
    return shared().Precheck(draw, parameters, reads, reason);
}

bool RecordResolved(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
    return shared().Record(context, draw, parameters, reason);
}

std::string RecordLine() {
    return shared().Line();
}

}
