#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Unified::Record {

[[nodiscard]] bool RecordActive();
[[nodiscard]] bool RecordsKernels(std::uint32_t queue);
enum class Kernel : std::uint8_t { Fill, Copy };
void NoteKernel(Kernel kernel);
void PublishRecords(const Graphics::Context& context, std::uint64_t address, std::uint64_t bytes);
[[nodiscard]] bool IdleStores();
bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason);
bool RecordResolved(const Graphics::Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason);
bool RecordDispatchPrecheck(const Graphics::Context& context, VulkanDevice& owner, const Resolve::ResolvedDraw& dispatch, std::string& reason);
bool RecordDispatch(const Graphics::Context& context, const Resolve::ResolvedDraw& dispatch, const std::array<std::uint32_t, 3>& groups, std::string& reason);
[[nodiscard]] std::string RecordLine();

}

#endif
