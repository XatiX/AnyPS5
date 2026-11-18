#include "prx/libSceAgcDriver/Unified/include/Ranges.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <algorithm>

namespace AgcDriver::Unified {

namespace {

void add(std::vector<GuestUse>& uses, std::uint64_t address, std::uint64_t size, bool write) {
    if (address == 0 || size == 0) return;
    uses.push_back({address, size, write});
}

}

void DrawUses(const Pm4::DrawParameters& draw, std::vector<GuestUse>& uses) {
    if (draw.indexed) add(uses, draw.indexAddress, static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize, false);
    if (!draw.indirect) return;
    add(uses, draw.indirect->arguments, draw.indirect->RangeBytes(), false);
    if (draw.indirect->countIndirect) add(uses, draw.indirect->countAddress, sizeof(std::uint32_t), false);
}

void TargetUses(const QueueState& queue, std::vector<GuestUse>& uses) {
    const auto state = Graphics::DecodeState(queue);
    for (const auto& color : state.colors) {
        add(uses, color.address, color.bytes, true);
        add(uses, color.dccAddress, color.bytes / 256 + 1, true);
        add(uses, color.cmaskAddress, color.cmaskBytes, true);
    }
    if (!state.depth) return;
    const auto& depth = *state.depth;
    const auto slices = static_cast<std::uint64_t>(depth.lastSlice >= depth.firstSlice ? depth.lastSlice - depth.firstSlice + 1 : 1);
    const auto samples = static_cast<std::uint64_t>(std::max<std::uint32_t>(depth.samples, 1));
    const auto plane = Graphics::DepthSliceBytes(depth.extent, 4) * slices * samples;
    add(uses, depth.address, plane, true);
    add(uses, depth.stencilAddress, plane / 4 + 1, true);
    add(uses, depth.htileAddress, plane / 64 + 1, true);
}

}
