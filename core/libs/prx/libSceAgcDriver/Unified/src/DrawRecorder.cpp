#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libkernel/include/Api.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineSpecialization.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Unified/include/DataRing.hpp"
#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/ImageDetile.hpp"
#include "prx/libSceAgcDriver/Unified/include/MemoryCore.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidencyService.hpp"
#include "prx/libSceAgcDriver/Unified/include/SlotPatch.hpp"
#include "prx/libSceAgcDriver/Unified/include/Shadow.hpp"
#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "prx/libSceAgcDriver/Unified/include/Targets.hpp"
#include "prx/libSceAgcDriver/Unified/include/VulkanHeap.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "BdaAbi.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>
#include <thread>

namespace AgcDriver::Graphics {
void WriteCheckpointMarker(const Context& context, VkCommandBuffer commands, std::uint64_t token, std::uint32_t queue);
void WriteNvCheckpointToken(const Context& context, VkCommandBuffer commands, std::uint64_t token, std::uint32_t queue);
}

namespace AgcDriver::Unified::Record {

namespace {

using Graphics::Context;

constexpr std::uint64_t UploadTimingKey = 0x7ec0de02;
constexpr std::uint64_t DrawTimingKey = Graphics::Recorder::DrawTimingFlag;
constexpr std::uint64_t DispatchTimingKey = 0x7ec0de04;

bool writesFaultTokens() {
    static const bool writes = std::getenv("APS5_GPU_CHECKPOINTS") != nullptr;
    return writes;
}

void noteFaultToken(const Context& context, Graphics::Recorder& recorder, VkCommandBuffer commands, std::span<const Resolve::ResolvedStage> stages, FaultStage stage, const char* kind, std::uint32_t count, std::uint32_t instances) {
    try {
        if (!writesFaultTokens()) return;
        const auto queue = GuestMemory::GpuLockThreadTag();
        if (queue != 0) return;
        auto& ring = FaultTokenRing::Instance();
        const auto token = ring.Claim();
        auto* entry = ring.Mutable(token);
        if (entry == nullptr) return;
        std::uint64_t hashes[2] = {};
        std::uint64_t address = 0;
        std::uint64_t variant = 0;
        for (const auto& one : stages) {
            if (one.program.result == nullptr) continue;
            const auto hash = one.program.result->spirv.ContentHash().low;
            const bool fragment = one.program.stage == ShaderRecompiler::ShaderStage::Fragment;
            hashes[fragment ? 1 : 0] = hash;
            if (variant == 0 || fragment) {
                variant = one.program.result->variantId;
                address = one.program.codeAddress;
            }
        }
        entry->submitSerial = recorder.OpenSerial();
        entry->programAddress = address;
        entry->variantId = variant;
        entry->stage = static_cast<std::uint32_t>(stage);
        entry->argBuffer = 0;
        entry->argOffset = 0;
        entry->argStride = 0;
        entry->argCount = 0;
        std::snprintf(entry->desc, sizeof(entry->desc), "recode %s %08x/%08x c=%u i=%u", kind, static_cast<std::uint32_t>(hashes[0]), static_cast<std::uint32_t>(hashes[1]), count, instances);
        recorder.NoteFaultToken(token);
        std::atomic_ref<FaultToken>(entry->token).store(token, std::memory_order_release);
        Graphics::WriteCheckpointMarker(context, commands, token, queue);
        Graphics::WriteNvCheckpointToken(context, commands, token, queue);
    } catch (...) {
    }
}

constexpr std::uint64_t RingBytes = 64ull << 20u;
constexpr std::uint64_t RingZeroBytes = 256;
constexpr std::uint64_t KnownReadableBytes = 32;
constexpr std::uint32_t PoolSets = 512;
constexpr std::uint32_t PoolBuffers = 4096;
constexpr std::uint64_t MapBudgetBytes = 256ull << 20u;

struct MapBuffer {
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t serial = 0;
};

struct FaultBuffer {
    std::shared_ptr<Graphics::Buffer> buffer;
    std::uint64_t serial = 0;
};

struct KnownWrite {
    std::uint64_t serial = 0;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

struct CopyBack {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    const Graphics::HostImport* import = nullptr;
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
    std::uint64_t surfaceFailures = 0;
    std::uint64_t faultReads = 0;
    std::uint64_t batchMoves = 0;
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::uint64_t passFeedback = 0;
    std::uint64_t deferredReads = 0;
    std::uint64_t targetMemoHits = 0;
    std::uint64_t targetMemoMisses = 0;
    std::uint64_t residencyLands = 0;
    std::uint64_t faults = 0;
    std::uint64_t unmappedFaults = 0;
    std::uint64_t faultPages = 0;
    std::uint64_t writtenRanges = 0;
    std::uint64_t writtenLate = 0;
    std::uint64_t writtenOverflows = 0;
    std::uint64_t writtenOnImports = 0;
    std::uint64_t importSwitches = 0;
    std::uint64_t writtenSwitches = 0;
    std::uint64_t storageWrites = 0;
    std::uint64_t switchBytes = 0;
    std::uint64_t switchRefused = 0;
    std::uint64_t nanoseconds = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t dispatchExisting = 0;
    std::uint64_t dispatchNanoseconds = 0;
    std::uint64_t pipelinesQueued = 0;
    std::uint64_t pipelinesBuilt = 0;
    std::uint64_t pipelineNanoseconds = 0;
    std::uint64_t surfaceNanoseconds = 0;
    std::uint64_t targetNanoseconds = 0;
    std::uint64_t graphicsPipelineNanoseconds = 0;
    std::uint64_t uploadNanoseconds = 0;
    std::uint64_t knownServed = 0;
    std::uint64_t knownMapped = 0;
    std::uint64_t knownPaged = 0;
    std::uint64_t knownCopied = 0;
    std::uint64_t knownPointers = 0;
    std::uint64_t knownRefused = 0;
    std::uint64_t knownSkipped = 0;
    std::uint64_t knownWritten = 0;
    std::uint64_t knownWriteLookups = 0;
    std::uint64_t knownWriteBytes = 0;
    std::uint64_t copyBacks = 0;
    std::uint64_t copyBackBytes = 0;
    std::uint64_t copyBackCovered = 0;
    std::uint64_t importsReturned = 0;
    std::uint64_t knownGpuWritten = 0;
    std::uint64_t inputsResident = 0;
    std::uint64_t inputsImported = 0;
    std::uint64_t inputsWritten = 0;
    std::uint64_t inputsCopied = 0;
    std::uint64_t inputsGpuWritten = 0;
    std::uint64_t inputsAliased = 0;
    std::uint64_t inputsMerged = 0;
    std::map<std::string, std::uint64_t> reasons;
};

std::uint64_t elapsed(std::chrono::steady_clock::time_point from) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - from).count());
}

bool mergesPasses() {
    static const bool merges = std::getenv("APS5_RECODE_NO_PASS_MERGE") == nullptr;
    return merges;
}

bool memoizesTargets() {
    static const bool memoizes = std::getenv("APS5_RECODE_NO_TARGET_MEMO") == nullptr;
    return memoizes;
}

struct TargetMemo {
    bool valid = false;
    std::uint64_t recorder = 0;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t proof = 0;
    std::uint64_t pending = 0;
    std::uint64_t depthSerial = 0;
    std::uint64_t depthReleased = 0;
    std::vector<Graphics::ColorTarget> colors;
    std::vector<std::weak_ptr<Graphics::StorageTexture>> residents;
    std::vector<std::uint64_t> versions;
    std::vector<VkImageView> views;
    std::optional<Graphics::DepthTarget> depth;
    bool depthWrite = false;
    bool stencilTest = false;
    bool stencilWritten = false;
    VkImageView depthView = VK_NULL_HANDLE;
};

bool overlaps(std::span<const std::pair<std::uint64_t, std::uint64_t>> left, std::span<const std::pair<std::uint64_t, std::uint64_t>> right) {
    for (const auto& [begin, end] : left) {
        for (const auto& [otherBegin, otherEnd] : right) {
            if (begin < otherEnd && otherBegin < end) return true;
        }
    }
    return false;
}

bool servesFaultPages() {
    static const bool serves = std::getenv("APS5_RECODE_NO_FAULT_SERVE") == nullptr;
    return serves;
}

bool switchesWrittenChunks() {
    static const bool switches = std::getenv("APS5_RECODE_NO_WRITTEN_IMPORTS") == nullptr;
    return switches;
}

bool copiesBackDispatchWrites() {
    static const bool copies = std::getenv("APS5_RECODE_NO_COPY_BACK") == nullptr;
    return copies;
}

bool refusesGpuWrittenCopies() {
    static const bool refuses = std::getenv("APS5_RECODE_COPY_GPU_WRITTEN_KNOWN") == nullptr;
    return refuses;
}

