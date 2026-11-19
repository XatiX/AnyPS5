#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWBINDINGS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWBINDINGS_HPP

#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Unified::Bindings {

enum class Source : std::uint8_t {
    Words,
    Guest,
    PageTable,
    Fault,
    Gds,
};

struct StageInput {
    ShaderRecompiler::ShaderStage stage = ShaderRecompiler::ShaderStage::Vertex;
    const ShaderRecompiler::RecompileResult* result = nullptr;
};

struct Binding {
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t count = 1;
    std::uint32_t stages = 0;
    std::uint32_t stage = 0;
    std::uint32_t index = 0;
    Source source = Source::Words;
};

struct Layout {
    std::vector<Binding> bindings;
    std::array<std::vector<std::uint32_t>, 2> keys;
    bool split = false;
};

struct SourceCounts {
    std::uint64_t words = 0;
    std::uint64_t guest = 0;
    std::uint64_t pageTable = 0;
    std::uint64_t fault = 0;
    std::uint64_t gds = 0;
};

[[nodiscard]] std::uint32_t StageBit(ShaderRecompiler::ShaderStage stage);
bool Describe(std::span<const StageInput> stages, Layout& out, std::string& reason);
void CountSources(const Layout& layout, SourceCounts& counts);

}

#endif
