#include "prx/libSceAgcDriver/Unified/include/PacketTable.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testOfferedValueStaysUntilReleased() {
    PacketTable<int> table;
    table.Offer(0, 7, 0x40, std::make_shared<const int>(5));
    const auto first = table.Find(0, 7, 0x40, std::chrono::milliseconds(0));
    Expect(first.offered && first.value != nullptr && *first.value == 5, "an offered value is found");
    const auto again = table.Find(0, 7, 0x40, std::chrono::milliseconds(0));
    Expect(again.offered && again.value == first.value, "a second find of the same packet sees the same value");
    table.Release(0, 7, 0x40);
    const auto gone = table.Find(0, 7, 0x40, std::chrono::milliseconds(0));
    Expect(!gone.offered, "a released packet is no longer found");
    const auto counts = table.TakeCounts();
    Expect(counts.found == 2 && counts.released == 1 && counts.missed == 1 && counts.pending == 0, "finds, releases and misses are counted");
}

void testRefusalIsOfferedWithoutAValue() {
    PacketTable<int> table;
    table.Offer(1, 3, 0x10, nullptr);
    const auto found = table.Find(1, 3, 0x10, std::chrono::milliseconds(1000));
    Expect(found.offered && found.value == nullptr, "a refused packet is found at once without a value");
    const auto counts = table.TakeCounts();
    Expect(counts.refused == 1 && counts.refusedOffers == 1 && counts.waited == 0, "a refusal is counted and needs no wait");
}

void testFindWaitsForALateOffer() {
    PacketTable<int> table;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        table.Offer(0, 9, 0x80, std::make_shared<const int>(11));
    });
    const auto found = table.Find(0, 9, 0x80, std::chrono::milliseconds(5000));
    producer.join();
    Expect(found.offered && found.value != nullptr && *found.value == 11, "a find waits for an offer that comes later");
    Expect(table.TakeCounts().waited == 1, "the late offer counts one wait");
}

void testKeysSeparateQueuesSerialsAndOffsets() {
    PacketTable<int> table;
    table.Offer(0, 1, 0x20, std::make_shared<const int>(1));
    table.Offer(1, 1, 0x20, std::make_shared<const int>(2));
    table.Offer(0, 2, 0x20, std::make_shared<const int>(3));
    table.Offer(0, 1, 0x24, std::make_shared<const int>(4));
    const auto value = [&](std::uint32_t queue, std::uint64_t serial, std::size_t offset) {
        const auto found = table.Find(queue, serial, offset, std::chrono::milliseconds(0));
        return found.value != nullptr ? *found.value : -1;
    };
    Expect(value(0, 1, 0x20) == 1 && value(1, 1, 0x20) == 2 && value(0, 2, 0x20) == 3 && value(0, 1, 0x24) == 4, "queue, serial and offset each select their own packet");
}

void testOverflowDropsPending() {
    PacketTable<int> table;
    for (std::size_t offset = 0; offset < PacketTable<int>::MaxPending; ++offset) table.Offer(0, 1, offset * 4u, nullptr);
    table.Offer(0, 2, 0, std::make_shared<const int>(9));
    const auto counts = table.TakeCounts();
    Expect(counts.dropped == PacketTable<int>::MaxPending && counts.pending == 1, "a full table drops its pending packets and keeps the new one");
}

}

int main() {
    testOfferedValueStaysUntilReleased();
    testRefusalIsOfferedWithoutAValue();
    testFindWaitsForALateOffer();
    testKeysSeparateQueuesSerialsAndOffsets();
    testOverflowDropsPending();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified packet table: offer, find, release and wait checks passed\n");
    return 0;
}