struct DrawInput {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

enum class InputSource : std::uint8_t { Imported, Resident, Written, GpuWritten, Aliased };

std::optional<DrawInput> importedInput(const Context& context, std::uint64_t address, std::uint64_t bytes) {
    const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
    if (import == nullptr) return std::nullopt;
    return DrawInput{import->buffer, Graphics::HostImportOffset(*import, address)};
}

std::optional<DrawInput> drawInput(const Context& context, std::uint64_t address, std::uint64_t bytes, InputSource& source) {
    source = InputSource::Imported;
    auto& pages = MemoryCore::Get().Pages();
    if (!Shadow::ResidentDrawInputs()) return importedInput(context, address, bytes);
    if (pages.GpuWrittenIn(address, bytes) || !pages.GpuOwnedIn(address, bytes).empty()) {
        source = InputSource::GpuWritten;
    } else if (DirectMemoryAliased_nid_postfix(static_cast<std::uintptr_t>(address), static_cast<std::size_t>(bytes))) {
        source = InputSource::Aliased;
    } else {
        Shadow::OwnedSpan span;
        if (Shadow::ServeOwned(context, address, bytes, &span) && span.handle != 0u) {
            if (pages.CleanIn(address, bytes)) {
                source = InputSource::Resident;
                return DrawInput{reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span.handle)), static_cast<VkDeviceSize>(address - span.origin)};
            }
            source = InputSource::Written;
        }
    }
    return importedInput(context, address, bytes);
}

struct InputRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::size_t merged = 0;
};

bool mergesInputs() {
    static const bool merges = std::getenv("APS5_RECODE_NO_INPUT_MERGE") == nullptr;
    return merges;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> mergeInputs(std::vector<InputRange>& ranges) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unions;
    if (!mergesInputs()) {
        for (auto& range : ranges) {
            range.merged = unions.size();
            unions.emplace_back(range.begin, range.end);
        }
        return unions;
    }
    std::vector<std::size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, {}, [&](std::size_t index) { return ranges[index].begin; });
    for (const auto index : order) {
        auto& range = ranges[index];
        if (!unions.empty() && range.begin < unions.back().second) unions.back().second = std::max(unions.back().second, range.end);
        else unions.emplace_back(range.begin, range.end);
        range.merged = unions.size() - 1;
    }
    return unions;
}

void countInput(Totals& totals, InputSource source) {
    switch (source) {
        case InputSource::Resident: ++totals.inputsResident; break;
        case InputSource::Written: ++totals.inputsWritten; break;
        case InputSource::GpuWritten: ++totals.inputsGpuWritten; break;
        case InputSource::Aliased: ++totals.inputsAliased; break;
        case InputSource::Imported: ++totals.inputsImported; break;
    }
}

constexpr std::uint64_t KnownBufferLimitBytes = 256ull << 20u;
constexpr std::uint64_t KnownCopyLimitBytes = 64ull << 10u;
constexpr std::uint64_t KnownWriteLimitBytes = 1ull << 20u;
constexpr std::uint64_t InputCopyLimitBytes = 64ull << 10u;
constexpr std::uint32_t PagesPerChunk = static_cast<std::uint32_t>(ChunkBytes / PageBytes);

bool copiesInputs() {
    static const bool copies = std::getenv("APS5_RECODE_NO_INPUT_COPIES") == nullptr;
    return copies;
}

