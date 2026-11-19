#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

using namespace AgcDriver::Unified;
using namespace AgcDriver::Unified::Heap;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class FakeDevice final : public HeapDevice {
public:
    std::uint32_t writes = 0;
    void WriteImage(std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t) override { ++writes; }
    void WriteSampler(std::uint32_t, std::uint64_t) override { ++writes; }
};

constexpr std::uint64_t ImageBase = 0x300000000ull;

struct Fixture {
    FakeDevice device;
    SlotTable table{device, 8u, 8u, 4u};
    Events::ChangeLog shared;
    Events::ChangeLog imageLog;
    Memo memo{shared, imageLog};
    std::map<std::uint32_t, std::shared_ptr<const void>> textures;
    std::shared_ptr<const void> samplerObject = std::make_shared<int>(0);
    std::uint32_t imageLookups = 0;
    std::uint32_t samplerLookups = 0;
    Refill refill{table, memo,
                  [this](const SlotRequest& request) -> std::optional<ResolvedImage> {
                      ++imageLookups;
                      if (request.words[0] == 0xdeadu) return std::nullopt;
                      auto& owner = textures[request.words[0]];
                      if (owner == nullptr) owner = std::make_shared<int>(0);
                      return ResolvedImage{{0x1000u + request.words[0], 1u, owner}, {ImageBase + request.words[0] * Events::RegionBytes, 0x1000}};
                  },
                  [this](const SlotRequest& request) -> std::optional<ResolvedSampler> {
                      ++samplerLookups;
                      return ResolvedSampler{0x9000u + request.words[0], samplerObject};
                  }};
};

SlotRequest Image(std::uint32_t word, std::uint32_t flags = 0u) {
    SlotRequest request{1u, flags, {}};
    request.words[0] = word;
    request.words[1] = 1u;
    return request;
}

SlotRequest Sampler(std::uint32_t word) {
    SlotRequest request{SamplerKind, RequestFlag::Sampler, {}};
    request.words[0] = word;
    return request;
}

void testFillGivesSlotsInRequestOrder() {
    Fixture fixture;
    const std::vector<SlotRequest> requests{Image(1), Image(2), Image(0, RequestFlag::Inactive), Image(0xdead), Sampler(7), Sampler(7)};
    std::vector<std::uint32_t> slots;
    std::string reason;
    Expect(fixture.refill.Fill(requests, slots, reason), "a fill of images and samplers succeeds");
    Expect(slots.size() == requests.size(), "one slot per request");
    if (slots.size() != requests.size()) return;
    Expect(slots[0] == 1u && slots[1] == 2u, "active images take slots past the placeholder");
    Expect(slots[2] == 0u && slots[3] == 0u, "inactive and unresolved images read the placeholder");
    Expect(slots[4] == FirstSamplerSlot && slots[5] == FirstSamplerSlot, "the same S# twice shares a slot");
    Expect(fixture.samplerLookups == 1u && fixture.refill.Counts().memoHits == 1u, "the second S# comes from the memo");
    Expect(fixture.refill.Counts().placeholders == 1u, "the unresolved image counts as a placeholder");
}

void testRefillUsesTheMemoUntilTheImageChanges() {
    Fixture fixture;
    const std::vector<SlotRequest> requests{Image(1), Image(2)};
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> second;
    std::string reason;
    Expect(fixture.refill.Fill(requests, first, reason), "the first fill succeeds");
    const auto lookups = fixture.imageLookups;
    const auto writes = fixture.device.writes;
    Expect(fixture.refill.Fill(requests, second, reason) && second == first, "a second fill returns the same slots");
    Expect(fixture.imageLookups == lookups && fixture.device.writes == writes, "a second fill looks nothing up and writes nothing");
    fixture.imageLog.Note(ImageBase + 1u * Events::RegionBytes + 0x10, 4);
    Expect(fixture.refill.Fill(requests, second, reason) && second == first, "after an image write the slots stay the same");
    Expect(fixture.imageLookups == lookups + 1u && fixture.device.writes == writes, "only the written image is looked up again, and its live view keeps its slot");
}

void testFillReportsImageMemory() {
    Fixture fixture;
    const std::vector<SlotRequest> requests{Image(1), Image(2, RequestFlag::Storage | RequestFlag::Written), Image(0, RequestFlag::Inactive), Image(0xdead), Sampler(7)};
    std::vector<std::uint32_t> slots;
    std::vector<ImageUse> uses;
    std::string reason;
    Expect(fixture.refill.Fill(requests, slots, reason, &uses), "a fill with image uses succeeds");
    Expect(uses.size() == 2u, "only resolved images report memory");
    if (uses.size() != 2u) return;
    Expect(uses[0].memory.address == ImageBase + Events::RegionBytes && uses[0].memory.bytes == 0x1000 && !uses[0].write, "a sampled image reports its memory as a read");
    Expect(uses[1].memory.address == ImageBase + 2u * Events::RegionBytes && uses[1].write, "a written storage image reports a write");
    uses.clear();
    Expect(fixture.refill.Fill(requests, slots, reason, &uses) && uses.size() == 2u && uses[0].memory.address == ImageBase + Events::RegionBytes, "memo hits report the same memory");
}

void testExpiredTextureTakesANewLookup() {
    Fixture fixture;
    const std::vector<SlotRequest> requests{Image(1)};
    std::vector<std::uint32_t> slots;
    std::string reason;
    Expect(fixture.refill.Fill(requests, slots, reason), "the first fill succeeds");
    fixture.textures.clear();
    const auto lookups = fixture.imageLookups;
    Expect(fixture.refill.Fill(requests, slots, reason) && slots[0] == 1u, "a recreated texture with the same view keeps the slot");
    Expect(fixture.imageLookups == lookups + 1u, "an expired texture is looked up again");
}

void testFullHeapFailsTheFill() {
    Fixture fixture;
    std::vector<SlotRequest> requests;
    for (std::uint32_t word = 1; word <= 8u; ++word) requests.push_back(Image(word));
    std::vector<std::uint32_t> slots;
    std::string reason;
    Expect(!fixture.refill.Fill(requests, slots, reason) && !reason.empty() && fixture.refill.Counts().failures == 1u, "more live images than slots fails with a reason");
}

}

int main() {
    testFillGivesSlotsInRequestOrder();
    testRefillUsesTheMemoUntilTheImageChanges();
    testFillReportsImageMemory();
    testExpiredTextureTakesANewLookup();
    testFullHeapFailsTheFill();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified refill: slot fill and memo checks passed\n");
    return 0;
}
