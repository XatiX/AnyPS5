#include "prx/libSceAgcDriver/Unified/include/LabelTable.hpp"
#include <array>
#include <cstdio>
#include <cstring>

using namespace AgcDriver::Unified::Labels;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

std::array<std::byte, 8> bytesOf(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

void testIssuedAfterFindsALaterStore() {
    LabelTable table;
    const auto value = bytesOf(5u);
    const auto stamp = table.Note(0x1000, std::span(value).first(4), 0);
    const auto found = table.IssuedAfter(0x1000, 4, 0, [](std::uint64_t seen) { return seen == 5u; });
    Expect(found.has_value() && found->stamp == stamp && found->queue == 0 && found->value == 5u, "a store issued after the wait's point satisfies it");
    const auto earlier = table.IssuedAfter(0x1000, 4, stamp, [](std::uint64_t seen) { return seen == 5u; });
    Expect(!earlier.has_value(), "a store issued before the wait's point does not satisfy it");
}

void testCompareSelectsTheMatchingStore() {
    LabelTable table;
    const auto one = bytesOf(1u);
    const auto two = bytesOf(2u);
    static_cast<void>(table.Note(0x2000, std::span(one).first(4), 1));
    static_cast<void>(table.Note(0x2000, std::span(two).first(4), 0));
    const auto found = table.IssuedAfter(0x2000, 4, 0, [](std::uint64_t seen) { return seen == 1u; });
    Expect(found.has_value() && found->queue == 1 && found->value == 1u, "the older store that matches is found behind a newer one");
    const auto none = table.IssuedAfter(0x2000, 4, 0, [](std::uint64_t seen) { return seen == 3u; });
    Expect(!none.has_value(), "no store matches a value never written");
}

void testEightByteLabelsJoinBothWords() {
    LabelTable table;
    const auto wide = bytesOf(0x0000000700000009ull);
    static_cast<void>(table.Note(0x3000, wide, 0));
    const auto found = table.IssuedAfter(0x3000, 8, 0, [](std::uint64_t seen) { return seen == 0x0000000700000009ull; });
    Expect(found.has_value(), "an 8-byte label compares both words of one store");
    const auto narrow = table.IssuedAfter(0x3004, 4, 0, [](std::uint64_t seen) { return seen == 7u; });
    Expect(narrow.has_value(), "the upper word of an 8-byte store is found alone");
}

void testHistoryKeepsTheLastStores() {
    LabelTable table;
    for (std::uint64_t value = 1; value <= History + 1u; ++value) {
        const auto bytes = bytesOf(value);
        static_cast<void>(table.Note(0x4000, std::span(bytes).first(4), 0));
    }
    const auto oldest = table.IssuedAfter(0x4000, 4, 0, [](std::uint64_t seen) { return seen == 1u; });
    Expect(!oldest.has_value(), "a store older than the history is forgotten");
    const auto newest = table.IssuedAfter(0x4000, 4, 0, [](std::uint64_t seen) { return seen == History + 1u; });
    Expect(newest.has_value(), "the newest store is kept");
}

}

int main() {
    testIssuedAfterFindsALaterStore();
    testCompareSelectsTheMatchingStore();
    testEightByteLabelsJoinBothWords();
    testHistoryKeepsTheLastStores();
    if (failures != 0) return 1;
    std::printf("agc_unified_label_table_tests passed\n");
    return 0;
}
