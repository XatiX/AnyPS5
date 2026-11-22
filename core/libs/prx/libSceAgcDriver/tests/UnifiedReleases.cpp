#include "prx/libSceAgcDriver/Unified/include/Releases.hpp"
#include <atomic>
#include <cstdio>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testReleasesRunInOrderOffTheQueuingThread() {
    const auto before = Releases::Totals();
    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> gate;
    auto opened = gate.get_future().share();
    std::atomic<bool> returned{false};
    std::atomic<bool> ranAfterReturn{false};
    std::thread::id releaser;
    Releases::Queue([&, opened] {
        opened.wait();
        ranAfterReturn.store(returned.load());
        releaser = std::this_thread::get_id();
        std::lock_guard lock(mutex);
        order.push_back(1);
    });
    for (int value = 2; value <= 4; ++value) {
        Releases::Queue([&, value] {
            std::lock_guard lock(mutex);
            order.push_back(value);
        });
    }
    returned.store(true);
    {
        std::lock_guard lock(mutex);
        Expect(order.empty(), "nothing is released while the first release is held");
    }
    gate.set_value();
    Releases::Drain();
    std::lock_guard lock(mutex);
    Expect(order == std::vector<int>({1, 2, 3, 4}), "releases run in the order they were queued");
    Expect(ranAfterReturn.load() && releaser != std::this_thread::get_id(), "a release runs on another thread, after the queuing call returned");
    const auto after = Releases::Totals();
    Expect(after.queued == before.queued + 4 && after.released == before.released + 4, "every queued release is counted once it ran");
}

void testAReleaseThatThrowsDoesNotStopTheNext() {
    std::atomic<int> ran{0};
    Releases::Queue([] { throw std::runtime_error("a release that fails"); });
    Releases::Queue([&] { ran.fetch_add(1); });
    Releases::Drain();
    Expect(ran.load() == 1, "the release after a failed one still runs");
}

void testCountsOfDirectReleasesAndLabels() {
    const auto before = Releases::Totals();
    Releases::NoteDirect();
    Releases::NoteLabelBehindLandings();
    Releases::NoteLabelBehindLandings();
    const auto after = Releases::Totals();
    Expect(after.direct == before.direct + 1 && after.labelsBehindLandings == before.labelsBehindLandings + 2, "direct releases and labels behind landings are counted");
    Expect(Releases::Line().find("flips released after their batch") != std::string::npos, "the status line names the flips released after their batch");
}

}

int main() {
    testReleasesRunInOrderOffTheQueuingThread();
    testAReleaseThatThrowsDoesNotStopTheNext();
    testCountsOfDirectReleasesAndLabels();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified release tests passed\n");
    return 0;
}
