#include "prx/libSceAgcDriver/Unified/include/TableResolutions.hpp"
#include <cstdio>
#include <memory>
#include <vector>

using namespace AgcDriver::Unified;
using ShaderRecompiler::DescriptorValue;
using ShaderRecompiler::ImageTableColumnSnapshot;
using ShaderRecompiler::ResolvedImageTables;
using ShaderRecompiler::ResourceSnapshot;
using ShaderRecompiler::SharedValues;
using ShaderRecompiler::ShaderInfo;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

ImageTableColumnSnapshot Column(std::uint32_t word) {
    ImageTableColumnSnapshot column;
    column.base = 0x1000u;
    column.size = 0x100u;
    column.keys = 1u;
    column.codes = SharedValues<std::uint32_t>(std::vector<std::uint32_t>{0u});
    DescriptorValue value;
    value.dwords[0] = word;
    value.dwordCount = 8u;
    column.words = SharedValues<DescriptorValue>(std::vector<DescriptorValue>{value});
    return column;
}

ResourceSnapshot Snapshot(std::uint32_t word) {
    ResourceSnapshot snapshot;
    snapshot.tables.tables.push_back(Column(word));
    return snapshot;
}

struct Counter {
    int calls = 0;
    Tables::TableResolutions::Resolve Resolve() {
        return [this] {
            ++calls;
            return std::make_shared<const ResolvedImageTables>();
        };
    }
};

void testSameColumnsHit() {
    Tables::TableResolutions memo;
    Counter counter;
    const ShaderInfo info;
    const auto snapshot = Snapshot(1u);
    const auto first = memo.Find(info, snapshot, 7u, counter.Resolve());
    const auto second = memo.Find(info, snapshot, 7u, counter.Resolve());
    Expect(first != nullptr && first == second && counter.calls == 1, "the same captured columns reuse the resolution");
    Expect(memo.Counts().hits == 1u && memo.Counts().fresh == 1u, "a first resolution is new and the second a hit");
}

void testContentMatchesNewStorage() {
    Tables::TableResolutions memo;
    Counter counter;
    const ShaderInfo info;
    const auto first = memo.Find(info, Snapshot(1u), 7u, counter.Resolve());
    const auto copy = memo.Find(info, Snapshot(1u), 7u, counter.Resolve());
    Expect(copy == first && counter.calls == 1 && memo.Counts().contentHits == 1u, "a new capture of the same words reuses the resolution");
    Tables::TableResolutions storage(Tables::ResolutionBytes, false);
    static_cast<void>(storage.Find(info, Snapshot(1u), 7u, counter.Resolve()));
    static_cast<void>(storage.Find(info, Snapshot(1u), 7u, counter.Resolve()));
    Expect(counter.calls == 3 && storage.Counts().changed == 1u, "matching by storage alone resolves a new capture again");
}

void testChangedWordsResolveAgain() {
    Tables::TableResolutions memo;
    Counter counter;
    const ShaderInfo info;
    static_cast<void>(memo.Find(info, Snapshot(1u), 7u, counter.Resolve()));
    static_cast<void>(memo.Find(info, Snapshot(2u), 7u, counter.Resolve()));
    static_cast<void>(memo.Find(info, Snapshot(1u), 8u, counter.Resolve()));
    Expect(counter.calls == 3 && memo.Counts().changed == 1u && memo.Counts().fresh == 2u, "changed words and another variant resolve again");
    static_cast<void>(memo.Find(info, Snapshot(1u), 7u, counter.Resolve()));
    Expect(counter.calls == 3, "an earlier resolution of the variant is still held");
}

void testEntriesPerVariantAreBounded() {
    Tables::TableResolutions memo;
    Counter counter;
    const ShaderInfo info;
    for (std::uint32_t word = 0; word < Tables::ResolutionKeyEntries + 4u; ++word) static_cast<void>(memo.Find(info, Snapshot(word), 7u, counter.Resolve()));
    Expect(memo.Counts().held == Tables::ResolutionKeyEntries && memo.Counts().evicted == 4u, "one variant holds a bounded number of resolutions");
    const auto calls = counter.calls;
    static_cast<void>(memo.Find(info, Snapshot(Tables::ResolutionKeyEntries + 3u), 7u, counter.Resolve()));
    Expect(counter.calls == calls, "the most recent resolutions are kept");
    static_cast<void>(memo.Find(info, Snapshot(0u), 7u, counter.Resolve()));
    Expect(counter.calls == calls + 1, "the oldest resolutions are evicted");
}

void testByteBudgetEvictsOldest() {
    Tables::TableResolutions memo(1u << 12u);
    Counter counter;
    const ShaderInfo info;
    for (std::uint64_t key = 1; key <= 64u; ++key) static_cast<void>(memo.Find(info, Snapshot(1u), key, counter.Resolve()));
    Expect(memo.Counts().bytes <= (1u << 12u) && memo.Counts().evicted != 0u, "the byte budget evicts the oldest resolutions");
    const auto calls = counter.calls;
    static_cast<void>(memo.Find(info, Snapshot(1u), 64u, counter.Resolve()));
    Expect(counter.calls == calls, "the newest resolution survives the budget");
    static_cast<void>(memo.Find(info, Snapshot(1u), 1u, counter.Resolve()));
    Expect(counter.calls == calls + 1, "the oldest resolution is evicted by the budget");
}

}

int main() {
    testSameColumnsHit();
    testContentMatchesNewStorage();
    testChangedWordsResolveAgain();
    testEntriesPerVariantAreBounded();
    testByteBudgetEvictsOldest();
    if (failures != 0) return 1;
    std::printf("unified table resolution tests passed\n");
    return 0;
}
