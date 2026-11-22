#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESOLVE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESOLVE_HPP

#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawBindings.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Unified/include/PacketTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Programs.hpp"
#include "prx/libSceAgcDriver/Unified/include/Ranges.hpp"
#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Graphics {
class StorageTexture;
}

namespace AgcDriver::Unified::Heap {
class VulkanHeapDevice;
}

namespace AgcDriver::Unified::Tables {
struct Mirror;
}

namespace AgcDriver::Unified::Images {
class SurfaceTable;
struct Surface;
}

namespace AgcDriver::Unified::Resolve {

struct ResolvedStage {
    StageProgram program;
    std::vector<std::uint32_t> slots;
    std::shared_ptr<const Tables::Mirror> tableMap;
    std::vector<std::shared_ptr<const void>> owners;
    std::vector<Heap::DeferredImage> deferredImages;
};

struct ResolvedDraw {
    Operation operation;
    std::vector<ResolvedStage> stages;
    std::vector<GuestUse> uses;
    bool complete = true;
    std::size_t targetBegin = 0;
    std::size_t targetEnd = 0;
    std::size_t imageBegin = 0;
    Bindings::Layout layout;
    std::shared_ptr<const Graphics::State> state;
};

[[nodiscard]] bool Active();
[[nodiscard]] bool DispatchesActive();
[[nodiscard]] bool DispatchesRoutedOn(std::uint32_t queue);
void Attach(const std::shared_ptr<VulkanDevice>& device);
void Consume(const Operation& operation, const DecodeState& state);
[[nodiscard]] std::string Line();
[[nodiscard]] std::string TakeRefusal();
[[nodiscard]] std::chrono::milliseconds HandoffWait();
[[nodiscard]] PacketTable<ResolvedDraw>& ResolvedDraws();
[[nodiscard]] PacketTable<ResolvedDraw>& ResolvedDispatches();
[[nodiscard]] const Heap::VulkanHeapDevice* HeapDevice();
[[nodiscard]] Images::SurfaceTable* Surfaces();
[[nodiscard]] std::shared_ptr<Graphics::StorageTexture> BindDeferredImage(const Graphics::Context& context, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason);
[[nodiscard]] std::shared_ptr<Images::Surface> BindDeferredRecord(const Heap::SlotRequest& request, std::uint32_t& slot);
[[nodiscard]] std::shared_ptr<Images::Surface> BindRecordSlot(const Heap::SlotRequest& request, std::shared_ptr<Images::Surface> record, std::uint32_t& slot);
[[nodiscard]] bool StorageRecordShape(const Heap::SlotRequest& request);
[[nodiscard]] bool SampledRecordsAll();
[[nodiscard]] bool BindViewSlot(const Heap::SlotRequest& request, std::uint64_t view, std::uint32_t layout, std::shared_ptr<const void> owner, std::uint32_t& slot);
[[nodiscard]] std::shared_ptr<const void> BindDeferredSampled(const Graphics::Context& context, const Heap::SlotRequest& request, std::uint32_t& slot, std::string& reason);

}

#endif
