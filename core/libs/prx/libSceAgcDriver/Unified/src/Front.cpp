#include "prx/libSceAgcDriver/Unified/include/Front.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <chrono>

namespace AgcDriver::Unified {

struct Front::Lane {
    explicit Lane(std::uint32_t queue) : state(queue) {}

    class Forward : public Sink {
    public:
        Forward(Front& front, Lane& lane) : front(front), lane(lane) {}
        void Emit(const Operation& operation) override {
            lane.state.RetireBefore(operation.snapshots);
            if (operation.kind == OpKind::Wait) {
                Operation wait = operation;
                static_cast<void>(front.order.Resolve(wait));
                deliver(wait);
                return;
            }
            static_cast<void>(front.order.Note(operation));
            deliver(operation);
        }
        void MakeRoom() override { std::this_thread::yield(); }

    private:
        void deliver(const Operation& operation) {
            if (front.consumer) front.consumer(operation, lane.state);
        }
        Front& front;
        Lane& lane;
    };

    DecodeState state;
    SpscRing<Input, LaneDepth> input;
    std::atomic<std::uint32_t> wake{0};
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> decoded{0};
    std::atomic<std::uint64_t> decodeNanoseconds{0};
    std::atomic<bool> stop{false};
    std::mutex publishedMutex;
    LaneTotals published;
    std::thread thread;
};

Front::Front(Consumer consumer, bool resolve, UseSink uses) : consumer(std::move(consumer)), resolve(resolve), uses(std::move(uses)) {}

Front::~Front() {
    std::lock_guard lock(lanesMutex);
    for (auto& [queue, lane] : lanes) {
        lane->stop.store(true, std::memory_order_release);
        lane->wake.fetch_add(1, std::memory_order_release);
        lane->wake.notify_one();
    }
    for (auto& [queue, lane] : lanes) {
        if (lane->thread.joinable()) lane->thread.join();
    }
}

Front::Lane& Front::laneFor(std::uint32_t queue) {
    std::lock_guard lock(lanesMutex);
    auto& lane = lanes[queue];
    if (lane == nullptr) {
        lane = std::make_unique<Lane>(queue);
        lane->state.SetResolve(resolve);
        lane->state.SetUses(uses);
        Lane& created = *lane;
        created.thread = std::thread([this, &created, queue] {
            StartLaneSampler(queue);
            run(created);
        });
    }
    return *lane;
}

void Front::Push(std::shared_ptr<const DriverDetail::Submission> submission) {
    Lane& lane = laneFor(submission->queue);
    const bool reset = submission->queue == 0 && graphicsReset.exchange(false, std::memory_order_relaxed);
    Input item{std::move(submission), reset};
    while (!lane.input.TryPush(item)) std::this_thread::yield();
    lane.pushed.fetch_add(1, std::memory_order_release);
    lane.wake.fetch_add(1, std::memory_order_release);
    lane.wake.notify_one();
}

void Front::run(Lane& lane) {
    Lane::Forward sink(*this, lane);
    for (;;) {
        const auto seen = lane.wake.load(std::memory_order_acquire);
        auto next = lane.input.TryPop();
        if (!next.has_value()) {
            if (lane.stop.load(std::memory_order_acquire)) return;
            lane.wake.wait(seen, std::memory_order_acquire);
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        if (next->reset) lane.state.ResetQueueState();
        DecodeSubmission(*next->submission, lane.state, sink);
        const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        lane.decodeNanoseconds.fetch_add(static_cast<std::uint64_t>(spent), std::memory_order_relaxed);
        next->submission.reset();
        {
            std::lock_guard lock(lane.publishedMutex);
            lane.published.queue = lane.state.Queue();
            ++lane.published.submissions;
            lane.published.decodeNanoseconds += static_cast<std::uint64_t>(spent);
            lane.published.counts = lane.state.Counts();
        }
        lane.decoded.fetch_add(1, std::memory_order_release);
        lane.decoded.notify_all();
    }
}

void Front::Flush() {
    std::vector<Lane*> current;
    {
        std::lock_guard lock(lanesMutex);
        for (auto& [queue, lane] : lanes) current.push_back(lane.get());
    }
    for (Lane* lane : current) {
        const auto target = lane->pushed.load(std::memory_order_acquire);
        for (auto done = lane->decoded.load(std::memory_order_acquire); done < target; done = lane->decoded.load(std::memory_order_acquire)) lane->decoded.wait(done, std::memory_order_acquire);
    }
}

std::vector<LaneTotals> Front::Totals(bool flush) {
    if (flush) Flush();
    std::vector<LaneTotals> totals;
    std::lock_guard lock(lanesMutex);
    for (auto& [queue, lane] : lanes) {
        std::lock_guard publishedLock(lane->publishedMutex);
        totals.push_back(lane->published);
        totals.back().queue = queue;
    }
    return totals;
}

}
