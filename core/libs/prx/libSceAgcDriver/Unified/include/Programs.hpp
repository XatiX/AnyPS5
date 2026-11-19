#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROGRAMS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_PROGRAMS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "CompiledVariant.hpp"
#include <memory>
#include <string>
#include <vector>

namespace AgcDriver::Unified::Resolve {

struct ProgramInputs {
    const QueueState& queue;
    const DriverDetail::ShaderRegistry& registry;
    const Graphics::Context& context;
    const ShaderRecompiler::SpirvTarget& target;
    bool fixedSlots = false;
};

struct StageProgram {
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    std::uint64_t codeAddress = 0;
    std::uint32_t pushOffset = 0;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    std::shared_ptr<const ShaderRecompiler::CompiledVariant> variant;
};

struct ProgramSet {
    DriverDetail::DrawDecode decode;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<StageProgram> stages;
};

[[nodiscard]] ShaderRecompiler::SpirvTarget HeapTarget(const ShaderRecompiler::SpirvTarget& device);
[[nodiscard]] bool DecodePrograms(const ProgramInputs& inputs, DriverDetail::DrawDecode& decode, std::string& reason);
[[nodiscard]] bool BuildPrograms(const ProgramInputs& inputs, const DrawFacts& facts, ProgramSet& out, std::string& reason);

}

#endif
