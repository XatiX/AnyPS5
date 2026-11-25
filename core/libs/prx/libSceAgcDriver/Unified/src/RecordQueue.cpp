#include "prx/libSceAgcDriver/Unified/include/RecordQueue.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <string_view>

namespace AgcDriver::Unified::Loop {

void RecordQueue::Push(Item item) {
    {
        std::lock_guard lock(mutex);
        items.push_back(std::move(item));
        counts.pushed++;
        counts.deepest = std::max(counts.deepest, items.size());
    }
    ready.notify_one();
}

std::optional<Item> RecordQueue::Pop(std::chrono::milliseconds wait) {
    std::unique_lock lock(mutex);
    if (items.empty() && !closed) {
        counts.waited++;
        ready.wait_for(lock, wait, [&] { return !items.empty() || closed; });
    }
    if (items.empty()) return std::nullopt;
    auto item = std::move(items.front());
    items.pop_front();
    counts.popped++;
    return item;
}

void RecordQueue::Close() {
    {
        std::lock_guard lock(mutex);
        closed = true;
    }
    ready.notify_all();
}

std::size_t RecordQueue::Size() const {
    std::lock_guard lock(mutex);
    return items.size();
}

std::uint64_t RecordQueue::Popped() const {
    std::lock_guard lock(mutex);
    return counts.popped;
}

QueueCounts RecordQueue::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

RecordQueue& QueueFor(std::uint32_t queue) {
    static std::mutex queuesMutex;
    static std::map<std::uint32_t, std::unique_ptr<RecordQueue>> queues;
    std::lock_guard lock(queuesMutex);
    auto& held = queues[queue];
    if (held == nullptr) held = std::make_unique<RecordQueue>();
    return *held;
}

bool OwnsLoop() {
    static const bool owns = [] {
        const char* value = std::getenv("APS5_RECODE_OWN_LOOP");
        if (value != nullptr) return std::string_view(value) != "0";
        const char* route = std::getenv("APS5_RECODE_DRAW");
        return route != nullptr && std::string_view(route) == "record";
    }();
    return owns;
}

}
