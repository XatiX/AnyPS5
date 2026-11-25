#include "prx/libSceAgcDriver/Unified/include/DrawBindings.hpp"
#include "RuntimeAbi.hpp"
#include <algorithm>

namespace AgcDriver::Unified::Bindings {

namespace {

bool sourceOf(ShaderRecompiler::DescriptorRole role, Source& source) {
    using ShaderRecompiler::DescriptorRole;
    switch (role) {
        case DescriptorRole::GuestBuffers: source = Source::Guest; return true;
        case DescriptorRole::FlattenedSrt:
        case DescriptorRole::ShaderData: source = Source::Words; return true;
        case DescriptorRole::BdaPagetable: source = Source::PageTable; return true;
        case DescriptorRole::FaultBuffer: source = Source::Fault; return true;
        case DescriptorRole::Gds: source = Source::Gds; return true;
        case DescriptorRole::ImageTableMap: source = Source::TableMap; return true;
        default: return false;
    }
}

}

std::uint32_t StageBit(ShaderRecompiler::ShaderStage stage) {
    return 1u << static_cast<std::uint32_t>(stage);
}

bool Describe(std::span<const StageInput> stages, Layout& out, std::string& reason) {
    out = {};
    for (std::uint32_t stage = 0; stage < stages.size(); ++stage) {
        const auto* result = stages[stage].result;
        if (result == nullptr) {
            reason = "stage without a program";
            return false;
        }
        for (std::uint32_t index = 0; index < result->bindings.size(); ++index) {
            const auto& binding = result->bindings[index];
            if (binding.descriptorSet == ShaderRecompiler::RuntimeAbi::DirectHeapSet) continue;
            if (result->directHeap && (binding.role == ShaderRecompiler::DescriptorRole::ImageTable || binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable || binding.role == ShaderRecompiler::DescriptorRole::SamplerTable)) continue;
            if (binding.descriptorSet != ShaderRecompiler::RuntimeAbi::DescriptorSet && binding.descriptorSet != ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet) {
                reason = "binding in set " + std::to_string(binding.descriptorSet);
                return false;
            }
            Source source{};
            if (!sourceOf(binding.role, source)) {
                reason = "binding role " + std::to_string(static_cast<int>(binding.role)) + " outside the heap";
                return false;
            }
            const std::uint32_t set = binding.descriptorSet == ShaderRecompiler::RuntimeAbi::FragmentDescriptorSet ? 1u : 0u;
            const auto shared = std::ranges::find_if(out.bindings, [&](const Binding& other) { return other.set == set && other.binding == binding.binding; });
            if (shared != out.bindings.end()) {
                reason = "binding " + std::to_string(binding.binding) + " shared between stages";
                return false;
            }
            out.split = out.split || set == 1u;
            out.bindings.push_back({set, binding.binding, std::max(binding.count, 1u), StageBit(stages[stage].stage), stage, index, source});
        }
    }
    std::ranges::sort(out.bindings, [](const Binding& left, const Binding& right) { return left.set != right.set ? left.set < right.set : left.binding < right.binding; });
    for (const auto& binding : out.bindings) {
        auto& key = out.keys[binding.set];
        key.insert(key.end(), {binding.binding, binding.count, binding.stages});
    }
    return true;
}

std::uint32_t OutputLocations(std::span<const std::uint32_t> spirv) {
    constexpr std::uint32_t opDecorate = 71;
    constexpr std::uint32_t opVariable = 59;
    constexpr std::uint32_t decorationLocation = 30;
    constexpr std::uint32_t storageOutput = 3;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> locations;
    std::vector<std::uint32_t> outputs;
    for (std::size_t word = 5; word < spirv.size();) {
        const auto count = spirv[word] >> 16u;
        const auto opcode = spirv[word] & 0xffffu;
        if (count == 0u || word + count > spirv.size()) break;
        if (opcode == opDecorate && count >= 4u && spirv[word + 2] == decorationLocation) locations.emplace_back(spirv[word + 1], spirv[word + 3]);
        if (opcode == opVariable && count >= 4u && spirv[word + 3] == storageOutput) outputs.push_back(spirv[word + 2]);
        word += count;
    }
    std::uint32_t mask = 0;
    for (const auto& [id, location] : locations) {
        if (location < 32u && std::ranges::find(outputs, id) != outputs.end()) mask |= 1u << location;
    }
    return mask;
}

void CountSources(const Layout& layout, SourceCounts& counts) {
    for (const auto& binding : layout.bindings) {
        switch (binding.source) {
            case Source::Words: counts.words++; break;
            case Source::Guest: counts.guest++; break;
            case Source::PageTable: counts.pageTable++; break;
            case Source::Fault: counts.fault++; break;
            case Source::Gds: counts.gds++; break;
            case Source::TableMap: counts.tableMap++; break;
        }
    }
}

}
