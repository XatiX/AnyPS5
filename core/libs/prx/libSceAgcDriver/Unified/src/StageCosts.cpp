#include "prx/libSceAgcDriver/Unified/include/StageCosts.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace AgcDriver::Unified::StageCosts {

namespace {

constexpr std::size_t Stages = static_cast<std::size_t>(Stage::Count);

struct Ledger {
    std::array<std::atomic<std::uint64_t>, Stages> nanoseconds{};
    std::array<std::atomic<std::uint64_t>, Stages> operations{};
    std::mutex mutex;
    std::array<std::uint64_t, Stages> reportedNanoseconds{};
    std::array<std::uint64_t, Stages> reportedOperations{};
    std::uint64_t reportedFrame = 0;
};

Ledger& ledger() {
    static auto& shared = *new Ledger();
    return shared;
}

}

void Note(Stage stage, std::uint64_t nanoseconds, std::uint64_t operations) {
    auto& costs = ledger();
    const auto index = static_cast<std::size_t>(stage);
    costs.nanoseconds[index].fetch_add(nanoseconds, std::memory_order_relaxed);
    costs.operations[index].fetch_add(operations, std::memory_order_relaxed);
}

std::string Line(std::uint64_t frame) {
    static constexpr std::array<const char*, Stages> names{"resolve", "order wait", "record", "host copy", "device drain"};
    auto& costs = ledger();
    std::lock_guard lock(costs.mutex);
    const auto frames = frame > costs.reportedFrame ? frame - costs.reportedFrame : 0u;
    std::string line = "[recode-stages] per frame";
    char text[96];
    for (std::size_t index = 0; index < Stages; ++index) {
        const auto nanoseconds = costs.nanoseconds[index].load(std::memory_order_relaxed);
        const auto operations = costs.operations[index].load(std::memory_order_relaxed);
        const auto spent = nanoseconds - costs.reportedNanoseconds[index];
        const auto count = operations - costs.reportedOperations[index];
        costs.reportedNanoseconds[index] = nanoseconds;
        costs.reportedOperations[index] = operations;
        const double perFrame = frames == 0u ? 0.0 : static_cast<double>(spent) / 1e6 / static_cast<double>(frames);
        const double perOperation = count == 0u ? 0.0 : static_cast<double>(spent) / 1e3 / static_cast<double>(count);
        std::snprintf(text, sizeof(text), "%s %s %.2f ms (%.1f us x %.0f)", index == 0 ? ":" : ",", names[index], perFrame, perOperation, frames == 0u ? 0.0 : static_cast<double>(count) / static_cast<double>(frames));
        line += text;
    }
    std::snprintf(text, sizeof(text), "; %llu frames", static_cast<unsigned long long>(frames));
    line += text;
    costs.reportedFrame = frame;
    return line;
}

}
