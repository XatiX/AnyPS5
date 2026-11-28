#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_INTAKE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_INTAKE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
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

namespace AgcDriver::Unified::Intake {

bool Active();
bool Takes();
Front& Lanes();
std::shared_ptr<QueueState> RecordedState(const DriverDetail::Submission& submission, std::size_t packetOffset);
void AttachDevice(const std::shared_ptr<VulkanDevice>& device);
void NoteDriverDraw(const QueueState& queue, const DriverDetail::Submission& submission, std::size_t packetOffset);
std::shared_ptr<DriverDetail::Submission> Copy(const DriverDetail::Submission& submission);
void Submit(const DriverDetail::Submission& submission);

}

#endif
