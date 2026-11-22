#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_INDIRECTARGUMENTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_INDIRECTARGUMENTS_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>

namespace AgcDriver::Unified::Record {

inline constexpr std::uint32_t PatchedRecordLimit = 1024;

[[nodiscard]] bool PatchableArguments(const Pm4::DrawParameters::IndirectDraw& arguments);
void PatchIndirectArguments(const Graphics::Context& context, VkCommandBuffer commands, const Pm4::DrawParameters::IndirectDraw& arguments, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset);

}

#endif
