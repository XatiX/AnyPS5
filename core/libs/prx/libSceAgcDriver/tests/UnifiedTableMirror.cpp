#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

using namespace AgcDriver::Unified;
using namespace AgcDriver::Unified::Heap;
namespace Abi = ShaderRecompiler::ImageTableAbi;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class FakeDevice final : public HeapDevice {
public:
    void WriteImage(std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t) override {}
    void WriteSampler(std::uint32_t, std::uint64_t) override {}
};

struct Fixture {
    FakeDevice device;
    SlotTable table{device, 16u, 8u, 4u};
    Events::ChangeLog shared;
    Events::ChangeLog imageLog;
    Memo memo{shared, imageLog};
    std::map<std::uint32_t, std::shared_ptr<const void>> textures;
    Refill refill{table, memo,
                  [this](const SlotRequest& request) -> std::optional<ResolvedImage> {
                      auto& owner = textures[request.words[0]];
                      if (owner == nullptr) owner = std::make_shared<int>(0);
                      return ResolvedImage{{0x1000u + request.words[0], 1u, owner}, {0x300000000ull + request.words[0] * Events::RegionBytes, 0x1000}};
                  },
                  [](const SlotRequest&) -> std::optional<ResolvedSampler> { return std::nullopt; }};
    Tables::TableMirror mirror{refill, shared, imageLog};
};

ShaderRecompiler::ImageTableElement Element(std::uint32_t word, ShaderRecompiler::DescriptorBindingKind kind = static_cast<ShaderRecompiler::DescriptorBindingKind>(FirstImageKind)) {
    ShaderRecompiler::ImageTableElement element;
    element.words[0] = word;
    element.words[1] = 1u;
    element.kind = kind;
    return element;
}

std::shared_ptr<ShaderRecompiler::ResolvedImageTables> Tables(std::vector<std::uint32_t> words) {
    auto tables = std::make_shared<ShaderRecompiler::ResolvedImageTables>();
    tables->map.assign(Abi::HeaderWords + 4u, 7u);
    tables->map[Abi::HeaderElementBase] = 0u;
    for (const auto word : words) tables->elements.push_back(Element(word));
    tables->map[Abi::HeaderElementCount] = static_cast<std::uint32_t>(tables->elements.size());
    return tables;
}

void testMirrorAppendsSlots() {
    Fixture fixture;
    const auto tables = Tables({1u, 2u, 0u});
    tables->elements[2].words[1] = 0u;
    std::string reason;
    const auto mirror = fixture.mirror.Build(tables, reason);
    Expect(mirror != nullptr, "a sampled table mirrors");
    if (mirror == nullptr) return;
    const auto base = mirror->map[Abi::HeaderElementBase];
    Expect(base == tables->map.size(), "the element base points past the resolved map");
    Expect(mirror->map.size() == tables->map.size() + 3u, "one slot word per element");
    Expect(mirror->map[base] == 1u && mirror->map[base + 1u] == 2u, "active elements read their heap slots");
    Expect(mirror->map[base + 2u] == 0u, "an inactive element reads the placeholder slot");
    Expect(std::equal(tables->map.begin() + Abi::HeaderElementCount, tables->map.end(), mirror->map.begin() + Abi::HeaderElementCount), "the rest of the map is kept");
}

void testMirrorReusedForSameTables() {
    Fixture fixture;
    const auto tables = Tables({1u, 2u});
    std::string reason;
    const auto first = fixture.mirror.Build(tables, reason);
    const auto second = fixture.mirror.Build(tables, reason);
    Expect(first != nullptr && first == second, "the same tables with the same slots reuse the mirror");
    Expect(fixture.mirror.Counts().built == 1u && fixture.mirror.Counts().unchanged == 1u && fixture.mirror.Counts().elements == 2u, "an unchanged mirror is reused without a refill");
    const auto other = fixture.mirror.Build(Tables({3u, 4u}), reason);
    Expect(other != nullptr && other->id != first->id, "different tables build a new mirror");
}

void testWritesToElementImagesRefill() {
    Fixture fixture;
    const auto tables = Tables({1u, 2u});
    std::string reason;
    std::vector<ImageUse> uses;
    const auto first = fixture.mirror.Build(tables, reason, &uses);
    Expect(first != nullptr && uses.size() == 2u, "a built mirror reports one use per element image");
    uses.clear();
    static_cast<void>(fixture.mirror.Build(tables, reason, &uses));
    Expect(uses.size() == 2u && fixture.mirror.Counts().unchanged == 1u, "an unchanged mirror reports the same uses");
    static_cast<void>(fixture.imageLog.Note(0x300000000ull + 2u * Events::RegionBytes, 0x100));
    const auto again = fixture.mirror.Build(tables, reason);
    Expect(again == first && fixture.mirror.Counts().reused == 1u && fixture.mirror.Counts().elements == 3u && fixture.mirror.Counts().kept == 1u, "a write to an element image refills that element alone");
    static_cast<void>(fixture.shared.Note(0x400000000ull, 0x100));
    static_cast<void>(fixture.mirror.Build(tables, reason));
    Expect(fixture.mirror.Counts().unchanged == 2u, "a write elsewhere keeps the mirror unchanged");
}

