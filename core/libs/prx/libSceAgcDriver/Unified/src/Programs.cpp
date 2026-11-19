#include "prx/libSceAgcDriver/Unified/include/Programs.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <spirv/unified1/spirv.hpp>
#include <cstdlib>
#include <exception>
#include <string_view>

namespace AgcDriver::Unified::Resolve {

namespace {

using Stage = ShaderRecompiler::ShaderStage;
using Role = ShaderRecompiler::ProgramRole;

bool readOnlyDepthBoundsCompare() {
    static const bool enabled = [] {
        const auto* value = std::getenv("APS5_READONLY_DEPTH_BOUNDS_COMPARE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

template <typename T>
std::uint64_t capacityBytes(const std::vector<T>& values) {
    return static_cast<std::uint64_t>(values.capacity()) * sizeof(T);
}

void reportLargeTemplate(const ShaderRecompiler::ResultTemplate& shape) {
    static std::atomic<bool> reported{false};
    const auto bytes = ShaderRecompiler::ResultTemplateBytes(shape);
    if (bytes < (256u << 10u) || reported.exchange(true)) return;
    const auto& result = *shape.result;
    std::uint64_t descriptors = 0;
    std::uint64_t largestBinding = 0;
    for (const auto& binding : result.bindings) {
        descriptors += capacityBytes(binding.guestDescriptor);
        largestBinding = std::max<std::uint64_t>(largestBinding, binding.guestDescriptor.size());
    }
    std::uint64_t readRanges = capacityBytes(result.imageTableReadRanges);
    for (const auto& ranges : result.imageTableReadRanges) readRanges += capacityBytes(ranges);
    std::fprintf(stderr, "[recode-resolve] first large template: %llu bytes; %zu bindings, descriptor words %llu bytes (largest %llu words), push %zu, table poison %llu, table ranges %llu, table read ranges %llu, vertex patches %llu, runtime images %llu, fragment parameters %llu, vertex attributes %llu\n", static_cast<unsigned long long>(bytes), result.bindings.size(), static_cast<unsigned long long>(descriptors), static_cast<unsigned long long>(largestBinding), result.pushConstants.size(), static_cast<unsigned long long>(capacityBytes(result.imageTablePoison)), static_cast<unsigned long long>(capacityBytes(result.imageTableRanges)), static_cast<unsigned long long>(readRanges), static_cast<unsigned long long>(capacityBytes(result.vertexInputPatches)), static_cast<unsigned long long>(capacityBytes(result.runtimeImageResources)), static_cast<unsigned long long>(capacityBytes(result.fragmentParameters)), static_cast<unsigned long long>(capacityBytes(result.vertexAttributes)));
}

bool usesSamplerTables(const ShaderRecompiler::ShaderInfo& info) {
    return std::ranges::any_of(info.samplers, [](const auto& sampler) { return sampler.table != ShaderRecompiler::NoTable; });
}

}

ShaderRecompiler::SpirvTarget HeapTarget(const ShaderRecompiler::SpirvTarget& device) {
    auto target = device;
    target.directResources = true;
    return target;
}

bool DecodePrograms(const ProgramInputs& inputs, DriverDetail::DrawDecode& decode, std::string& reason) {
    const auto& queue = inputs.queue;
    decode.state = Graphics::DecodeState(queue, DriverDetail::VertexWorkgroupRequired(queue, inputs.registry));
    DriverDetail::DecodeGraphicsPrograms(decode, queue, inputs.registry, false, true);
    auto& state = decode.state;
    if (state.stages.mesh || state.stages.tessellation || state.stages.tessellationGeometry) {
        reason = "mesh or tessellation stages";
        return false;
    }
    if (std::ranges::any_of(decode.roles, [](Role role) { return role == Role::GeometryBack; })) {
        reason = "geometry stage";
        return false;
    }
    if (state.sampleLocationGrid) {
        reason = "sample location grid";
        return false;
    }
    if (std::ranges::any_of(decode.programs, [](const DriverDetail::DrawProgram& program) { return program.binary.codeAddress == DriverDetail::DepthCopyPixelProgramAddress(true) || program.binary.codeAddress == DriverDetail::StencilCopyPixelProgramAddress(true); })) {
        reason = "depth or stencil copy program";
        return false;
    }
    std::span<const std::byte> fragmentHeader;
    for (const auto& program : decode.programs) {
        if (program.binary.stage != Stage::Fragment) continue;
        fragmentHeader = program.binary.header;
        break;
    }
    auto& pixel = decode.pixel;
    pixel = Graphics::DecodePixelStageInfo(queue.context, Graphics::ExportMappings(state), Graphics::PixelProgramSkipped(queue), fragmentHeader);
    Graphics::NormalizeUnusedViewportDepth(state, pixel);
    Graphics::ApplyViewportIndex(inputs.context, state);
    Graphics::ApplyDepthRemap(inputs.context, state);
    Graphics::ApplyDepthBoundsEmulation(inputs.context, state, pixel);
    Graphics::ApplyReadOnlyDepthBoundsComparison(state, pixel, readOnlyDepthBoundsCompare());
    pixel.targetExportPacking = Graphics::ExportPackings(state);
    pixel.dualSourceBlend = state.dualSourceBlend;
    pixel.reversedBlend = state.reversedBlend;
    pixel.targetMerges = Graphics::ExportTargetMerges(state);
    pixel.alphaToCoverage = state.alphaToCoverage;
    pixel.quadPixelMask = Graphics::QuadPixelMask(state);
    pixel.quadSampleMask = Graphics::QuadSampleMasks(state, pixel.sampleOrder);
    if (pixel.stencilRefExportEnable && Graphics::StencilReferenceUnused(state)) {
        const auto& capabilities = inputs.target.supportedCapabilities;
        if (std::ranges::find(capabilities, static_cast<std::uint32_t>(spv::CapabilityStencilExportEXT)) == capabilities.end()) pixel.stencilRefExportEnable = false;
    }
    if (pixel.ancillary) pixel.rasterPrimitive = Graphics::RasterPrimitiveOf(state);
    if (pixel.ancillary) pixel.layerExported = state.layerExports != 0;
    pixel.ditheredAlphaToCoverage = state.ditheredAlphaToCoverage;
    pixel.ditheredAlphaSamples = state.ditheredAlphaSamples;
    pixel.iteratesSamples = state.perSampleShading;
    pixel.sampleLocations = Graphics::PixelSampleLocations(state, pixel);
    return true;
}

bool BuildPrograms(const ProgramInputs& inputs, const DrawFacts& facts, ProgramSet& out, std::string& reason) try {
    out.stages.clear();
    out.memory.clear();
    out.nanoseconds = {};
    auto mark = std::chrono::steady_clock::now();
    const auto lap = [&](ProgramPhase phase) {
        const auto now = std::chrono::steady_clock::now();
        out.nanoseconds[phase] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
        mark = now;
    };
    const bool decoded = DecodePrograms(inputs, out.decode, reason);
    lap(PhaseDecode);
    if (!decoded) return false;
    const auto& decode = out.decode;
    const auto& programs = decode.programs;
    const auto& state = decode.state;
    auto& memory = out.memory;
    memory.reserve(2 * programs.size() + 32);
    DriverDetail::AppendProgramMemory(memory, programs);
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    linked.reserve(programs.size());
    for (std::size_t i = 0; i < programs.size(); ++i) linked.push_back({decode.roles[i], programs[i].binary, programs[i].userDataBase, programs[i].firstUserSgpr, programs[i].userData});
    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos(programs.size());
    std::vector<Graphics::DecodeRead> reads;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment) continue;
        reads.clear();
        auto info = DriverDetail::DecodeOrderedVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, reads, 8u - program.firstUserSgpr);
        info.paClVsOutCntl = DriverDetail::PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, DriverDetail::readRegister(inputs.queue.context, 0x207), program.tailContinuation.get()) | state.layerExports | state.viewportExports;
        info.depthRemapMode = state.depthRemap.mode;
        Graphics::ApplyPointSize(state, info);
        vertexInfos[i] = info;
    }
    ShaderMemory shaderMemory(memory, nullptr, nullptr, nullptr, &DriverDetail::Driver::QueryPendingSpan);
    const bool hasFragment = std::ranges::any_of(programs, [](const DriverDetail::DrawProgram& program) { return program.binary.stage == Stage::Fragment; });
    std::uint32_t pushCursor = 0;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        const auto stage = program.binary.stage;
        if (program.snapshot == nullptr) {
            reason = "program without a snapshot";
            return false;
        }
        pushCursor = Graphics::StagePushOffset(pushCursor, stage, inputs.fixedSlots);
        if (pushCursor > Graphics::PushBlockBytes(inputs.fixedSlots)) {
            reason = "push offset past the push block";
            return false;
        }
        const auto budget = Graphics::StagePushBudget(Graphics::StagePushLimit(state.stages, pushCursor, state.depthRemap.mode != 0u), pushCursor, stage, inputs.fixedSlots, hasFragment);
        ShaderRecompiler::RecompileRequest request{
            program.binary,
            {Graphics::StageWaveSize(state.stages, stage), program.firstUserSgpr, program.userData, std::nullopt, stage == Stage::Fragment ? std::optional(decode.pixel) : std::nullopt, vertexInfos[i], memory, DriverDetail::RegisteredFloatMode(*program.snapshot)},
            inputs.target,
            {0, 0, pushCursor, budget},
            ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, Graphics::StageMeshConfiguration(state.stages, stage), state.stages.tessellation, {facts.indexAddress, facts.indexCount, facts.indexSize, facts.instanceCount}, std::nullopt},
        };
        lap(PhaseDecode);
        const auto source = ShaderRecompiler::PrepareShader(request);
        lap(PhaseVariant);
        if (source == nullptr || source->artifact == nullptr) {
            reason = "compiled variant not reachable";
            return false;
        }
        if (usesSamplerTables(source->artifact->info.info)) {
            reason = "sampler tables";
            return false;
        }
        auto invocation = DriverDetail::InvocationFor(*program.snapshot, program.codeOffset, request, false);
        lap(PhasePrepare);
        const auto capture = DriverDetail::CaptureInvocation(shaderMemory, *program.snapshot, program.codeOffset, request, invocation);
        lap(PhaseCapture);
        memory = shaderMemory.Regions();
        request.context.memory = memory;
        std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
        std::shared_ptr<const ShaderRecompiler::ResultTemplate> shape;
        ShaderRecompiler::TemplateWords words;
        if (inputs.templates != nullptr) {
            const auto key = invocation.TemplateKey(*capture);
            shape = inputs.templates->Find(key);
            const bool stale = shape != nullptr && !shape->refillable && shape->snapshotKey != invocation.SnapshotKey(*capture);
            if (shape == nullptr || stale) {
                shape = invocation.BuildTemplate(*capture);
                inputs.templates->CountBuild(stale);
                inputs.templates->Store(key, shape);
                reportLargeTemplate(*shape);
            }
            result = shape->result;
            if (shape->refillable) {
                ShaderRecompiler::RefillTemplate(*shape, capture->snapshot, words);
            } else {
                words.pushConstants = result->pushConstants;
                words.poisonedSrtReads = result->poisonedSrtReads;
            }
            if (inputs.verifyTemplates) {
                const auto full = invocation.MaterializeUnshared(*capture);
                if (const auto* difference = ShaderRecompiler::TemplateWordsDifference(*shape, words, *full)) {
                    out.templateMismatches++;
                    out.templateMismatch = difference;
                }
            }
        } else {
            result = invocation.Materialize(*capture);
        }
        lap(PhaseMaterialize);
        if (result == nullptr) {
            reason = "materialize failed";
            return false;
        }
        if (!capture->snapshot.tables.nested.empty()) {
            reason = "nested image tables";
            return false;
        }
        if (result->pushConstants.size() > budget) {
            reason = "push constants past the stage budget";
            return false;
        }
        std::vector<ShaderRecompiler::VertexAttribute> attributes = result->vertexAttributes;
        for (auto& attribute : attributes) {
            const auto& live = vertexInfos[i];
            if (!live.has_value() || attribute.location >= live->resources.size()) {
                reason = "vertex attribute without live vertex info";
                return false;
            }
            const auto& fields = live->resources[attribute.location].fields;
            if (fields[3] != attribute.resource.fields[3] || (fields[1] >> 16u) != (attribute.resource.fields[1] >> 16u)) {
                reason = "vertex format differs from the template";
                return false;
            }
            attribute.resource = live->resources[attribute.location];
        }
        out.stages.push_back({stage, program.binary.codeAddress, result->pushConstants.empty() ? 0u : pushCursor, capture, std::move(result), source->artifact, std::move(shape), std::move(words), std::move(attributes)});
        pushCursor += static_cast<std::uint32_t>(out.stages.back().result->pushConstants.size());
    }
    return true;
} catch (const std::exception& error) {
    reason = error.what();
    return false;
}

}
