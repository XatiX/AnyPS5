#include "prx/libSceAgcDriver/Unified/include/RecordQueue.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

Loop::Item itemOf(std::uint64_t sequence, OpKind kind) {
    Loop::Item item;
    item.operation.sequence = sequence;
    item.operation.kind = kind;
    return item;
}

void testItemsComeOutInOrder() {
    Loop::RecordQueue queue;
    queue.Push(itemOf(1, OpKind::Draw));
    queue.Push(itemOf(2, OpKind::LabelStore));
    queue.Push(itemOf(3, OpKind::Wait));
    const auto first = queue.Pop(std::chrono::milliseconds(0));
    const auto second = queue.Pop(std::chrono::milliseconds(0));
    const auto third = queue.Pop(std::chrono::milliseconds(0));
    Expect(first && first->operation.sequence == 1 && first->operation.kind == OpKind::Draw, "the first pushed item comes out first");
    Expect(second && second->operation.sequence == 2 && second->operation.kind == OpKind::LabelStore, "the second pushed item comes out second");
    Expect(third && third->operation.sequence == 3 && third->operation.kind == OpKind::Wait, "the third pushed item comes out third");
    Expect(!queue.Pop(std::chrono::milliseconds(0)).has_value(), "an empty queue gives nothing");
    const auto counts = queue.Counts();
    Expect(counts.pushed == 3 && counts.popped == 3 && counts.deepest == 3, "pushes, pops and depth are counted");
}

void testPopWaitsForAPush() {
    Loop::RecordQueue queue;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue.Push(itemOf(7, OpKind::Dispatch));
    });
    const auto item = queue.Pop(std::chrono::milliseconds(2000));
    producer.join();
    Expect(item && item->operation.sequence == 7, "a pop waits until an item is pushed");
}

void testCloseEndsAWait() {
    Loop::RecordQueue queue;
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue.Close();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto item = queue.Pop(std::chrono::milliseconds(5000));
    closer.join();
    Expect(!item.has_value() && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2000), "closing the queue ends a waiting pop");
}

}

int main() {
    testItemsComeOutInOrder();
    testPopWaitsForAPush();
    testCloseEndsAWait();
    if (failures != 0) return 1;
    std::printf("agc_unified_record_queue_tests passed\n");
    return 0;
}
