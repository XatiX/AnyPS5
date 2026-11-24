#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDLOOP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDLOOP_HPP

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Unified::Loop {

using InterruptHook = void (*)(void* owner, std::uint32_t queue, std::uint32_t context);

struct LoopCounts {
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t refused = 0;
    std::uint64_t labels = 0;
    std::uint64_t labelsAtOnce = 0;
    std::uint64_t interrupts = 0;
    std::uint64_t waits = 0;
    std::uint64_t waitsFromTable = 0;
    std::uint64_t waitsFromGuest = 0;
    std::uint64_t waitPolls = 0;
    std::uint64_t copies = 0;
    std::uint64_t copiesOnCpu = 0;
    std::uint64_t flips = 0;
};

void SetInterruptHook(InterruptHook hook, void* owner);
[[nodiscard]] bool Takes(std::span<const std::uint32_t> packet, std::uint32_t opcode, std::uint32_t queue);
void TraceKept(std::span<const std::uint32_t> packet, std::uint32_t opcode, std::uint32_t queue, std::uint64_t submission, std::size_t offset, bool flip);
void TraceInterrupt(std::uint32_t queue, std::uint32_t context);
void NoteRefreshedSegment(std::uint32_t queue, std::uint64_t submission, std::size_t offset);
void Ensure(std::uint32_t queue);
[[nodiscard]] bool Passed(std::uint32_t queue, std::uint64_t submission, std::uint32_t packetOffset);
bool WaitPassed(std::uint32_t queue, std::uint64_t submission, std::uint32_t packetOffset, std::chrono::milliseconds limit);
[[nodiscard]] LoopCounts Counts();
[[nodiscard]] std::string Line();

}

#endif
