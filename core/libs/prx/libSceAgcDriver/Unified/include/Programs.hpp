#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROGRAMS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROGRAMS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Unified/include/Templates.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "CompiledVariant.hpp"
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Resolve {

struct ProgramInputs {
    const QueueState& queue;
    const DriverDetail::ShaderRegistry& registry;
    const Graphics::Context& context;
    const ShaderRecompiler::SpirvTarget& target;
    bool fixedSlots = false;
    TemplateTable* templates = nullptr;
    bool verifyTemplates = false;
    const ShaderRecompiler::SpirvTarget* computeWave32 = nullptr;
    const ShaderRecompiler::SpirvTarget* computeWave64 = nullptr;
};

struct StageProgram {
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    std::uint64_t codeAddress = 0;
    std::uint32_t pushOffset = 0;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    std::shared_ptr<const ShaderRecompiler::CompiledVariant> variant;
    std::shared_ptr<const ShaderRecompiler::ResultTemplate> shape;
    ShaderRecompiler::TemplateWords words;
    std::vector<ShaderRecompiler::VertexAttribute> vertexAttributes;
    std::vector<std::array<std::uint32_t, 8>> targetMerges;
};

enum ProgramPhase : std::size_t {
    PhaseDecode,
    PhasePrepare,
    PhaseCapture,
    PhaseMaterialize,
    PhaseVariant,
    PhaseCount,
};

struct ProgramSet {
    DriverDetail::DrawDecode decode;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<StageProgram> stages;
    std::array<std::uint64_t, PhaseCount> nanoseconds{};
    std::uint32_t templateMismatches = 0;
    const char* templateMismatch = nullptr;
};

[[nodiscard]] ShaderRecompiler::SpirvTarget HeapTarget(const ShaderRecompiler::SpirvTarget& device);
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> KnownBufferSpan(const ShaderRecompiler::KnownBuffer& known, const ShaderRecompiler::DescriptorValue& value);
[[nodiscard]] bool DecodePrograms(const ProgramInputs& inputs, DriverDetail::DrawDecode& decode, std::string& reason);
[[nodiscard]] bool BuildPrograms(const ProgramInputs& inputs, const DrawFacts& facts, ProgramSet& out, std::string& reason);
[[nodiscard]] bool BuildComputeProgram(const ProgramInputs& inputs, const DispatchFacts& facts, ProgramSet& out, std::string& reason);

}

#endif
