#include "prx/libSceAgcDriver/Unified/include/Releases.hpp"
#include "prx/libSceAgcDriver/Unified/include/DrawRecorder.hpp"
#include "prx/libSceAgcDriver/Unified/include/MirrorHost.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>

namespace AgcDriver::Unified::Releases {

namespace {

struct State {
    std::mutex mutex;
    std::condition_variable signal;
    std::condition_variable idle;
    std::deque<std::function<void()>> pending;
    bool started = false;
    bool busy = false;
    std::atomic<std::uint64_t> queued{0};
    std::atomic<std::uint64_t> released{0};
    std::atomic<std::uint64_t> direct{0};
    std::atomic<std::uint64_t> labelsBehindLandings{0};
};

State& state() {
    static auto& shared = *new State();
    return shared;
}

void run() {
    auto& shared = state();
    std::unique_lock lock(shared.mutex);
    for (;;) {
        shared.signal.wait(lock, [&] { return !shared.pending.empty(); });
        auto release = std::move(shared.pending.front());
        shared.pending.pop_front();
        shared.busy = true;
        lock.unlock();
        try {
            release();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[recode-release] a release after its batch failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "[recode-release] a release after its batch failed\n");
        }
        shared.released.fetch_add(1, std::memory_order_relaxed);
        lock.lock();
        shared.busy = false;
        if (shared.pending.empty()) shared.idle.notify_all();
    }
}

}

bool FlipsFollowWork() {
    static const bool follows = [] {
        const char* value = std::getenv("APS5_RECODE_FLIP_RELEASE");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return follows && Mirror::WritesEnabled() && Record::RecordActive();
}

void Queue(std::function<void()> release) {
    auto& shared = state();
    {
        std::lock_guard lock(shared.mutex);
        if (!shared.started) {
            shared.started = true;
            std::thread(run).detach();
        }
        shared.pending.push_back(std::move(release));
    }
    shared.queued.fetch_add(1, std::memory_order_relaxed);
    shared.signal.notify_one();
}

void NoteDirect() {
    state().direct.fetch_add(1, std::memory_order_relaxed);
}

void NoteLabelBehindLandings() {
    state().labelsBehindLandings.fetch_add(1, std::memory_order_relaxed);
}

void Drain() {
    auto& shared = state();
    std::unique_lock lock(shared.mutex);
    shared.idle.wait(lock, [&] { return shared.pending.empty() && !shared.busy; });
}

Counts Totals() {
    const auto& shared = state();
    return {shared.queued.load(std::memory_order_relaxed), shared.released.load(std::memory_order_relaxed), shared.direct.load(std::memory_order_relaxed), shared.labelsBehindLandings.load(std::memory_order_relaxed)};
}

std::string Line() {
    const auto counts = Totals();
    return "[recode-release] flips released after their batch " + std::to_string(counts.released) + " of " + std::to_string(counts.queued) + " queued, " + std::to_string(counts.direct) + " released at once (nothing recorded was in flight); labels stored behind a pending landing " + std::to_string(counts.labelsBehindLandings);
}

}
