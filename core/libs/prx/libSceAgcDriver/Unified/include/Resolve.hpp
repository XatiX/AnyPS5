#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESOLVE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESOLVE_HPP

#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawBindings.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Unified/include/PacketTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Programs.hpp"
#include "prx/libSceAgcDriver/Unified/include/Ranges.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Unified::Heap {
class VulkanHeapDevice;
}

namespace AgcDriver::Unified::Resolve {

struct ResolvedStage {
    StageProgram program;
    std::vector<std::uint32_t> slots;
};

struct ResolvedDraw {
    Operation operation;
    std::vector<ResolvedStage> stages;
    std::vector<GuestUse> uses;
    bool complete = true;
    Bindings::Layout layout;
    std::shared_ptr<const Graphics::State> state;
};

[[nodiscard]] bool Active();
void Attach(const std::shared_ptr<VulkanDevice>& device);
void Consume(const Operation& operation, const DecodeState& state);
[[nodiscard]] std::string Line();
[[nodiscard]] PacketTable<ResolvedDraw>& ResolvedDraws();
[[nodiscard]] const Heap::VulkanHeapDevice* HeapDevice();

}

#endif
