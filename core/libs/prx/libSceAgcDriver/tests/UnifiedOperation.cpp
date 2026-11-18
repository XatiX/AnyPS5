#include "prx/libSceAgcDriver/Unified/include/Operation.hpp"
#include "prx/libSceAgcDriver/Unified/include/SnapshotRing.hpp"
#include "prx/libSceAgcDriver/Unified/include/SpscRing.hpp"
#include <cstdint>
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

struct Bank {
    std::uint32_t words[4] = {};
};

void testSnapshotSharedBetweenWrites() {
    SnapshotRing<Bank, 8> ring;
    Bank live;
    bool dirty = true;
    SnapshotRing<Bank, 8>::View first;
    Expect(ring.TryTake(live, dirty, first), "the first snapshot is taken");
    Expect(first.id == 1 && !dirty, "the first snapshot has id 1 and clears the dirty flag");
    SnapshotRing<Bank, 8>::View second;
    Expect(ring.TryTake(live, dirty, second) && second.id == first.id, "two draws between register writes share one snapshot");
    live.words[2] = 7;
    dirty = true;
    SnapshotRing<Bank, 8>::View third;
    Expect(ring.TryTake(live, dirty, third) && third.id == 2, "a register write makes a new snapshot");
    Expect(third.value->words[2] == 7 && first.value->words[2] == 0, "each snapshot keeps the values of its own time");
    Expect(ring.Find(1) == first.value && ring.Find(2) == third.value && ring.Find(3) == nullptr, "snapshots are found by id");
}

void testSnapshotWaitsForRetire() {
    SnapshotRing<Bank, 4> ring;
    Bank live;
    SnapshotRing<Bank, 4>::View view;
    for (std::uint32_t i = 0; i < 4; ++i) {
        bool dirty = true;
        live.words[0] = i;
        Expect(ring.TryTake(live, dirty, view), "the ring holds as many snapshots as it has slots");
    }
    bool dirty = true;
    Expect(!ring.TryTake(live, dirty, view), "a full ring refuses a new snapshot until the oldest is retired");
    Expect(dirty, "a refused snapshot leaves the bank dirty");
    ring.Retire(1);
    Expect(ring.TryTake(live, dirty, view) && view.id == 5, "retiring the oldest frees its slot");
    Expect(ring.Find(1) == nullptr && ring.Find(2) != nullptr, "a reused slot no longer answers for its old id");
}

void testOperationKinds() {
    Expect(IsDraw(OpKind::Draw) && IsDraw(OpKind::DrawIndirect) && !IsDraw(OpKind::Dispatch), "draw kinds");
    Expect(IsDispatch(OpKind::Dispatch) && IsDispatch(OpKind::DispatchIndirect) && !IsDispatch(OpKind::Wait), "dispatch kinds");
    Operation operation;
    Expect(!operation.after.Present(), "an operation has no wait edge by default");
}

void testRingOrderAndCapacity() {
    SpscRing<std::uint64_t, 4> ring;
    for (std::uint64_t i = 1; i <= 4; ++i) Expect(ring.TryPush(i), "the ring takes up to its capacity");
    Expect(!ring.TryPush(5), "a full ring refuses a push");
    Expect(ring.TryPop() == 1u && ring.TryPop() == 2u, "items come out in order");
    Expect(ring.TryPush(5) && ring.TryPush(6), "popped slots are reused across the wrap");
    Expect(ring.TryPop() == 3u && ring.TryPop() == 4u && ring.TryPop() == 5u && ring.TryPop() == 6u, "order holds across the wrap");
    Expect(!ring.TryPop().has_value() && ring.Size() == 0, "an empty ring gives nothing");
}

void testRingAcrossThreads() {
    constexpr std::uint64_t count = 200000;
    SpscRing<std::uint64_t, 1024> ring;
    std::thread producer([&ring] {
        for (std::uint64_t i = 1; i <= count; ++i) {
            while (!ring.TryPush(i)) std::this_thread::yield();
        }
    });
    std::uint64_t expected = 1;
    bool ordered = true;
    while (expected <= count) {
        if (auto value = ring.TryPop()) {
            ordered = ordered && *value == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    Expect(ordered, "one producer and one consumer see every item once, in order");
}

}

int main() {
    testSnapshotSharedBetweenWrites();
    testSnapshotWaitsForRetire();
    testOperationKinds();
    testRingOrderAndCapacity();
    testRingAcrossThreads();
    if (failures != 0) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("unified operation tests passed");
    return 0;
}
