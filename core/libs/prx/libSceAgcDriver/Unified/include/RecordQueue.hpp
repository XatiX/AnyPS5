#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDQUEUE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RECORDQUEUE_HPP

#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace AgcDriver::DriverDetail {
struct Submission;
}

namespace AgcDriver::Unified::Resolve {
struct ResolvedDraw;
}

namespace AgcDriver::Unified::Loop {

struct Item {
    Operation operation;
    std::shared_ptr<const Resolve::ResolvedDraw> resolved;
    std::shared_ptr<const DriverDetail::Submission> submission;
    std::optional<Pm4::DrawParameters> parameters;
    std::array<std::uint32_t, 3> groups{};
    std::vector<std::uint32_t> words;
};

struct QueueCounts {
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t waited = 0;
    std::size_t deepest = 0;
};

class RecordQueue {
public:
    void Push(Item item);
    [[nodiscard]] std::optional<Item> Pop(std::chrono::milliseconds wait);
    void Close();
    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] std::uint64_t Popped() const;
    [[nodiscard]] QueueCounts Counts() const;

private:
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Item> items;
    QueueCounts counts;
    bool closed = false;
};

[[nodiscard]] bool OwnsLoop();
[[nodiscard]] RecordQueue& QueueFor(std::uint32_t queue);

}

#endif
