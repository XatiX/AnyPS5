#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDDRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDDRAW_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace AgcDriver::Unified::Record {

struct HandoffCounts {
    std::uint64_t offered = 0;
    std::uint64_t taken = 0;
    std::uint64_t waited = 0;
    std::uint64_t missed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t pending = 0;
};

class Handoff {
public:
    static constexpr std::size_t MaxPending = 1u << 14u;

    void Offer(const Operation& operation, const DecodeState& state);
    std::shared_ptr<QueueState> Take(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset, std::chrono::milliseconds wait);
    [[nodiscard]] HandoffCounts Counts() const;

private:
    static std::uint64_t slotOf(std::uint32_t queueId, std::uint64_t serial, std::size_t packetOffset);

    mutable std::mutex mutex;
    std::condition_variable offered;
    std::unordered_map<std::uint64_t, std::shared_ptr<QueueState>> ready;
    HandoffCounts counts;
};

bool DrawActive();
Handoff& SharedHandoff();
std::string HandoffLine();

}

#endif
