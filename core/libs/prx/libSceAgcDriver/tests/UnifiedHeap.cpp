#include "prx/libSceAgcDriver/Unified/include/Heap.hpp"
#include <cstdio>
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
    struct Write {
        std::uint32_t kind;
        std::uint32_t slot;
        std::uint64_t handle;
    };
    std::vector<Write> writes;

    void WriteImage(std::uint32_t kind, std::uint32_t slot, std::uint64_t view, std::uint32_t) override { writes.push_back({kind, slot, view}); }
    void WriteSampler(std::uint32_t slot, std::uint64_t sampler) override { writes.push_back({SamplerKind, slot, sampler}); }
};

std::shared_ptr<const void> Owner() {
    return std::make_shared<int>(0);
}

void testImageSlotsSkipThePlaceholder() {
    FakeDevice device;
    SlotTable table(device);
    const auto owner = Owner();
    Expect(table.ImageSlot(1, {0x10, 0, owner}) == 1u, "the first image of a kind takes slot 1, past the placeholder");
    Expect(table.ImageSlot(1, {0x20, 0, owner}) == 2u, "a second view takes slot 2");
    Expect(table.ImageSlot(2, {0x30, 0, owner}) == 1u, "each kind numbers its own slots");
    Expect(table.ImageSlot(1, {0x10, 0, owner}) == 1u && device.writes.size() == 3u && table.Counts().hits == 1u, "the same live view keeps its slot and is written once");
    Expect(table.ImageSlot(0, {0x40, 0, owner}) == NoSlot && table.ImageSlot(SamplerKind, {0x40, 0, owner}) == NoSlot, "kinds outside the image range have no slots");
    Expect(table.ImageSlot(LastSampledKind + 1u, {0x40, 0, owner}) == 1u, "storage kinds have their own slots");
    SlotTable noStorage(device, SampledSlots, SamplerSlots, 0u);
    Expect(noStorage.ImageSlot(LastStorageKind, {0x40, 0, owner}) == NoSlot && noStorage.ImageSlot(LastSampledKind, {0x40, 0, owner}) == 1u, "without storage slots only sampled kinds get slots");
    Expect(table.ImageSlot(1, {0x50, 0, nullptr}) == NoSlot, "an image without an owner gets no slot");
}

void testExpiredOwnerRecyclesInPlace() {
    FakeDevice device;
    SlotTable table(device);
    auto owner = Owner();
    const auto slot = table.ImageSlot(3, {0x10, 0, owner});
    owner.reset();
    const auto again = table.ImageSlot(3, {0x10, 0, Owner()});
    Expect(again == slot && device.writes.size() == 2u && table.Counts().recycled == 1u, "a view whose owner expired is rewritten in its old slot");
}

void testFullTableSweepsExpiredOwners() {
    FakeDevice device;
    SlotTable table(device, 4u, 8u);
    const auto kept = Owner();
    auto dropped = Owner();
    Expect(table.ImageSlot(1, {0x10, 0, kept}) == 1u, "slot 1 for the kept image");
    Expect(table.ImageSlot(1, {0x20, 0, dropped}) == 2u, "slot 2 for the dropped image");
    Expect(table.ImageSlot(1, {0x30, 0, kept}) == 3u, "slot 3 fills the table");
    dropped.reset();
    Expect(table.ImageSlot(1, {0x40, 0, kept}) == 2u, "a full table reuses the slot of an expired owner");
    Expect(table.ImageSlot(1, {0x50, 0, kept}) == NoSlot && table.Counts().full == 1u, "a full table with only live owners has no slot");
    Expect(table.ImageSlot(1, {0x10, 0, kept}) == 1u, "a live owner's slot survives the sweep");
}

void testSamplerSlots() {
    FakeDevice device;
    SlotTable table(device, 4u, 4u);
    Expect(table.SamplerSlot(0x100, Owner()) == FirstSamplerSlot, "samplers start past the reserved slots");
    Expect(table.SamplerSlot(0x100, nullptr) == FirstSamplerSlot && device.writes.size() == 1u, "the same sampler keeps its slot");
    Expect(table.SamplerSlot(0x200, Owner()) == FirstSamplerSlot + 1u, "a second sampler takes the next slot");
    Expect(table.SamplerSlot(0x300, Owner()) == NoSlot, "a full sampler heap has no slot");
    Expect(table.SamplerSlot(0, Owner()) == NoSlot, "a null sampler has no slot");
}

