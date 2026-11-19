#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_DRAWRECORDER_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Unified/include/Resolve.hpp"
#include <string>

namespace AgcDriver::Unified::Record {

[[nodiscard]] bool RecordActive();
bool RecordPrecheck(const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads, std::string& reason);
bool RecordResolved(const Graphics::Context& context, const Resolve::ResolvedDraw& draw, const Pm4::DrawParameters& parameters, std::string& reason);
[[nodiscard]] std::string RecordLine();

}

#endif