bool shaderDataBinding(const Resolve::ResolvedStage& stage, std::uint32_t index) {
    const auto& bindings = stage.program.result->bindings;
    return index < bindings.size() && bindings[index].role == ShaderRecompiler::DescriptorRole::ShaderData;
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
        if (color.samples > 1u) {
            reason = "multisampled color target";
            return false;
        }
        if (color.mipTail) {
            reason = "color target in a mip tail";
            return false;
        }
        if (color.layers != 1u) {
            reason = "layered color target";
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

struct ComputePipeline {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ComputePipeline() = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ~ComputePipeline() {
        if (pipeline != VK_NULL_HANDLE) destroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) destroyLayout(device, layout, nullptr);
    }
};

struct PipelineJob {
    std::vector<std::uint64_t> key;
    Graphics::Context context;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    VkDescriptorSetLayout main = VK_NULL_HANDLE;
    VkDescriptorSetLayout heap = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    VkPipelineCreateFlags flags = 0;
    VkPipelineShaderStageCreateFlags stageFlags = 0;
    std::uint32_t subgroupSize = 0;
};

bool dispatchPrecheck(const Resolve::ResolvedDraw& dispatch, std::string& reason) {
    if (dispatch.stages.size() != 1u || dispatch.stages.front().program.stage != ShaderRecompiler::ShaderStage::Compute) {
        reason = "not one compute stage";
        return false;
    }
    if (!dispatch.complete) {
        reason = "placeholder images";
        return false;
    }
    if (dispatch.operation.dispatch.indirect) {
        reason = "indirect dispatch";
        return false;
    }
    const auto& result = *dispatch.stages.front().program.result;
    if (result.workgroupMemoryDwords != 0u) {
        reason = "workgroup memory buffer";
        return false;
    }
    if (result.scratchPoolSlotDwords != 0u) {
        reason = "scratch pool";
        return false;
    }
    if (dispatch.stages.front().program.words.pushConstants.size() > Graphics::ComputePushConstantBytes) {
        reason = "push data outside the block";
        return false;
    }
    for (const auto& binding : dispatch.layout.bindings) {
        if (binding.count != 1u) {
            reason = "buffer array binding";
            return false;
        }
        if (binding.source == Bindings::Source::Guest || binding.source == Bindings::Source::Gds) {
            reason = "guest or GDS buffer binding";
            return false;
        }
        if (binding.source == Bindings::Source::TableMap && dispatch.stages.front().tableMap == nullptr) {
            reason = "table map without a mirror";
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
            landWrites();
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

    bool DispatchPrecheck(const Context& context, const VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason) {
        std::lock_guard lock(mutex);
        if (dispatchPrecheck(dispatch, reason) && pipelineReady(context, owner, dispatch, reason)) return true;
        totals.dispatchExisting++;
        const auto key = "existing path dispatch: " + reason.substr(0, 45);
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        return false;
    }

    bool RecordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
        std::lock_guard lock(mutex);
        const auto started = std::chrono::steady_clock::now();
        bool recorded = false;
        try {
            recorded = recordDispatch(context, dispatch, groups, reason);
            landWrites();
        } catch (const std::exception& error) {
            reason = std::string("record failed: ") + error.what();
        }
        totals.dispatchNanoseconds += elapsed(started);
        if (recorded) {
            totals.dispatches++;
            return true;
        }
        refuse("dispatch " + reason);
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
        line += "; batch moved before the pass " + std::to_string(totals.batchMoves);
        line += "; passes " + std::to_string(totals.passesBegun) + " begun, " + std::to_string(totals.passesContinued) + " draws continued, " + std::to_string(totals.passFeedback) + " broken by a target read";
        line += "; target memo " + std::to_string(totals.targetMemoHits) + " hits, " + std::to_string(totals.targetMemoMisses) + " misses; " + std::to_string(totals.deferredReads) + " draws with reads bound at record";
        line += "; residency updates landed " + std::to_string(totals.residencyLands);
        line += "; known buffers " + std::to_string(totals.knownServed) + " served, " + std::to_string(totals.knownMapped) + " mapped (" + std::to_string(totals.knownPaged) + " paged in), " + std::to_string(totals.knownCopied) + " copied (" + std::to_string(totals.knownGpuWritten) + " GPU-written ranges not copied), " + std::to_string(totals.knownRefused) + " unresolved, " + std::to_string(totals.knownSkipped) + " skipped (" + std::to_string(totals.knownPointers) + " of them pointers); known writes " + std::to_string(totals.knownWritten) + " direct (" + std::to_string(totals.knownWriteBytes >> 10u) + " KiB landed), " + std::to_string(totals.knownWriteLookups) + " through the page table, " + std::to_string(totals.copyBacks) + " copied back to their import after the dispatch (" + std::to_string(totals.copyBackBytes >> 20u) + " MiB, " + std::to_string(totals.copyBackCovered >> 10u) + " KiB of written pieces not landed, " + std::to_string(totals.importsReturned) + " chunks returned from their import)";
        line += "; draw inputs " + std::to_string(totals.inputsResident) + " resident, " + std::to_string(totals.inputsImported) + " imported, " + std::to_string(totals.inputsWritten) + " after CPU writes (" + std::to_string(totals.inputsCopied) + " copied into the ring), " + std::to_string(totals.inputsGpuWritten) + " GPU-written, " + std::to_string(totals.inputsAliased) + " aliased, " + std::to_string(totals.inputsMerged) + " served with an overlapping input";
        std::snprintf(text, sizeof(text), "; draw phases surfaces %.1f ms, targets %.1f ms, pipelines %.1f ms, owned uploads %.1f ms", static_cast<double>(totals.surfaceNanoseconds) / 1e6, static_cast<double>(totals.targetNanoseconds) / 1e6, static_cast<double>(totals.graphicsPipelineNanoseconds) / 1e6, static_cast<double>(totals.uploadNanoseconds) / 1e6);
        line += text;
        std::snprintf(text, sizeof(text), "; dispatches %llu recorded (%llu storage images written), %llu to the existing path, %.1f us per dispatch; compute pipelines %zu held, %llu queued, %llu built in %.1f ms, %zu building, %zu failed", static_cast<unsigned long long>(totals.dispatches), static_cast<unsigned long long>(totals.storageWrites), static_cast<unsigned long long>(totals.dispatchExisting), totals.dispatches == 0 ? 0.0 : static_cast<double>(totals.dispatchNanoseconds) / 1000.0 / static_cast<double>(totals.dispatches), computePipelines.size(), static_cast<unsigned long long>(totals.pipelinesQueued), static_cast<unsigned long long>(totals.pipelinesBuilt), static_cast<double>(totals.pipelineNanoseconds) / 1e6, buildingPipelines.size(), failedPipelines.size());
        line += text;
        std::snprintf(text, sizeof(text), "; table maps %llu hits, %llu uploads (%.1f MiB), %llu retired, %zu held (%.1f MiB)", static_cast<unsigned long long>(totals.mapHits), static_cast<unsigned long long>(totals.mapUploads), static_cast<double>(totals.mapUploadBytes) / 1048576.0, static_cast<unsigned long long>(totals.mapRetired), maps.size(), static_cast<double>(mapBytes) / 1048576.0);
        line += text;
        std::snprintf(text, sizeof(text), "; fault buffers %zu in flight, %llu read: %llu faults (%llu unmapped), %llu fault pages served, %llu written ranges (%llu pieces landed late), %llu written overflows", faults.size(), static_cast<unsigned long long>(totals.faultReads), static_cast<unsigned long long>(totals.faults), static_cast<unsigned long long>(totals.unmappedFaults), static_cast<unsigned long long>(totals.faultPages), static_cast<unsigned long long>(totals.writtenRanges), static_cast<unsigned long long>(totals.writtenLate), static_cast<unsigned long long>(totals.writtenOverflows));
        line += text;
        std::snprintf(text, sizeof(text), "; GPU-written chunks %llu switched to their import from the written bitmap (%.1f MiB copied over), %llu from other GPU writes, %llu refused, %llu written pieces already on an import", static_cast<unsigned long long>(totals.importSwitches), static_cast<double>(totals.switchBytes) / 1048576.0, static_cast<unsigned long long>(totals.writtenSwitches), static_cast<unsigned long long>(totals.switchRefused), static_cast<unsigned long long>(totals.writtenOnImports));
        line += text;
        for (const auto& [reasonCode, count] : faultReasons) line += "; fault reason " + std::to_string(reasonCode) + " " + std::to_string(count);
        faultReasons.clear();
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
        const auto ringMemory = ResidencyService::ShaderReadMemory(context, RingBytes);
        const VkBufferUsageFlags addressable = context.bufferDeviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u;
        ring = std::make_shared<Graphics::Buffer>(context, RingBytes + RingZeroBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | addressable, ringMemory);
        std::memset(ring->Bytes().data() + RingBytes, 0, RingZeroBytes);
        ringAddress = addressable != 0u ? ring->DeviceAddress() : 0u;
        space.emplace(RingBytes, alignment);
        std::fprintf(stderr, "[recode-record] ring %llu MiB, alignment %llu, %s memory\n", static_cast<unsigned long long>(RingBytes >> 20u), static_cast<unsigned long long>(alignment), (ringMemory & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 ? "device" : "host");
        return true;
    }

    void refuse(const std::string& reason) {
        totals.refused++;
        auto key = "dropped: " + reason.substr(0, 50);
        if (seen.insert(key).second) std::fprintf(stderr, "[recode-record] first drop: %s\n", reason.c_str());
        if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
    }

    void drainSurfaces(const Context& context, Graphics::Recorder& recorder) {
        auto* table = Resolve::Surfaces();
        if (table == nullptr) return;
        auto work = table->TakeWork();
        if (work.empty()) return;
        if (surfaceDetiler == nullptr) surfaceDetiler = std::make_unique<Images::SurfaceDetiler>(context);
        surfaceDetiler->Record(recorder, *table, work, [&](const std::string& why) {
            totals.surfaceFailures++;
            auto key = "surface: " + why.substr(0, 50);
            if (seen.insert(key).second) std::fprintf(stderr, "[recode-record] first surface failure: %s\n", why.c_str());
            if (totals.reasons.size() < 24u || totals.reasons.contains(key)) totals.reasons[key]++;
        });
    }

    void landWrites() {
        if (landing.empty()) return;
        const auto written = std::move(landing);
        landing.clear();
        Shadow::LandGpuWrites(written);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ends;
        ends.reserve(written.size());
        for (const auto& [begin, size] : written) ends.emplace_back(begin, begin + size);
        Events::NoteRanges(ends);
    }

    bool switchChunk(const Context& context, VkCommandBuffer commands, std::uint64_t chunk, bool copyWritten, bool& copied) {
        auto& residency = ResidencyService::Get();
        if (residency.WrittenImport(chunk)) return false;
        const auto* import = Graphics::HostImportFor(context, chunk, static_cast<std::size_t>(ChunkBytes));
        if (import == nullptr || chunk < import->base || chunk + ChunkBytes > import->base + import->bytes) {
            totals.switchRefused++;
            return false;
        }
        const auto found = writtenPages.find(chunk);
        const std::uint32_t mask = copyWritten && found != writtenPages.end() ? found->second : 0u;
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        for (std::uint32_t page = 0; page < PagesPerChunk;) {
            if ((mask & (1u << page)) == 0u) {
                ++page;
                continue;
            }
            auto last = page;
            while (last + 1u < PagesPerChunk && (mask & (1u << (last + 1u))) != 0u) ++last;
            const auto begin = chunk + page * PageBytes;
            const auto bytes = (last - page + 1u) * PageBytes;
            page = last + 1u;
            const auto span = residency.Translate(begin, bytes);
            if (!span) continue;
            if (!copied) Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            copied = true;
            const VkBufferCopy region{span->offset, Graphics::HostImportOffset(*import, begin), bytes};
            copy(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(span->handle)), import->buffer, 1, &region);
            totals.switchBytes += bytes;
        }
        return residency.MapWrittenImport(chunk, import->address + (chunk - import->base));
    }

    void applySwitches(const Context& context, Graphics::Recorder& recorder) {
        if (!switchesWrittenChunks()) return;
        auto others = Shadow::TakeGpuWrittenChunks();
        if (others.empty() && switches.empty()) return;
        const auto commands = recorder.Commands();
        bool copied = false;
        for (const auto chunk : switches) {
            if (switchChunk(context, commands, chunk, true, copied)) totals.importSwitches++;
        }
        for (const auto chunk : others) {
            if (switchChunk(context, commands, chunk, false, copied)) totals.writtenSwitches++;
        }
        switches.clear();
        if (copied) Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
    }

    void landWritten(std::uint64_t begin, std::uint64_t size) {
        auto& residency = ResidencyService::Get();
        for (auto at = begin; at < begin + size;) {
            const auto chunk = at & ~(ChunkBytes - 1u);
            const auto next = std::min(begin + size, chunk + ChunkBytes);
            if (residency.WrittenImport(chunk)) {
                totals.writtenOnImports++;
            } else {
                landing.emplace_back(at, next - at);
                totals.writtenLate++;
                for (auto page = (at - chunk) / PageBytes; page <= (next - 1u - chunk) / PageBytes; ++page) writtenPages[chunk] |= 1u << page;
                if (switchesWrittenChunks()) switches.insert(chunk);
            }
            at = next;
        }
    }

    void landKnownWrites(std::uint64_t completed) {
        while (!knownWrites.empty() && knownWrites.front().serial <= completed) {
            const auto& written = knownWrites.front();
            if (!ResidencyService::Get().WrittenImportThroughout(written.address, written.bytes)) {
                landWritten(written.address, written.bytes);
                totals.knownWriteBytes += written.bytes;
            }
            knownWrites.pop_front();
        }
    }

    bool queueCopyBack(const Context& context, std::uint64_t address, std::uint64_t bytes) {
        if (!copyingBack || bytes > KnownBufferLimitBytes) return false;
        const auto* import = Graphics::HostImportFor(context, address, static_cast<std::size_t>(bytes));
        if (import == nullptr || address < import->base || address + bytes > import->base + import->bytes) return false;
        returnImports(context, address, bytes, *import);
        copyBacks.push_back({address, bytes, import});
        return true;
    }

    void returnImports(const Context& context, std::uint64_t address, std::uint64_t bytes, const Graphics::HostImport& import) {
        auto& residency = ResidencyService::Get();
        if (!residency.WrittenImportAny(address, bytes)) return;
        std::vector<std::pair<std::uint64_t, VkBufferCopy>> regions;
        for (auto chunk = address & ~(ChunkBytes - 1u); chunk < address + bytes; chunk += ChunkBytes) {
            if (chunk < import.base || chunk + ChunkBytes > import.base + import.bytes || !residency.WrittenImport(chunk)) continue;
            const auto span = residency.ReturnWrittenImport(chunk);
            if (!span) continue;
            regions.push_back({span->handle, VkBufferCopy{Graphics::HostImportOffset(import, chunk), span->offset, ChunkBytes}});
        }
        if (regions.empty()) return;
        const auto commands = context.recorder->Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        for (const auto& [handle, region] : regions) copy(commands, import.buffer, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle)), 1, &region);
        totals.importsReturned += regions.size();
    }

    void recordCopyBacks(const Context& context, VkCommandBuffer commands, std::uint64_t serial) {
        if (copyBacks.empty()) return;
        auto& residency = ResidencyService::Get();
        const auto copy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        auto& covered = copiedBack[serial];
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ends;
        std::vector<VkBufferCopy> regions;
        std::uint64_t handle = 0;
        VkBuffer target = VK_NULL_HANDLE;
        const auto flush = [&] {
            if (!regions.empty()) copy(commands, reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(handle)), target, static_cast<std::uint32_t>(regions.size()), regions.data());
            regions.clear();
        };
        for (const auto& back : copyBacks) {
            const auto end = back.address + back.bytes;
            const auto whole = residency.WrittenImportAny(back.address, back.bytes) ? std::nullopt : residency.Translate(back.address, back.bytes);
            if (whole) {
                if (whole->handle != handle || back.import->buffer != target) {
                    flush();
                    handle = whole->handle;
                    target = back.import->buffer;
                }
                regions.push_back({whole->offset, Graphics::HostImportOffset(*back.import, back.address), back.bytes});
                totals.copyBackBytes += back.bytes;
            }
            for (auto at = whole ? end : back.address; at < end;) {
                const auto chunk = at & ~(ChunkBytes - 1u);
                const auto next = std::min(end, chunk + ChunkBytes);
                const auto span = residency.WrittenImport(chunk) ? std::nullopt : residency.Translate(at, next - at);
                if (span) {
                    if (span->handle != handle || back.import->buffer != target) {
                        flush();
                        handle = span->handle;
                        target = back.import->buffer;
                    }
                    const auto destination = Graphics::HostImportOffset(*back.import, at);
                    if (!regions.empty() && regions.back().srcOffset + regions.back().size == span->offset && regions.back().dstOffset + regions.back().size == destination) regions.back().size += next - at;
                    else regions.push_back({span->offset, destination, next - at});
                    totals.copyBackBytes += next - at;
                }
                at = next;
            }
            covered.emplace_back(back.address, end);
            ends.emplace_back(back.address, end);
            totals.copyBacks++;
        }
        flush();
        copyBacks.clear();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        for (const auto& [begin, end] : ends) {
            Events::Shared().Note(begin, end - begin);
            MemoryCore::Get().Pages().NoteGpuWritten(begin, end - begin);
        }
    }

    void landUncovered(std::uint64_t begin, std::uint64_t size, const std::vector<std::pair<std::uint64_t, std::uint64_t>>* covered) {
        const auto end = begin + size;
        if (covered == nullptr) {
            landWritten(begin, size);
            return;
        }
        for (auto at = begin; at < end;) {
            const auto hit = std::ranges::find_if(*covered, [&](const auto& range) { return range.first <= at && at < range.second; });
            if (hit != covered->end()) {
                const auto stop = std::min(end, hit->second);
                totals.copyBackCovered += stop - at;
                at = stop;
                continue;
            }
            auto stop = end;
            for (const auto& range : *covered) {
                if (range.first > at && range.first < stop) stop = range.first;
            }
            landWritten(at, stop - at);
            at = stop;
        }
    }

    std::uint64_t serveKnownWrite(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, bool copiedBackWrite) {
        auto& residency = ResidencyService::Get();
        if (residency.WrittenImportThroughout(address, bytes)) return residency.TranslateMapped(address, bytes, true);
        if (bytes > (copiedBackWrite ? KnownBufferLimitBytes : KnownWriteLimitBytes)) return 0u;
        auto mapped = residency.TranslateMapped(address, bytes, true);
        if (mapped == 0u && bytes <= KnownWriteLimitBytes && servesFaultPages()) {
            if (const auto pages = residency.UnmappedPages(address, bytes); !pages.empty()) {
                Shadow::ServeFaultPages(pages);
                mapped = residency.TranslateMapped(address, bytes, true);
            }
        }
        if (mapped == 0u || copiedBackWrite) return mapped;
        if (knownWrites.empty() || knownWrites.back().serial != serial || knownWrites.back().address != address || knownWrites.back().bytes != bytes) knownWrites.push_back({serial, address, bytes});
        return mapped;
    }

    void readFaults(Graphics::Buffer& buffer, std::uint64_t serial) {
        namespace Abi = ShaderRecompiler::BdaAbi;
        auto bytes = buffer.Bytes();
        auto* words = reinterpret_cast<std::uint32_t*>(bytes.data());
        totals.faultReads++;
        if (words[Abi::WrittenOverflowWord] != 0u) totals.writtenOverflows++;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
        if (Abi::ForEachWrittenRange(words, [&](std::uint64_t begin, std::size_t size) { written.emplace_back(begin, size); })) {
            totals.writtenRanges += written.size();
            const auto found = copiedBack.find(serial);
            for (const auto& [begin, size] : written) landUncovered(begin, size, found == copiedBack.end() ? nullptr : &found->second);
        }
        copiedBack.erase(copiedBack.begin(), copiedBack.upper_bound(serial));
        Abi::Fault report{};
        std::memcpy(&report, bytes.data(), sizeof(report));
        if (report.state == Abi::FaultState::Ready) {
            totals.faults++;
            const auto code = static_cast<std::uint32_t>(report.reason);
            faultReasons[code]++;
            if (report.reason == Abi::FaultReason::Unmapped) totals.unmappedFaults++;
            if (seenFaults.insert(code).second) std::fprintf(stderr, "[recode-record] first fault: reason %u at 0x%llx, %u bytes, stage %u, pc 0x%x\n", code, static_cast<unsigned long long>(report.address), report.bytes, report.stage, report.instruction);
            std::vector<std::uint64_t> pages;
            if (Abi::ForEachFaultPage(words, [&](std::uint64_t page) { pages.push_back(page); }) && !pages.empty()) {
                totals.faultPages += pages.size();
                if (servesFaultPages()) Shadow::ServeFaultPages(pages);
            }
        }
        std::memset(bytes.data(), 0, bytes.size());
    }

    std::shared_ptr<Graphics::Buffer> faultFor(const Context& context, std::uint64_t serial, std::uint64_t completed) {
        while (!faults.empty() && faults.front().serial <= completed && faults.front().serial != serial) {
            readFaults(*faults.front().buffer, faults.front().serial);
            spareFaults.push_back(std::move(faults.front().buffer));
            faults.pop_front();
        }
        if (!faults.empty() && faults.back().serial == serial) return faults.back().buffer;
        std::shared_ptr<Graphics::Buffer> buffer;
        if (!spareFaults.empty()) {
            buffer = std::move(spareFaults.back());
            spareFaults.pop_back();
        } else {
            buffer = std::make_shared<Graphics::Buffer>(context, ShaderRecompiler::BdaAbi::FaultBufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            std::memset(buffer->Bytes().data(), 0, buffer->Bytes().size());
        }
        faults.push_back({buffer, serial});
        return buffer;
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
        auto buffer = std::make_shared<Graphics::Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ResidencyService::ShaderReadMemory(context, bytes));
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

    template<typename TPut>
    void serveKnownBuffers(const Context& context, const Resolve::ResolvedStage& stage, std::uint64_t serial, std::uint64_t completed, TPut&& put) {
        if (stage.program.variant == nullptr) return;
        static const std::vector<ShaderRecompiler::DescriptorValue> none;
        const auto& layout = stage.program.variant->info.bindings;
        const auto& known = stage.program.capture != nullptr ? stage.program.capture->snapshot.knownBuffers : none;
        const auto& infos = stage.program.variant->info.info.knownBuffers;
        landKnownWrites(completed);
        for (std::uint32_t index = 0; index < layout.knownBufferCount; ++index) {
            const auto dword = layout.KnownBufferDword() + index * ShaderRecompiler::KnownBufferDwords;
            const auto write = [&](std::uint64_t device, std::uint64_t bytes) {
                put(dword, static_cast<std::uint32_t>(device));
                put(dword + 1u, static_cast<std::uint32_t>(device >> 32u));
                put(dword + 2u, static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, 0xffffffffu)));
            };
            const auto [address, bytes] = index < known.size() ? Resolve::KnownBufferSpan(index < infos.size() ? infos[index] : ShaderRecompiler::KnownBuffer{}, known[index]) : std::pair<std::uint64_t, std::uint64_t>{0u, 0u};
            if (index < infos.size() && infos[index].Pointer()) totals.knownPointers++;
            const auto zero = ringAddress != 0u ? ringAddress + RingBytes : 0u;
            if (address == 0u || bytes == 0u) {
                totals.knownSkipped++;
                write(zero, 0u);
                continue;
            }
            if (index < infos.size() && infos[index].written) {
                const auto mapped = serveKnownWrite(address, bytes, serial, queueCopyBack(context, address, bytes));
                if (mapped != 0u) totals.knownWritten++;
                else totals.knownWriteLookups++;
                write(mapped, mapped != 0u ? bytes : 0u);
                continue;
            }
            if (const auto copied = copyKnownBuffer(address, bytes, serial, completed); copied != 0u) {
                totals.knownCopied++;
                write(copied, bytes);
                continue;
            }
            const auto reach = std::max(bytes, KnownReadableBytes);
            Shadow::OwnedSpan span;
            if (bytes <= KnownBufferLimitBytes && !MemoryCore::Get().Pages().GpuWrittenIn(address, reach) && ((reach != bytes && Shadow::ServeOwned(context, address, reach, &span)) || Shadow::ServeOwned(context, address, bytes, &span)) && span.address != 0u) {
                totals.knownServed++;
                write(span.address, bytes);
                continue;
            }
            auto mapped = ResidencyService::Get().TranslateMapped(address, reach);
            if (mapped == 0u && reach != bytes) mapped = ResidencyService::Get().TranslateMapped(address, bytes);
            if (mapped == 0u && bytes <= KnownBufferLimitBytes && servesFaultPages()) {
                if (const auto pages = ResidencyService::Get().UnmappedPages(address, bytes); !pages.empty()) {
                    Shadow::ServeFaultPages(pages);
                    mapped = ResidencyService::Get().TranslateMapped(address, bytes);
                    if (mapped != 0u) totals.knownPaged++;
                }
            }
            if (mapped != 0u) {
                totals.knownMapped++;
                write(mapped, bytes);
                continue;
            }
            totals.knownRefused++;
            write(zero, 0u);
        }
    }

    std::optional<std::uint64_t> ringCopy(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        if (!GuestArena::GuestArenaContains_nid_postfix(reinterpret_cast<const void*>(address), static_cast<std::size_t>(bytes))) return std::nullopt;
        const auto committed = GuestMemory::CommittedRanges(address, static_cast<std::size_t>(bytes));
        if (committed.size() != 1u || committed.front().first > address || committed.front().second < address + bytes) return std::nullopt;
        if (!MemoryCore::Get().Pages().GpuOwnedIn(address, bytes).empty()) return std::nullopt;
        return ringWrite(std::span(reinterpret_cast<const std::byte*>(address), static_cast<std::size_t>(bytes)), serial, completed);
    }

    std::uint64_t copyKnownBuffer(std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        if (ringAddress == 0u || bytes > KnownCopyLimitBytes) return 0u;
        if (refusesGpuWrittenCopies() && MemoryCore::Get().Pages().GpuWrittenIn(address, bytes)) {
            totals.knownGpuWritten++;
            return 0u;
        }
        const auto offset = ringCopy(address, bytes, serial, completed);
        return offset.has_value() ? ringAddress + *offset : 0u;
    }

    struct InputCopy {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        std::size_t merged = 0;
    };

    struct CopiedSpan {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        VkDeviceSize offset = 0;
    };

    std::optional<DrawInput> copiedInput(std::vector<CopiedSpan>& done, std::uint64_t address, std::uint64_t bytes, std::uint64_t serial, std::uint64_t completed) {
        for (const auto& copy : done) {
            if (copy.address <= address && address + bytes <= copy.address + copy.bytes) return DrawInput{ring->Handle(), copy.offset + (address - copy.address)};
        }
        const auto offset = ringCopy(address, bytes, serial, completed);
        if (!offset.has_value()) return std::nullopt;
        done.push_back({address, bytes, *offset});
        return DrawInput{ring->Handle(), *offset};
    }

    void servePushKnownBuffers(const Context& context, const Resolve::ResolvedStage& stage, std::span<std::byte> block, std::uint64_t serial, std::uint64_t completed) {
        if (stage.program.variant == nullptr || !stage.program.variant->info.bindings.UsesPushData()) return;
        serveKnownBuffers(context, stage, serial, completed, [&](std::uint32_t dword, std::uint32_t value) {
            const auto offset = static_cast<std::size_t>(dword) * sizeof(std::uint32_t);
            if (offset + sizeof(value) <= block.size()) std::memcpy(block.data() + offset, &value, sizeof(value));
        });
    }

    bool writeSet(const Context& context, const Resolve::ResolvedDraw& draw, std::uint32_t set, VkDescriptorSet target, std::uint64_t serial, std::uint64_t completed, std::shared_ptr<Graphics::Buffer>& pageTable, std::string& reason) {
        std::vector<VkDescriptorBufferInfo> infos;
        std::vector<VkWriteDescriptorSet> writes;
        infos.reserve(draw.layout.bindings.size());
        for (const auto& binding : draw.layout.bindings) {
            if (binding.set != set) continue;
            VkDescriptorBufferInfo info{};
            switch (binding.source) {
                case Bindings::Source::Words: {
                    const auto& stage = draw.stages.at(binding.stage);
                    const auto words = wordsOf(stage, binding.index);
                    const bool shaderData = shaderDataBinding(stage, binding.index) && stage.program.variant != nullptr;
                    const auto dataBytes = shaderData ? static_cast<std::size_t>(stage.program.variant->info.bindings.ShaderDataDwords()) * sizeof(std::uint32_t) : 0u;
                    std::vector<std::byte> bytes(std::max<std::size_t>({words.size_bytes(), dataBytes, 16u}));
                    std::memcpy(bytes.data(), words.data(), words.size_bytes());
                    if (shaderData) {
                        serveKnownBuffers(context, stage, serial, completed, [&](std::uint32_t dword, std::uint32_t value) {
                            const auto offset = static_cast<std::size_t>(dword) * sizeof(std::uint32_t);
                            if (offset + sizeof(value) <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value));
                        });
                    }
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
                case Bindings::Source::Fault: info = {faultFor(context, serial, completed)->Handle(), 0, VK_WHOLE_SIZE}; break;
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
            write.dstSet = target;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &infos.back();
            writes.push_back(write);
        }
        if (!writes.empty()) context.Resolved(&Graphics::DeviceFunctions::updateDescriptorSets, "vkUpdateDescriptorSets")(context.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
        return true;
    }

    VkDescriptorSetLayout emptySetLayout(const Context& context) {
        if (emptyLayout != VK_NULL_HANDLE) return emptyLayout;
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        Graphics::Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &info, nullptr, &emptyLayout), "vkCreateDescriptorSetLayout recode dispatch");
        return emptyLayout;
    }

    static std::vector<std::uint64_t> pipelineKey(const Resolve::ResolvedDraw& dispatch) {
        const auto hash = dispatch.stages.front().program.result->spirv.ContentHash();
        std::vector<std::uint64_t> key{hash.low, hash.high};
        key.insert(key.end(), dispatch.layout.keys[0].begin(), dispatch.layout.keys[0].end());
        return key;
    }

    static std::shared_ptr<ComputePipeline> buildComputePipeline(const PipelineJob& job) {
        const auto& context = job.context;
        const auto& result = *job.result;
        auto made = std::make_shared<ComputePipeline>();
        made->device = context.device;
        made->destroyPipeline = context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
        made->destroyLayout = context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
        const std::array<VkDescriptorSetLayout, ShaderRecompiler::RuntimeAbi::DirectHeapSet + 1u> setLayouts{job.main, job.empty, job.empty, job.empty, job.heap};
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::ComputePushConstantBytes};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        Graphics::Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &made->layout), "vkCreatePipelineLayout recode dispatch");
        const Graphics::PipelineSpecialization specialization(result, context.atomicLockTable);
        const auto module = Graphics::CreateShaderModule(context, result.spirv.data(), result.spirv.size() * sizeof(std::uint32_t), "vkCreateShaderModule recode dispatch");
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.flags = job.flags;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.flags = job.stageFlags;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.stage.pSpecializationInfo = specialization.Info();
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT};
        subgroup.requiredSubgroupSize = job.subgroupSize;
        if (job.subgroupSize != 0u) info.stage.pNext = &subgroup;
        info.layout = made->layout;
        VkResult created = VK_SUCCESS;
        {
            const auto cacheLock = context.LockPipelineCacheForUse();
            created = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, context.pipelineCache, 1, &info, nullptr, &made->pipeline);
        }
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        Graphics::Check(created, "vkCreateComputePipelines recode dispatch");
        return made;
    }

    bool pipelineReady(const Context& context, const VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason) {
        auto key = pipelineKey(dispatch);
        if (computePipelines.contains(key)) return true;
        if (const auto failed = failedPipelines.find(key); failed != failedPipelines.end()) {
            reason = "compute pipeline failed: " + failed->second;
            return false;
        }
        reason = "compute pipeline building";
        if (buildingPipelines.contains(key)) return false;
        const auto* heap = Resolve::HeapDevice();
        if (heap == nullptr) {
            reason = "no heap";
            return false;
        }
        const auto& result = dispatch.stages.front().program.result;
        PipelineJob job{key, context, result, setLayout(context, dispatch, 0), heap->Layout(), emptySetLayout(context), owner.computePipelineFlags(), 0u, owner.computeSubgroupSize(*result)};
        try {
            job.stageFlags = owner.computeStageFlags(*result);
        } catch (const std::exception& error) {
            failedPipelines.emplace(std::move(key), error.what());
            reason = std::string("compute pipeline failed: ") + error.what();
            return false;
        }
        buildingPipelines.insert(key);
        pipelineJobs.push_back(std::move(job));
        totals.pipelinesQueued++;
        if (!builder.joinable()) builder = std::thread([this] { buildPipelines(); });
        jobsReady.notify_one();
        return false;
    }

    void buildPipelines() {
        for (;;) {
            std::optional<PipelineJob> job;
            {
                std::unique_lock lock(mutex);
                jobsReady.wait(lock, [&] { return !pipelineJobs.empty(); });
                job.emplace(std::move(pipelineJobs.front()));
                pipelineJobs.pop_front();
            }
            const auto started = std::chrono::steady_clock::now();
            std::shared_ptr<ComputePipeline> made;
            std::string failure;
            try {
                made = buildComputePipeline(*job);
            } catch (const std::exception& error) {
                failure = error.what();
            }
            std::lock_guard lock(mutex);
            buildingPipelines.erase(job->key);
            totals.pipelineNanoseconds += elapsed(started);
            if (made != nullptr) {
                computePipelines.emplace(std::move(job->key), std::move(made));
                totals.pipelinesBuilt++;
            } else {
                if (seen.insert("pipeline: " + failure.substr(0, 50)).second) std::fprintf(stderr, "[recode-record] first compute pipeline failure: %s\n", failure.c_str());
                failedPipelines.emplace(std::move(job->key), std::move(failure));
            }
        }
    }

    bool recordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
        if (dispatch.stages.size() != 1u || dispatch.stages.front().deferredImages.empty()) return recordBound(context, dispatch, groups, {}, reason);
        auto bound = dispatch;
        auto& stage = bound.stages.front();
        std::vector<std::shared_ptr<Graphics::StorageTexture>> written;
        written.reserve(stage.deferredImages.size());
        for (const auto& deferred : stage.deferredImages) {
            std::uint32_t slot = 0;
            if ((deferred.request.flags & Heap::RequestFlag::Storage) == 0u) {
                auto owner = Resolve::BindDeferredSampled(context, deferred.request, slot, reason);
                if (owner == nullptr) return false;
                stage.slots[deferred.index] = slot;
                if (context.recorder != nullptr) context.recorder->Keep(std::const_pointer_cast<void>(owner));
                continue;
            }
            auto texture = Resolve::BindDeferredImage(context, deferred.request, slot, reason);
            if (texture == nullptr) return false;
            stage.slots[deferred.index] = slot;
            if (context.recorder != nullptr) context.recorder->Keep(texture);
            if ((deferred.request.flags & Heap::RequestFlag::Written) != 0u) written.push_back(std::move(texture));
        }
        if (!Resolve::PatchSlots(*stage.program.result, stage.slots, stage.program.words, reason)) return false;
        return recordBound(context, bound, groups, written, reason);
    }

    bool recordBound(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::span<const std::shared_ptr<Graphics::StorageTexture>> written, std::string& reason) {
        auto* recorder = context.recorder;
        const auto* heap = Resolve::HeapDevice();
        if (recorder == nullptr || heap == nullptr) {
            reason = "no recorder or heap";
            return false;
        }
        if (!attach(context, reason) || !dispatchPrecheck(dispatch, reason)) return false;
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (groups[axis] > context.limits.maxComputeWorkGroupCount[axis]) {
                reason = "group count past the device limit";
                return false;
            }
        }
        drainSurfaces(context, *recorder);
        const auto& stage = dispatch.stages.front();
        const auto main = setLayout(context, dispatch, 0);
        const auto found = computePipelines.find(pipelineKey(dispatch));
        if (found == computePipelines.end()) {
            reason = "compute pipeline not ready";
            return false;
        }
        const auto pipeline = found->second;

        recorder->OpenBatch();
        const auto serial = recorder->OpenBatchSerial();
        const auto completed = recorder->CompletedThrough();
        std::shared_ptr<Graphics::Buffer> pageTable;
        applySwitches(context, *recorder);
        const auto set = allocateSet(context, main, serial, completed);
        copyBacks.clear();
        copyingBack = copiesBackDispatchWrites();
        std::array<std::byte, Graphics::ComputePushConstantBytes> block{};
        const auto& push = stage.program.words.pushConstants;
        const bool setWritten = writeSet(context, dispatch, 0, set, serial, completed, pageTable, reason);
        if (setWritten) {
            if (!push.empty()) std::memcpy(block.data(), push.data(), push.size());
            servePushKnownBuffers(context, stage, block, serial, completed);
        }
        copyingBack = false;
        if (!setWritten) {
            copyBacks.clear();
            return false;
        }

        if (Shadow::OwnedUploadsPending()) {
            std::vector<std::shared_ptr<void>> kept;
            if (Shadow::RecordOwnedUploads(recorder->Commands(), kept) != 0u) totals.residencyLands++;
            for (auto& object : kept) recorder->Keep(std::move(object));
        }
        if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
        if (recorder->HasQueuedStores()) recorder->FlushStores();
        const auto commands = recorder->Commands();
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        context.BindComputePipeline(commands, pipeline->pipeline);
        const auto bind = context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, ShaderRecompiler::RuntimeAbi::DescriptorSet, 1, &set, 0, nullptr);
        const VkDescriptorSet heapSet = heap->Set();
        bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, ShaderRecompiler::RuntimeAbi::DirectHeapSet, 1, &heapSet, 0, nullptr);
        if (!push.empty()) context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, Graphics::ComputePushConstantBytes, block.data());
        noteFaultToken(context, *recorder, commands, dispatch.stages, FaultStage::compute, "dispatch", groups[0], groups[1] * groups[2]);
        const auto dispatchTiming = recorder->BeginGpuTiming(DispatchTimingKey);
        context.Resolved(&Graphics::DeviceFunctions::cmdDispatch, "vkCmdDispatch")(commands, groups[0], groups[1], groups[2]);
        recorder->EndGpuTiming(dispatchTiming);
        Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_HOST_READ_BIT);
        recordCopyBacks(context, commands, serial);

        recorder->Keep(pipeline);
        for (const auto& owned : stage.owners) recorder->Keep(std::const_pointer_cast<void>(owned));
        if (stage.tableMap != nullptr) recorder->Keep(std::const_pointer_cast<Tables::Mirror>(stage.tableMap));
        if (pageTable != nullptr) recorder->Keep(pageTable);
        for (const auto& texture : written) {
            recorder->Keep(texture);
            texture->MarkDirty();
            static_cast<void>(Graphics::DepthSurfaceStoreBack(context, *texture));
            totals.storageWrites++;
        }
        if (!written.empty()) GuestMemory::BreakProofs();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writtenRanges;
        for (std::size_t index = 0; index < dispatch.uses.size(); ++index) {
            const auto& use = dispatch.uses[index];
            if (use.write) writtenRanges.emplace_back(use.address, use.address + use.size);
        }
        if (!writtenRanges.empty()) Events::NoteRanges(writtenRanges);
        return true;
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
        if (std::ranges::all_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return stage.deferredImages.empty(); })) return recordWithSlots(context, draw, parameters, reason);
        auto bound = draw;
        for (auto& stage : bound.stages) {
            if (stage.deferredImages.empty()) continue;
            for (const auto& deferred : stage.deferredImages) {
                std::uint32_t slot = 0;
                auto owner = Resolve::BindDeferredSampled(context, deferred.request, slot, reason);
                if (owner == nullptr) return false;
                stage.slots[deferred.index] = slot;
                if (context.recorder != nullptr) context.recorder->Keep(std::const_pointer_cast<void>(owner));
            }
            if (!Resolve::PatchSlots(*stage.program.result, stage.slots, stage.program.words, reason)) return false;
        }
        totals.deferredReads++;
        return recordWithSlots(context, bound, parameters, reason);
    }

    bool recordWithSlots(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
        auto* recorder = context.recorder;
        const auto* heap = Resolve::HeapDevice();
        if (recorder == nullptr || heap == nullptr || draw.state == nullptr) {
            reason = "no recorder or heap";
            return false;
        }
        if (!attach(context, reason) || !precheck(draw, parameters, nullptr, reason)) return false;
        auto phase = std::chrono::steady_clock::now();
        drainSurfaces(context, *recorder);
        totals.surfaceNanoseconds += elapsed(phase);
        const auto& state = *draw.state;
        std::vector<Graphics::CompiledShader> shaders;
        for (const auto& stage : draw.stages) shaders.push_back({stage.program.stage, stage.program.result.get(), stage.program.pushOffset});
        const auto& attributes = draw.stages.front().program.vertexAttributes;
        Graphics::VertexInputLayout vertexInput;
        std::vector<VkBuffer> vertexBuffers;
        std::vector<VkDeviceSize> vertexOffsets;
        std::vector<InputCopy> inputCopies;
        std::vector<InputRange> inputRanges;
        if (!attributes.empty()) {
            vertexInput = Graphics::BuildVertexInputLayout(context, attributes);
            for (std::size_t index = 0; index < attributes.size(); ++index) {
                const auto& attribute = attributes[index];
                if (vertexInput.attributes[index].format != Graphics::DecodeVertexFormat(attribute).format) {
                    reason = "widened vertex format";
                    return false;
                }
                const auto address = vertexAddress(attribute);
                inputRanges.push_back({address, address + Graphics::VertexBufferExtent(attribute), 0});
            }
            const auto unions = mergeInputs(inputRanges);
            std::vector<DrawInput> unionInputs;
            std::vector<InputSource> unionSources;
            for (std::size_t index = 0; index < unions.size(); ++index) {
                const auto& [begin, end] = unions[index];
                InputSource source = InputSource::Imported;
                const auto input = drawInput(context, begin, end - begin, source);
                if (!input) {
                    reason = "vertex buffer not imported";
                    return false;
                }
                if (source == InputSource::Written && copiesInputs() && end - begin <= InputCopyLimitBytes) inputCopies.push_back({begin, end - begin, index});
                recorder->NotePendingRead(begin, end - begin, Graphics::Recorder::ReadKind::DrawInput);
                unionInputs.push_back(*input);
                unionSources.push_back(source);
            }
            for (const auto& range : inputRanges) {
                countInput(totals, unionSources[range.merged]);
                vertexBuffers.push_back(unionInputs[range.merged].buffer);
                vertexOffsets.push_back(unionInputs[range.merged].offset + (range.begin - unions[range.merged].first));
            }
            totals.inputsMerged += inputRanges.size() - unions.size();
        }
        std::vector<VkImageView> views;
        std::vector<std::shared_ptr<void>> owners;
        std::vector<std::shared_ptr<Graphics::StorageTexture>> residents;
        phase = std::chrono::steady_clock::now();
        const bool memoUsable = std::exchange(targetMemo.valid, false);
        if (memoUsable && targetsFromMemo(context, *recorder, state, views, owners, residents)) {
            totals.targetMemoHits++;
        } else {
            if (memoizesTargets()) totals.targetMemoMisses++;
            views.clear();
            owners.clear();
            residents.clear();
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
                Images::Targets().Note(resident);
                owners.push_back(resident);
                residents.push_back(std::move(resident));
            }
            if (state.depth) {
                views.push_back(Graphics::DepthSurfaceViewWithStencilWriteIntent(context, *state.depth, state.depthWrite, state.stencilTest, writesStencil(state)));
            }
        }
        Graphics::PipelineSetLayouts sets;
        sets.main = setLayout(context, draw, 0);
        sets.split = draw.layout.split;
        if (sets.split) sets.fragment = setLayout(context, draw, 1);
        sets.mainKey = draw.layout.keys[0];
        sets.fragmentKey = draw.layout.keys[1];
        sets.heap = heap->Layout();
        totals.targetNanoseconds += elapsed(phase);
        phase = std::chrono::steady_clock::now();
        auto pipeline = Graphics::CachedPipeline(context, state, vertexInput, sets, shaders, VK_IMAGE_LAYOUT_GENERAL);
        totals.graphicsPipelineNanoseconds += elapsed(phase);
        if (pipeline == nullptr) {
            reason = "no pipeline";
            return false;
        }
        auto framebuffer = pipeline->AcquireFramebuffer(views, owners, state.renderExtent, state.renderLayers);
        const auto masked = maskedOutputs(state, fragmentOutputs(shaders));

        recorder->OpenBatch();
        const auto serial = recorder->OpenBatchSerial();
        const auto completed = recorder->CompletedThrough();
        std::vector<CopiedSpan> copiedInputs;
        for (const auto& copy : inputCopies) {
            const auto input = copiedInput(copiedInputs, copy.address, copy.bytes, serial, completed);
            if (!input) continue;
            for (std::size_t index = 0; index < inputRanges.size(); ++index) {
                if (inputRanges[index].merged != copy.merged) continue;
                vertexBuffers[index] = input->buffer;
                vertexOffsets[index] = input->offset + (inputRanges[index].begin - copy.address);
                totals.inputsCopied++;
            }
        }
        std::array<VkDescriptorSet, 2> descriptorSets{};
        std::shared_ptr<Graphics::Buffer> pageTable;
        for (std::uint32_t set = 0; set < (sets.split ? 2u : 1u); ++set) {
            descriptorSets[set] = allocateSet(context, set == 0u ? sets.main : sets.fragment, serial, completed);
            if (!writeSet(context, draw, set, descriptorSets[set], serial, completed, pageTable, reason)) return false;
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
            servePushKnownBuffers(context, draw.stages[index], std::span(block).subspan(shaders[index].pushConstantOffset), serial, completed);
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
        std::optional<DrawInput> indexInput;
        if (parameters.indexed) {
            const auto bytes = static_cast<std::size_t>(parameters.indexCount) * parameters.indexSize;
            InputSource source = InputSource::Imported;
            if (bytes != 0u) indexInput = drawInput(context, parameters.indexAddress, bytes, source);
            if (!indexInput) {
                reason = "indices not imported";
                return false;
            }
            countInput(totals, source);
            if (source == InputSource::Written && copiesInputs() && bytes <= InputCopyLimitBytes) {
                if (const auto copied = copiedInput(copiedInputs, parameters.indexAddress, bytes, serial, completed)) {
                    indexInput = copied;
                    totals.inputsCopied++;
                }
            }
            recorder->NotePendingRead(parameters.indexAddress, bytes, Graphics::Recorder::ReadKind::DrawInput);
        }

        applySwitches(context, *recorder);
        if (Shadow::OwnedUploadsPending()) {
            phase = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<void>> kept;
            const auto uploadCommands = recorder->Commands();
            const auto uploadTiming = recorder->BeginGpuTiming(UploadTimingKey);
            if (Shadow::RecordOwnedUploads(uploadCommands, kept) != 0u) totals.residencyLands++;
            recorder->EndGpuTiming(uploadTiming);
            for (auto& object : kept) recorder->Keep(std::move(object));
            totals.uploadNanoseconds += elapsed(phase);
        }
        if (recorder->HasQueuedKeyStores()) recorder->FlushKeyStores();
        if (recorder->HasQueuedStores()) recorder->FlushStores();
        const auto key = Graphics::DrawRenderPassKey(context, state, views);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> passReads;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> passWrites;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> drawTargets;
        for (std::size_t index = 0; index < draw.uses.size(); ++index) {
            const auto& use = draw.uses[index];
            if (use.size == 0u) continue;
            const std::pair range{use.address, use.address + use.size};
            if (index >= draw.targetBegin && index < draw.targetEnd) drawTargets.push_back(range);
            else if (use.write) passWrites.push_back(range);
            else passReads.push_back(range);
        }
        if (!passWrites.empty()) Shadow::NoteGpuWrites(passWrites);
        std::vector<std::pair<VkImage, bool>> attachments;
        for (const auto& resident : residents) attachments.emplace_back(resident->Image(), true);
        Graphics::PassAccess access{passReads, passWrites, {}, attachments};
        access.anyReads = pageTable != nullptr;
        access.anyWrites = pageTable != nullptr && std::ranges::any_of(draw.stages, [](const Resolve::ResolvedStage& stage) { return Graphics::ShaderStoresThroughAddresses(stage.program.result->spirv); });
        const bool feedback = recorder->RenderPassOpen() && overlaps(passReads, passTargets);
        if (feedback) totals.passFeedback++;
        const bool snapshotArguments = parameters.indirect && ring != nullptr && argumentBuffer == ring->Handle();
        const bool forced = !mergesPasses() || !draw.complete || feedback || (parameters.indirect && !snapshotArguments) || countImport != nullptr;
        const bool fenced = state.depth.has_value();
        const auto start = recorder->StartDrawPass(key, forced, fenced, access);
        if (recorder->OpenBatchSerial() != serial) totals.batchMoves++;
        const auto commands = start.commands;
        auto& recording = recorder->GraphicsState();
        if (start.continued) {
            totals.passesContinued++;
            for (const auto& range : drawTargets) {
                if (std::ranges::find(passTargets, range) == passTargets.end()) passTargets.push_back(range);
            }
        } else {
            totals.passesBegun++;
            passTargets = std::move(drawTargets);
            recorder->PrepareSampleSlot();
        }
        if (start.barrier) {
            Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
        }
        const auto* maskedState = masked.has_value() ? &*masked : nullptr;
        if (start.continued) {
            pipeline->Continue(recording, state, maskedState);
        } else {
            if (parameters.indirect) Graphics::RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
            pipeline->Begin(recording, *framebuffer, state.renderExtent, state, maskedState);
        }

        const auto bind = context.Resolved(&Graphics::DeviceFunctions::cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DescriptorSet, 1, &descriptorSets[0], 0, nullptr);
        if (sets.split) bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet, 1, &descriptorSets[1], 0, nullptr);
        const VkDescriptorSet heapSet = heap->Set();
        bind(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Layout(), ShaderRecompiler::RuntimeAbi::DirectHeapSet, 1, &heapSet, 0, nullptr);
        pipeline->PushConstants(commands, Graphics::PushConstantStages(shaders), block);

        if (!vertexBuffers.empty()) context.Resolved(&Graphics::DeviceFunctions::cmdBindVertexBuffers, "vkCmdBindVertexBuffers")(commands, 0, static_cast<std::uint32_t>(vertexBuffers.size()), vertexBuffers.data(), vertexOffsets.data());
        if (indexInput) context.Resolved(&Graphics::DeviceFunctions::cmdBindIndexBuffer, "vkCmdBindIndexBuffer")(commands, indexInput->buffer, indexInput->offset, parameters.indexSize == 2u ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        noteFaultToken(context, *recorder, commands, draw.stages, FaultStage::graphics, parameters.indirect ? "indirect" : (parameters.indexed ? "indexed" : "draw"), parameters.indexCount, parameters.instanceCount);
        std::uint64_t timedProgram = 0;
        for (const auto& one : draw.stages) {
            if (timedProgram == 0 || one.program.stage == ShaderRecompiler::ShaderStage::Fragment) timedProgram = one.program.codeAddress;
        }
        const auto drawTiming = recorder->BeginInPassGpuTiming(DrawTimingKey | (std::uint64_t{0x7ec} << 48u) | (timedProgram & 0xffffffffffffull));
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
        recorder->EndGpuTiming(drawTiming);

        recorder->LeaveRenderPassOpen(key, 0, fenced, access);
        recorder->Keep(pipeline);
        recorder->Keep(framebuffer);
        for (const auto& stage : draw.stages) {
            for (const auto& owner : stage.owners) recorder->Keep(std::const_pointer_cast<void>(owner));
            if (stage.tableMap != nullptr) recorder->Keep(std::const_pointer_cast<Tables::Mirror>(stage.tableMap));
        }
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
        if (memoizesTargets()) rememberTargets(*recorder, state, views, residents);
        return true;
    }

    bool targetsFromMemo(const Context& context, const Graphics::Recorder& recorder, const Graphics::State& state, std::vector<VkImageView>& views, std::vector<std::shared_ptr<void>>& owners, std::vector<std::shared_ptr<Graphics::StorageTexture>>& residents) {
        const auto& memo = targetMemo;
        if (recorder.Id() != memo.recorder || !recorder.RenderPassOpen() || recorder.RenderPassGeneration() != memo.generation) return false;
        if (GuestMemory::CollectEpoch() != memo.epoch || GuestMemory::ProofSerial() != memo.proof || Graphics::StorageTexture::PendingSerial() != memo.pending) return false;
        if (state.colors.size() != memo.colors.size() || !std::equal(state.colors.begin(), state.colors.end(), memo.colors.begin())) return false;
        if (state.depth.has_value() != memo.depth.has_value()) return false;
        if (state.depth && (!(*state.depth == *memo.depth) || state.depthWrite != memo.depthWrite || state.stencilTest != memo.stencilTest || writesStencil(state) != memo.stencilWritten || Graphics::DepthSurfacesSerial() != memo.depthSerial || Graphics::DepthSurfacesReleased() != memo.depthReleased)) return false;
        for (std::size_t index = 0; index < memo.residents.size(); ++index) {
            auto resident = memo.residents[index].lock();
            if (resident == nullptr || resident->Version() != memo.versions[index] || !Graphics::StorageImageCached(context, resident.get()) || !Graphics::StorageImageServesKeys(*resident, Graphics::SurfaceForTarget(state.colors[index]).dccAddress)) return false;
            views.push_back(memo.views[index]);
            owners.push_back(resident);
            residents.push_back(std::move(resident));
        }
        if (state.depth) views.push_back(memo.depthView);
        return true;
    }

    void rememberTargets(const Graphics::Recorder& recorder, const Graphics::State& state, std::span<const VkImageView> views, std::span<const std::shared_ptr<Graphics::StorageTexture>> residents) {
        auto& memo = targetMemo;
        const auto epoch = GuestMemory::CollectEpoch();
        const bool eligible = epoch != 0 && recorder.RenderPassOpen() && views.size() == residents.size() + (state.depth ? 1u : 0u) && (!state.depth || state.depth->samples == 1u) && std::ranges::all_of(state.colors, [](const Graphics::ColorTarget& color) { return color.samples == 1u && !color.staged && color.partialWriteMask == 0u; }) && std::ranges::all_of(residents, [](const std::shared_ptr<Graphics::StorageTexture>& resident) { return resident->Cached() && resident->Descriptor().tileMode != Graphics::TextureTileMode::kLinear; });
        if (!eligible) return;
        memo.valid = true;
        memo.recorder = recorder.Id();
        memo.generation = recorder.RenderPassGeneration();
        memo.epoch = epoch;
        memo.proof = GuestMemory::ProofSerial();
        memo.pending = Graphics::StorageTexture::PendingSerial();
        memo.depthSerial = Graphics::DepthSurfacesSerial();
        memo.depthReleased = Graphics::DepthSurfacesReleased();
        memo.colors.assign(state.colors.begin(), state.colors.end());
        memo.residents.assign(residents.begin(), residents.end());
        memo.versions.clear();
        for (const auto& resident : residents) memo.versions.push_back(resident->Version());
        memo.views.assign(views.begin(), views.begin() + static_cast<std::ptrdiff_t>(residents.size()));
        memo.depth = state.depth;
        memo.depthWrite = state.depthWrite;
        memo.stencilTest = state.stencilTest;
        memo.stencilWritten = state.depth ? writesStencil(state) : false;
        memo.depthView = state.depth ? views.back() : VK_NULL_HANDLE;
    }

    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t alignment = 16;
    std::shared_ptr<Graphics::Buffer> ring;
    std::uint64_t ringAddress = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> passTargets;
    TargetMemo targetMemo;
    std::deque<FaultBuffer> faults;
    std::deque<KnownWrite> knownWrites;
    std::vector<CopyBack> copyBacks;
    std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> copiedBack;
    bool copyingBack = false;
    std::set<std::uint64_t> switches;
    std::unordered_map<std::uint64_t, std::uint32_t> writtenPages;
    std::vector<std::shared_ptr<Graphics::Buffer>> spareFaults;
    std::map<std::uint32_t, std::uint64_t> faultReasons;
    std::optional<RingSpace> space;
    std::map<std::vector<std::uint32_t>, VkDescriptorSetLayout> layouts;
    std::deque<std::pair<VkDescriptorPool, std::uint64_t>> pools;
    std::vector<VkDescriptorPool> spare;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> outputs;
    std::map<std::uint64_t, MapBuffer> maps;
    std::uint64_t mapBytes = 0;
    std::set<std::string> seen;
    std::set<std::uint32_t> seenFaults;
    std::unique_ptr<Images::SurfaceDetiler> surfaceDetiler;
    std::map<std::vector<std::uint64_t>, std::shared_ptr<ComputePipeline>> computePipelines;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> landing;
    std::set<std::vector<std::uint64_t>> buildingPipelines;
    std::map<std::vector<std::uint64_t>, std::string> failedPipelines;
    std::deque<PipelineJob> pipelineJobs;
    std::condition_variable jobsReady;
    std::thread builder;
    VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;
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

bool IdleStores() {
    static const bool idle = !RecordActive() || std::getenv("APS5_RECODE_IDLE_STORE") != nullptr;
    return idle;
}

bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason) {
    return shared().Precheck(draw, parameters, reads, reason);
}

bool RecordResolved(const Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason) {
    return shared().Record(context, draw, parameters, reason);
}

bool RecordDispatchPrecheck(const Context& context, const VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason) {
    return shared().DispatchPrecheck(context, owner, dispatch, reason);
}

bool RecordDispatch(const Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason) {
    return shared().RecordDispatch(context, dispatch, groups, reason);
}

std::string RecordLine() {
    return shared().Line();
}

}
