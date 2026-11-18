#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_FRONT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_FRONT_HPP

#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Unified/include/Order.hpp"
#include "prx/libSceAgcDriver/Unified/include/SpscRing.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace AgcDriver::Unified {

struct LaneTotals {
    std::uint32_t queue = 0;
    std::uint64_t submissions = 0;
    std::uint64_t decodeNanoseconds = 0;
    DecodeCounts counts;

    [[nodiscard]] double NanosecondsPerOperation() const { return counts.operations == 0 ? 0.0 : static_cast<double>(decodeNanoseconds) / static_cast<double>(counts.operations); }
};

class Front {
public:
    using Consumer = std::function<void(const Operation&)>;

    explicit Front(Consumer consumer = {}, bool resolve = false, UseSink uses = {});
    ~Front();
    Front(const Front&) = delete;
    Front& operator=(const Front&) = delete;

    void Push(std::shared_ptr<const DriverDetail::Submission> submission);
    void Flush();
    [[nodiscard]] std::vector<LaneTotals> Totals(bool flush = true);
    [[nodiscard]] Order& Ordering() { return order; }

private:
    static constexpr std::size_t LaneDepth = 256;

    struct Lane;
    Lane& laneFor(std::uint32_t queue);
    void run(Lane& lane);

    Consumer consumer;
    bool resolve;
    UseSink uses;
    Order order;
    std::mutex lanesMutex;
    std::map<std::uint32_t, std::unique_ptr<Lane>> lanes;
};

}

#endif
