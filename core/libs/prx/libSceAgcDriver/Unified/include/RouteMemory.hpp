#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_ROUTEMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_ROUTEMEMORY_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Unified/include/Ranges.hpp"
#include <atomic>
#include <cstddef>
#include <string>
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

namespace AgcDriver::Unified::RouteMemory {

struct Totals {
    std::atomic<std::uint64_t> registrations{0};
    std::atomic<std::uint64_t> registeredBytes{0};
    std::atomic<std::uint64_t> outsideArena{0};
    std::atomic<std::uint64_t> accesses{0};
    std::atomic<std::uint64_t> unknownAccesses{0};
    std::atomic<std::uint64_t> proofObjects{0};
    std::atomic<std::uint64_t> proofObjectsOutside{0};
    std::atomic<std::uint64_t> consumes{0};
    std::atomic<std::uint64_t> consumesSkipped{0};
    std::atomic<std::uint64_t> ranges{0};
    std::atomic<std::uint64_t> dirtyBytes{0};
    std::atomic<std::uint64_t> consumeNanoseconds{0};
    std::atomic<std::uint64_t> maxConsumeNanoseconds{0};
    std::atomic<std::uint64_t> ownedServes{0};
    std::atomic<std::uint64_t> ownedWalks{0};
    std::atomic<std::uint64_t> ownedWalkNanoseconds{0};
    std::atomic<std::uint64_t> ownedRecords{0};
    std::atomic<std::uint64_t> ownedRecordNanoseconds{0};
    std::atomic<std::uint64_t> frames{0};
};

struct OwnedSpan {
    std::uint64_t handle = 0;
    std::uint64_t address = 0;
    std::uint64_t origin = 0;
};

using GuestRanges = std::span<const std::pair<std::uint64_t, std::uint64_t>>;

bool Active();
bool DrawScope();
bool Installed();
bool Refused();
Totals& Counts();
void RegisterUses(std::span<const GuestUse> uses);
void RegisterMapping(const GuestAllocations::Mapped& ranges);
void RegisterAccess(GuestRanges reads, GuestRanges writes, bool unknown);
bool RegisterProofRanges(GuestRanges ranges);
bool ServesOwned();
bool ServeOwned(const Graphics::Context& context, std::uint64_t address, std::uint64_t size, OwnedSpan* span);
void ServeFaultPages(std::span<const std::uint64_t> pages);
void LandGpuWrites(GuestRanges ranges);
bool ResidentDrawInputs();
void NoteDriverStore(std::uint64_t address, std::uint64_t bytes);
void NoteGpuWrites(GuestRanges ranges);
bool UploadsPending();
void ConsumeNow();
std::size_t ConsumeIn(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans);
std::size_t RecordUploads(VkCommandBuffer commands, std::vector<std::shared_ptr<void>>& kept);
void AdvanceFrame();
std::string WriteAuditLine();

}

#endif
