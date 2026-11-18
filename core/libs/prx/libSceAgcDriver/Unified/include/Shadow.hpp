#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_SHADOW_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace AgcDriver::Unified::Shadow {

using GuestRanges = std::span<const std::pair<std::uint64_t, std::uint64_t>>;

bool Active();
bool MemoryActive();
bool DrawScope();
bool Teeing();
std::shared_ptr<QueueState> RecordedState(const DriverDetail::Submission& submission, std::size_t packetOffset);
void NoteDriverDraw(const QueueState& queue, const DriverDetail::Submission& submission, std::size_t packetOffset);
void NoteGpuMapping(const GuestAllocations::Mapped& ranges);
void NoteGuestAccess(GuestRanges reads, GuestRanges writes, bool unknown);
std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission);
void Tee(const DriverDetail::Submission& submission);

}

#endif
