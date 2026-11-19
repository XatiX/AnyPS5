#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RANGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RANGES_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <cstdint>
#include <vector>

namespace AgcDriver::Graphics {
struct State;
}

namespace AgcDriver::Unified {

struct GuestUse {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
    bool write = false;

    bool operator==(const GuestUse&) const = default;
};

void DrawUses(const Pm4::DrawParameters& draw, std::vector<GuestUse>& uses);
void TargetUses(const QueueState& queue, std::vector<GuestUse>& uses);
void TargetUses(const Graphics::State& state, std::vector<GuestUse>& uses);

}

#endif
