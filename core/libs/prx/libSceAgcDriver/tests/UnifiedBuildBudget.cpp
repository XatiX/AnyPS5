#include "prx/libSceAgcDriver/Unified/include/BuildBudget.hpp"
#include <cstdint>
#include <cstdio>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

constexpr std::uint64_t GiB = 1ull << 30;
constexpr std::uint64_t MiB = 1ull << 20;

void testThreadsFollowTheProcessorCount() {
    Expect(BuildBudget::Threads({1, 0, 0}) == 1, "one processor builds on one thread");
    Expect(BuildBudget::Threads({4, 0, 0}) == 3, "four processors keep one free");
    Expect(BuildBudget::Threads({16, 0, 0}) == 15, "sixteen processors keep one free");
    Expect(BuildBudget::Threads({64, 0, 0}) == 63, "the thread count has no fixed cap");
    Expect(BuildBudget::Threads({16, 0, 6}) == 6, "a configured count replaces the processor rule");
}

void testAWaitingFrameTakesEveryThread() {
    Expect(BuildBudget::Share({16, 0, 0}, false) == 4, "a running frame leaves builds a quarter of the processors");
    Expect(BuildBudget::Share({16, 0, 0}, true) == 15, "a waiting frame gives builds every thread");
    Expect(BuildBudget::Share({2, 0, 0}, false) == 1 && BuildBudget::Share({2, 0, 0}, true) == 1, "two processors build one at a time");
    Expect(BuildBudget::Share({6, 0, 0}, false) == 1 && BuildBudget::Share({6, 0, 0}, true) == 5, "six processors: one beside a running frame, five for a waiting one");
    Expect(BuildBudget::Share({16, 0, 3}, false) == 3 && BuildBudget::Share({16, 0, 3}, true) == 3, "a configured count applies to both");
}

void testAdmissionFollowsTheShare() {
    const BuildBudget::Machine host{16, 64 * GiB, 0};
    Expect(BuildBudget::Admits(host, false, {3, 3 * GiB}, 2 * MiB), "a fourth build starts beside a running frame");
    Expect(!BuildBudget::Admits(host, false, {4, 4 * GiB}, 2 * MiB), "a fifth build waits beside a running frame");
    Expect(BuildBudget::Admits(host, true, {4, 4 * GiB}, 2 * MiB), "a fifth build starts for a waiting frame");
    Expect(!BuildBudget::Admits(host, true, {15, 15 * GiB}, 2 * MiB), "no build starts past the thread count");
}

void testAdmissionFollowsFreeMemory() {
    Expect(BuildBudget::MemoryBytes(2 * MiB) == 928 * MiB, "a 2 MiB module is budgeted at the measured 0.9 GiB");
    const BuildBudget::Machine small{16, 4 * GiB, 0};
    Expect(BuildBudget::Admits(small, true, {0, 0}, 13 * MiB), "the first build always starts");
    Expect(!BuildBudget::Admits(small, true, {1, BuildBudget::MemoryBytes(13 * MiB)}, 13 * MiB), "a second 13 MiB module waits on 4 GiB free");
    Expect(BuildBudget::Admits(small, true, {1, BuildBudget::MemoryBytes(34 << 10)}, 34 << 10), "small modules build side by side on 4 GiB free");
    const BuildBudget::Machine large{16, 21 * GiB, 0};
    Expect(BuildBudget::Admits(large, true, {8, 8 * BuildBudget::MemoryBytes(2 * MiB)}, 2 * MiB), "nine 2 MiB modules fit in 21 GiB free");
    Expect(BuildBudget::Admits({16, 0, 0}, true, {8, 64 * GiB}, 2 * MiB), "an unknown amount of free memory does not hold builds");
}

void testTheHostIsUsable() {
    const auto host = BuildBudget::Host();
    Expect(host.processors >= 1, "the host reports at least one processor");
    Expect(BuildBudget::Threads(host) >= 1 && BuildBudget::Share(host, false) >= 1 && BuildBudget::Share(host, true) >= BuildBudget::Share(host, false), "the host's shares are ordered");
}

}

int main() {
    testThreadsFollowTheProcessorCount();
    testAWaitingFrameTakesEveryThread();
    testAdmissionFollowsTheShare();
    testAdmissionFollowsFreeMemory();
    testTheHostIsUsable();
    if (failures != 0) return 1;
    std::puts("unified build budget tests passed");
    return 0;
}