void testNewTablesInheritTheirPredecessor() {
    Fixture fixture;
    std::string reason;
    const auto first = fixture.mirror.Build(Tables({1u, 2u, 3u}), reason);
    const auto same = fixture.mirror.Build(Tables({1u, 2u, 3u}), reason);
    Expect(first != nullptr && same == first, "new tables with the same map and elements keep the mirror");
    Expect(fixture.mirror.Counts().inherited == 1u && fixture.mirror.Counts().kept == 3u && fixture.mirror.Counts().elements == 3u, "their elements keep the predecessor's slots without a refill");
    const auto changed = fixture.mirror.Build(Tables({1u, 4u, 3u}), reason);
    Expect(changed != nullptr && changed->id != first->id, "a changed element builds a new mirror");
    Expect(fixture.mirror.Counts().elements == 4u && fixture.mirror.Counts().kept == 5u, "only the changed element is filled");
    if (changed == nullptr) return;
    const auto base = changed->map[Abi::HeaderElementBase];
    const auto firstBase = first->map[Abi::HeaderElementBase];
    Expect(changed->map[base] == first->map[firstBase] && changed->map[base + 2u] == first->map[firstBase + 2u] && changed->map[base + 1u] != first->map[firstBase + 1u], "kept elements keep their slots and the new one takes another slot");
    static_cast<void>(fixture.mirror.Build(Tables({1u, 2u}), reason));
    Expect(fixture.mirror.Counts().inherited == 2u, "a table with another map does not inherit");
}

void testSpansCoalesce() {
    const std::vector<ImageUse> uses{{{0x2000u, 0x1000u}, false}, {{0x1000u, 0x1000u}, false}, {{0x8000u, 0x100u}, false}, {{0x2800u, 0x1000u}, false}};
    const auto spans = Tables::CoalescedSpans(uses);
    Expect(spans.size() == 2u && spans[0] == std::pair<std::uint64_t, std::uint64_t>{0x1000u, 0x3800u} && spans[1].first == 0x8000u, "touching and overlapping spans merge in address order");
}

void testUsesAreCoalescedSpans() {
    Fixture fixture;
    const auto tables = Tables({1u, 2u});
    tables->elements.push_back(Element(1u));
    tables->elements.back().words[1] = 2u;
    std::string reason;
    std::vector<ImageUse> uses;
    static_cast<void>(fixture.mirror.Build(tables, reason, &uses));
    Expect(uses.size() == 2u && uses[0].memory.address < uses[1].memory.address && !uses[0].write, "elements over the same memory report one sampled use");
}

void testOutsideCutsHoles() {
    const auto pieces = Tables::Outside({0x1000u, 0x5000u}, {{0x4000u, 0x6000u}, {0x2000u, 0x3000u}, {0x8000u, 0x9000u}});
    Expect(pieces.size() == 2u && pieces[0] == std::pair<std::uint64_t, std::uint64_t>{0x1000u, 0x2000u} && pieces[1] == std::pair<std::uint64_t, std::uint64_t>{0x3000u, 0x4000u}, "holes are cut out of the span in address order");
    Expect(Tables::Outside({0x1000u, 0x2000u}, {{0x0u, 0x3000u}}).empty(), "a span inside a hole leaves nothing");
    Expect(Tables::Outside({0x1000u, 0x2000u}, {}).size() == 1u, "a span without holes is kept whole");
}

void testMirrorRefusesStorageAndSamplers() {
    Fixture fixture;
    std::string reason;
    auto storage = Tables({1u});
    storage->elements[0].kind = static_cast<ShaderRecompiler::DescriptorBindingKind>(LastSampledKind + 1u);
    Expect(fixture.mirror.Build(storage, reason) == nullptr && reason == "storage image tables", "storage tables are refused");
    auto samplers = Tables({1u});
    samplers->samplers.push_back({});
    Expect(fixture.mirror.Build(samplers, reason) == nullptr && reason == "sampler tables", "sampler tables are refused");
    Expect(fixture.mirror.Build(nullptr, reason) == nullptr, "missing tables are refused");
}

void testReleasedTablesAreDropped() {
    constexpr auto floor = static_cast<std::uint32_t>(AgcDriver::Unified::Tables::MirrorPruneFloor);
    Fixture fixture;
    std::string reason;
    for (std::uint32_t round = 0; round < 4u * floor; ++round) {
        static_cast<void>(fixture.mirror.Build(Tables({1u + round % 3u, 2u, 3u}), reason));
    }
    Expect(fixture.mirror.Size() < floor, "entries of released tables do not pile up");
    Expect(fixture.mirror.Counts().dropped != 0u, "the released entries are counted as dropped");
    Expect(fixture.mirror.Counts().inherited == 4u * floor - 1u, "each new table still inherits from the last one");
    const auto held = Tables({7u, 8u});
    const auto first = fixture.mirror.Build(held, reason);
    for (std::uint32_t round = 0; round < 2u * floor; ++round) {
        static_cast<void>(fixture.mirror.Build(Tables({1u, 2u, 3u}), reason));
    }
    Expect(fixture.mirror.Build(held, reason) == first && fixture.mirror.Counts().unchanged != 0u, "live tables keep their entry");
}

}

int main() {
    testReleasedTablesAreDropped();
    testMirrorAppendsSlots();
    testMirrorReusedForSameTables();
    testWritesToElementImagesRefill();
    testNewTablesInheritTheirPredecessor();
    testSpansCoalesce();
    testUsesAreCoalescedSpans();
    testOutsideCutsHoles();
    testMirrorRefusesStorageAndSamplers();
    if (failures != 0) return 1;
    std::printf("unified table mirror tests passed\n");
    return 0;
}
