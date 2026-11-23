#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace AgcDriver::Graphics {
struct Context;
}

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Unified::Shadow {

struct OwnedSpan {
    std::uint64_t handle = 0;
    std::uint64_t address = 0;
    std::uint64_t origin = 0;
};

using GuestRanges = std::span<const std::pair<std::uint64_t, std::uint64_t>>;

bool Active();
bool MemoryActive();
bool DrawScope();
bool Teeing();
std::shared_ptr<QueueState> RecordedState(const DriverDetail::Submission& submission, std::size_t packetOffset);
void NoteDevice(const std::shared_ptr<VulkanDevice>& device);
void NoteDriverDraw(const QueueState& queue, const DriverDetail::Submission& submission, std::size_t packetOffset);
void NoteGpuMapping(const GuestAllocations::Mapped& ranges);
void NoteGuestAccess(GuestRanges reads, GuestRanges writes, bool unknown);
bool NoteProofRanges(GuestRanges ranges);
std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission);
void Tee(const DriverDetail::Submission& submission);
bool ServesOwned();
bool ServeOwned(const Graphics::Context& context, std::uint64_t address, std::uint64_t size, OwnedSpan* span);
void ServeFaultPages(std::span<const std::uint64_t> pages);
void LandGpuWrites(GuestRanges ranges);
bool ResidentDrawInputs();
void NoteGpuWrites(GuestRanges ranges);
bool OwnedUploadsPending();
std::size_t RecordOwnedUploads(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept);

}

#endif