constexpr std::uint64_t Descriptor = 0x200000000ull;
constexpr std::uint64_t Image = 0x300000000ull;

DescriptorKey Key(std::uint32_t word) {
    DescriptorKey key{5, {}};
    key.words[0] = word;
    return key;
}

void testMemoHitsUntilAWrite() {
    Events::ChangeLog shared;
    Events::ChangeLog images;
    Memo memo(shared, images);
    const auto owner = Owner();
    Expect(!memo.Find(Key(1)).has_value(), "an empty memo misses");
    memo.Store(Key(1), 7, owner, {Descriptor, 32}, {Image, 0x1000}, memo.Now());
    Expect(memo.Find(Key(1)) == 7u, "a stored descriptor hits");
    Expect(!memo.Find(Key(2)).has_value(), "different descriptor words miss");
    shared.Note(Descriptor + Events::RegionBytes, 4);
    images.Note(Image + Events::BucketBytes, 4);
    Expect(memo.Find(Key(1)) == 7u, "writes outside the descriptor and image buckets keep the hit");
    shared.Note(Descriptor, 4);
    Expect(!memo.Find(Key(1)).has_value() && memo.Size() == 0u && memo.Counts().stale == 1u, "a write to the descriptor's page drops the entry");
}

void testMemoDropsOnImageWrites() {
    Events::ChangeLog shared;
    Events::ChangeLog images;
    Memo memo(shared, images);
    const auto owner = Owner();
    memo.Store(Key(1), 3, owner, {Descriptor, 32}, {Image, 0x1000}, memo.Now());
    images.Note(Image + 0x800, 4);
    Expect(!memo.Find(Key(1)).has_value(), "an image-set write over the image drops the entry");
    memo.Store(Key(1), 3, owner, {Descriptor, 32}, {Image, 0x1000}, memo.Now());
    shared.Note(Image, 4);
    Expect(!memo.Find(Key(1)).has_value(), "a guest write over the image drops the entry");
}

void testMemoStampIsTakenBeforeTheLookup() {
    Events::ChangeLog shared;
    Events::ChangeLog images;
    Memo memo(shared, images);
    const auto owner = Owner();
    const auto before = memo.Now();
    shared.Note(Descriptor, 4);
    memo.Store(Key(1), 3, owner, {Descriptor, 32}, {}, before);
    Expect(!memo.Find(Key(1)).has_value(), "a write between the lookup and the store is seen");
}

void testMemoExpiryAndBound() {
    Events::ChangeLog shared;
    Events::ChangeLog images;
    Memo memo(shared, images, 2u);
    auto dropped = Owner();
    const auto kept = Owner();
    memo.Store(Key(1), 1, dropped, {Descriptor, 32}, {}, memo.Now());
    memo.Store(Key(2), 2, kept, {Descriptor, 32}, {}, memo.Now());
    dropped.reset();
    Expect(!memo.Find(Key(1)).has_value() && memo.Counts().expired == 1u, "an expired owner misses");
    std::weak_ptr<const void> gone;
    {
        const auto brief = Owner();
        gone = brief;
    }
    memo.Store(Key(1), 1, gone, {Descriptor, 32}, {}, memo.Now());
    Expect(memo.Size() == 1u, "an entry whose owner is gone is not stored");
    memo.Store(Key(3), 3, kept, {Descriptor, 32}, {}, memo.Now());
    memo.Store(Key(4), 4, kept, {Descriptor, 32}, {}, memo.Now());
    Expect(memo.Size() <= 2u && memo.Counts().clears == 1u && memo.Find(Key(4)) == 4u, "a full memo clears and keeps the newest entry");
}

}

int main() {
    testImageSlotsSkipThePlaceholder();
    testExpiredOwnerRecyclesInPlace();
    testFullTableSweepsExpiredOwners();
    testSamplerSlots();
    testMemoHitsUntilAWrite();
    testMemoDropsOnImageWrites();
    testMemoStampIsTakenBeforeTheLookup();
    testMemoExpiryAndBound();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified heap: slot table and memo checks passed\n");
    return 0;
}
