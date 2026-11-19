#include "prx/libSceAgcDriver/Unified/include/DataRing.hpp"
#include <cstdio>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testAllocationsAreAlignedAndMergedPerSerial() {
    RingSpace ring(1024, 256);
    const auto first = ring.Allocate(100, 1, 0);
    const auto second = ring.Allocate(10, 1, 0);
    Expect(first == 0u && second == 256u, "allocations start at aligned offsets");
    Expect(ring.Used() == 266u, "one serial's allocations share a region");
    const auto counts = ring.TakeCounts();
    Expect(counts.allocations == 2 && counts.bytes == 110 && counts.full == 0, "allocations and bytes are counted");
}

void testFullRingRefusesUntilTheBatchCompletes() {
    RingSpace ring(1024, 256);
    Expect(ring.Allocate(512, 1, 0) == 0u, "first half");
    Expect(ring.Allocate(512, 2, 0) == 512u, "second half");
    Expect(!ring.Allocate(256, 3, 0).has_value(), "a full ring refuses while both batches run");
    const auto wrapped = ring.Allocate(256, 3, 1);
    Expect(wrapped == 0u, "space of a completed batch is reused from the start");
    Expect(ring.Allocate(256, 3, 1) == 256u, "a wrapped ring fills up to the oldest live region");
    Expect(!ring.Allocate(1, 3, 1).has_value(), "a wrapped ring does not pass the oldest live region");
    const auto counts = ring.TakeCounts();
    Expect(counts.wraps == 1 && counts.full == 2 && counts.retired == 1, "wraps, refusals and retired regions are counted");
}

void testEmptyRingRestartsAtZero() {
    RingSpace ring(1024, 16);
    Expect(ring.Allocate(700, 1, 0) == 0u, "first allocation");
    Expect(ring.Allocate(700, 2, 1) == 0u, "an idle ring starts again at zero");
    Expect(!ring.Allocate(2048, 3, 2).has_value() && !ring.Allocate(0, 3, 2).has_value(), "empty and oversized requests are refused");
}

}

int main() {
    testAllocationsAreAlignedAndMergedPerSerial();
    testFullRingRefusesUntilTheBatchCompletes();
    testEmptyRingRestartsAtZero();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified data ring: allocation, wrap and retire checks passed\n");
    return 0;
}
