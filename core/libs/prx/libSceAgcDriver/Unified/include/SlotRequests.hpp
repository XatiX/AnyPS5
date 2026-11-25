#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SLOTREQUESTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SLOTREQUESTS_HPP

#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace AgcDriver::Unified::Heap {

namespace RequestFlag {
inline constexpr std::uint32_t Sampler = 1u;
inline constexpr std::uint32_t Point = 2u;
inline constexpr std::uint32_t Inactive = 4u;
inline constexpr std::uint32_t Storage = 8u;
inline constexpr std::uint32_t Written = 16u;
inline constexpr std::uint32_t Atomic = 32u;
inline constexpr std::uint32_t Atomic64 = 64u;
inline constexpr std::uint32_t UintView = 256u;
inline constexpr std::uint32_t Compare = 512u;
inline constexpr std::uint32_t Unnormalized = 1024u;
inline constexpr std::uint32_t MipShift = 16u;
}

struct SlotRequest {
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    std::array<std::uint32_t, 8> words{};
};

struct SlotStatistics {
    std::uint64_t images = 0;
    std::uint64_t samplers = 0;
    std::uint64_t inactiveImages = 0;
};

[[nodiscard]] std::uint32_t PointFilterWord(std::uint32_t filter);

[[nodiscard]] bool BuildSlotRequests(const ShaderRecompiler::CompiledShaderInfo& compiled, const ShaderRecompiler::ResourceSnapshot& snapshot, std::vector<SlotRequest>& requests, SlotStatistics& statistics, std::string& reason, bool allowStorage);

}

#endif
