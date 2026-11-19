#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SLOTPATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SLOTPATCH_HPP

#include "Optimization/ResourceProgram.hpp"
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Unified::Resolve {

[[nodiscard]] bool PatchSlots(const ShaderRecompiler::RecompileResult& result, std::span<const std::uint32_t> slots, ShaderRecompiler::TemplateWords& words, std::string& reason);

}

#endif
