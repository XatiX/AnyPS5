#include "prx/libSceAgcDriver/Unified/include/Images.hpp"
#include <array>
#include <cstdio>

using namespace AgcDriver::Unified;
using namespace AgcDriver::Unified::Images;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class FakeSurface final : public DeviceSurface {
public:
    std::uint64_t View(const ViewKey& view, std::uint32_t kind, std::string&) override { return 0x100u + view.baseLevel + kind; }
    std::uint32_t Layout() const override { return 1u; }
};

class FakeDevice final : public SurfaceDevice {
public:
    std::uint32_t created = 0;
    bool refuse = false;
    std::shared_ptr<DeviceSurface> Create(const SurfaceKey&, std::span<const std::uint32_t, 8>, std::string& reason) override {
        if (refuse) {
            reason = "refused";
            return nullptr;
        }
        ++created;
        return std::make_shared<FakeSurface>();
    }
};

const std::array<std::uint32_t, 8> words{};

SurfaceKey Key(std::uint64_t address) {
    SurfaceKey key;
    key.address = address;
    key.bytes = 0x10000;
    key.format = 10;
    key.width = 64;
    key.height = 64;
    key.mips = 1;
    return key;
}

void testSurfacesAreSharedAndQueued() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto first = table.Find(Key(0x200000000ull), words, reason);
    const auto second = table.Find(Key(0x200000000ull), words, reason);
    Expect(first.has_value() && second.has_value() && first->surface == second->surface, "one surface per key");
    Expect(!first->ready && device.created == 1u, "a new surface is pending and created once");
    auto work = table.TakeWork();
    Expect(work.size() == 1u && table.TakeWork().empty(), "a new surface is queued once");
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    const auto ready = table.Find(Key(0x200000000ull), words, reason);
    Expect(ready.has_value() && ready->ready, "a detiled surface is ready");
}

void testWritesMakeSurfacesStale() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0x300000000ull);
    static_cast<void>(table.Find(key, words, reason));
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    static_cast<void>(shared.Note(0x400000000ull, 0x1000));
    Expect(table.Find(key, words, reason)->ready, "a write elsewhere leaves the surface ready");
    static_cast<void>(shared.Note(key.address + 0x2000u, 0x100));
    const auto stale = table.Find(key, words, reason);
    Expect(stale.has_value() && !stale->ready && stale->surface->state == State::Stale, "a write inside the surface makes it stale");
    static_cast<void>(table.Find(key, words, reason));
    Expect(table.TakeWork().size() == 1u, "a stale surface is queued once");
    const auto counts = table.TakeCounts();
    Expect(counts.created == 1u && counts.stale == 1u && counts.detiled == 1u, "counts follow the surface");
}

void testRefusals() {
    FakeDevice device;
    device.refuse = true;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    Expect(!table.Find(Key(0x500000000ull), words, reason).has_value() && reason == "refused", "a surface the device refuses is not kept");
    Expect(table.Size() == 0u, "nothing is stored for a refusal");
    const std::array<std::uint32_t, 8> empty{};
    Expect(!KeyOf(empty, reason).has_value(), "an empty T# has no key");
}

void testGpuWrittenSurfacesAreLeftToTheExistingCache() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0x600000000ull);
    static_cast<void>(gpuWrites.Note(key.address, 0x1000));
    const auto found = table.Find(key, words, reason);
    Expect(found.has_value() && found->gpuWritten && found->surface == nullptr && device.created == 0u, "a GPU-written surface is not created");
    Expect(table.TakeCounts().gpuWritten == 1u, "the GPU-written lookup is counted");
}

void testImageSetNotesAreNotGpuWrites() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0x700000000ull);
    static_cast<void>(shared.Note(key.address, 0x1000, Events::ImageSetOwner));
    const auto found = table.Find(key, words, reason);
    Expect(found.has_value() && !found->gpuWritten && found->surface != nullptr && device.created == 1u, "an image-set note alone does not make a surface GPU-written");
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    static_cast<void>(shared.Note(key.address, 0x1000, Events::ImageSetOwner));
    Expect(table.Find(key, words, reason)->ready, "an image-set note leaves a detiled surface ready");
    static_cast<void>(shared.Note(key.address + 0x1000u, 0x100));
    Expect(!table.Find(key, words, reason)->ready, "a store after an image-set note makes the surface stale");
    work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    static_cast<void>(gpuWrites.Note(key.address, 0x100, 8u));
    const auto written = table.Find(key, words, reason);
    Expect(written.has_value() && written->gpuWritten && !written->ready, "a GPU write hands a detiled surface back to the existing cache");
}

void testOwnGpuWritesKeepTheSurface() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0x900000000ull);
    const auto created = table.Find(key, words, reason);
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    const auto owner = OwnerOf(*created->surface);
    static_cast<void>(gpuWrites.Note(key.address, key.bytes, owner));
    static_cast<void>(shared.Note(key.address, key.bytes, owner));
    table.MarkGpuWritten(*created->surface);
    const auto served = table.Find(key, words, reason);
    Expect(served.has_value() && served->ready && !served->gpuWritten && served->surface == created->surface, "a surface's own GPU write keeps it served");
    Expect(served->surface->gpuNewer && table.TakeWork().empty(), "an own write needs no detile");
    static_cast<void>(gpuWrites.Note(key.address, key.bytes, owner));
    static_cast<void>(shared.Note(key.address, key.bytes, owner));
    table.MarkGpuWritten(*created->surface);
    Expect(table.Find(key, words, reason)->ready, "repeated own writes keep it served");
    const auto counts = table.TakeCounts();
    Expect(counts.ownWrites == 2u && counts.ownServed == 2u && counts.gpuWritten == 0u, "own writes and serves are counted");
}

void testForeignWritesHandTheSurfaceBack() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0xa00000000ull);
    const auto created = table.Find(key, words, reason);
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    const auto owner = OwnerOf(*created->surface);
    static_cast<void>(gpuWrites.Note(key.address, key.bytes, owner));
    table.MarkGpuWritten(*created->surface);
    static_cast<void>(shared.Note(key.address + 0x100u, 0x10));
    const auto stored = table.Find(key, words, reason);
    Expect(stored.has_value() && stored->gpuWritten && stored->surface == nullptr && !created->surface->gpuNewer, "a store over a GPU-newer surface hands it back");
    static_cast<void>(gpuWrites.Note(key.address, 0x10, 8u));
    Expect(table.Find(key, words, reason)->gpuWritten, "another GPU writer hands it back");
}

void testCpuReuseMakesARecordStale() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0xb00000000ull);
    const auto created = table.Find(key, words, reason);
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    const auto owner = OwnerOf(*created->surface);
    static_cast<void>(gpuWrites.Note(key.address, key.bytes, owner));
    static_cast<void>(shared.Note(key.address, key.bytes, owner));
    table.MarkGpuWritten(*created->surface);
    Expect(!table.ChangedSinceWrite(*created->surface), "a record's own write leaves it current");
    static_cast<void>(shared.Note(key.address + 0x8000u, 0x40));
    Expect(table.ChangedSinceWrite(*created->surface), "a CPU store into the record's memory after its GPU write makes it stale");
}

void testRetiredRecordLeavesTheTable() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0xc00000000ull);
    const auto created = table.Find(key, words, reason);
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    table.MarkGpuWritten(*created->surface);
    static_cast<void>(table.Claim(key.address, key.bytes, Side::Record, "target"));
    table.Retire(created->surface);
    Expect(created->surface->retired && !created->surface->gpuNewer, "a retired record is no longer newer than memory");
    Expect(table.Overlapping(key.address, key.bytes).empty() && table.Size() == 0u, "a retired record leaves the table");
    Expect(table.OwnerOfRange(key.address, key.bytes) == Side::None, "its claim goes with it, so another shape can claim the range");
    Expect(table.TakeCounts().retired == 1u, "the retirement is counted");
}

void testStaleMirrorIndex() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto key = Key(0xd00000000ull);
    const auto created = table.Find(key, words, reason);
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), gpuWrites.Serial());
    Expect(table.StaleOverlapping(key.address, key.bytes).empty(), "a record no GPU work wrote leaves the mirror current");
    table.MarkGpuWritten(*created->surface);
    Expect(table.StaleOverlapping(key.address + 0x100u, 4u).size() == 1u, "a buffer read inside a GPU-written record finds it");
    Expect(table.StaleOverlapping(key.address + key.bytes, 4u).empty() && table.StaleOverlapping(key.address - 4u, 4u).empty(), "reads next to it do not");
    table.MarkWrittenBack(*created->surface);
    Expect(table.StaleOverlapping(key.address, key.bytes).empty(), "a written-back record leaves the index");
    table.MarkGpuWritten(*created->surface);
    Expect(table.StaleOverlapping(key.address, 4u).size() == 1u, "the next GPU write makes it stale again");
    table.Retire(created->surface);
    Expect(table.StaleOverlapping(key.address, key.bytes).empty(), "a retired record leaves the index");
}

void testOverlapQuery() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto first = table.Find(Key(0xb00000000ull), words, reason);
    const auto second = table.Find(Key(0xb00010000ull), words, reason);
    Expect(table.Overlapping(0xb00008000ull, 0x10).size() == 1u && table.Overlapping(0xb00008000ull, 0x10).front() == first->surface, "a range inside one surface finds it");
    Expect(table.Overlapping(0xb0000ff00ull, 0x200).size() == 2u, "a range across both finds both");
    Expect(table.Overlapping(0xb00020000ull, 0x1000).empty(), "a range past both finds none");
    static_cast<void>(second);
}

void testBudgetDropsLeastRecentlyUsed() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites, 0x30000);
    std::string reason;
    const auto first = table.Find(Key(0x800000000ull), words, reason);
    static_cast<void>(table.Find(Key(0x810000000ull), words, reason));
    static_cast<void>(table.Find(Key(0x820000000ull), words, reason));
    Expect(table.Size() == 3u && table.HeldBytes() == 0x30000u, "surfaces within the budget are all kept");
    static_cast<void>(table.Find(Key(0x800000000ull), words, reason));
    static_cast<void>(table.Find(Key(0x830000000ull), words, reason));
    Expect(table.Size() == 2u && table.HeldBytes() == 0x20000u, "going over the budget drops down to seven eighths of it");
    Expect(table.TakeCounts().evicted == 2u, "the drops are counted");
    Expect(table.Find(Key(0x800000000ull), words, reason)->surface == first->surface, "the surface used last before the new one stays");
    const auto work = table.TakeWork();
    Expect(work.size() == 2u, "dropped surfaces leave the work queue");
    for (const auto& surface : work) Expect(!surface->retired, "queued work is only live surfaces");
    Expect(first->surface->device != nullptr, "a dropped surface stays alive while a draw holds it");
    const auto created = device.created;
    static_cast<void>(table.Find(Key(0x810000000ull), words, reason));
    Expect(device.created == created + 1u, "a dropped surface is created again on its next lookup");
}

}

void testRangesKeepTheirFirstOwner() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    Expect(table.OwnerOfRange(0xc00000000ull, 0x10000) == Side::None, "an unclaimed range has no owner");
    Expect(table.Claim(0xc00000000ull, 0x10000, Side::Record, "target") == Side::Record, "the first claim takes the range");
    Expect(table.Claim(0xc00008000ull, 0x100, Side::Upstream, "late") == Side::Record, "a later claim inside it does not move it");
    Expect(table.OwnerOfRange(0xc0000ff00ull, 0x200) == Side::Record, "a range touching it reads the owner");
    Expect(table.Claim(0xc00010000ull, 0x10000, Side::Upstream, "metadata") == Side::Upstream, "the neighbouring range is claimed separately");
    Expect(table.OwnerOfRange(0xc0000ff00ull, 0x200) == Side::Upstream, "the existing cache wins a range across both owners");
    const auto record = table.Seed(table.Adopt(Key(0xc00000000ull), words, reason));
    Expect(record != nullptr && record->seeded && table.TakeWork().size() == 1u, "a seeded record is queued once for its first content");
    static_cast<void>(table.Seed(record));
    Expect(table.TakeWork().empty(), "seeding it again queues nothing");
    table.Demote(0xc00000000ull, 0x10000, "another shape");
    Expect(table.OwnerOfRange(0xc00000000ull, 0x10000) == Side::Upstream, "a demoted range belongs to the existing cache");
    Expect(record->retired && table.Overlapping(0xc00000000ull, 0x10000).empty(), "its record is retired");
    Expect(table.Claim(0xc00000000ull, 0x10000, Side::Record, "again") == Side::Upstream, "a demoted range is not claimed back");
    Expect(table.OwnersLine().find("1 demoted") != std::string::npos, "the owners line counts the demotion");
}

void testCpuWrittenRecordIsDetiledAgain() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto record = table.Seed(table.Adopt(Key(0xc00040000ull), words, reason));
    Expect(record != nullptr && table.TakeWork().size() == 1u, "a new record is queued for its first content");
    table.MarkReady(*record, shared.Serial(), gpuWrites.Serial());
    Expect(!table.RefreshIfChanged(record), "an unchanged record is not detiled again");
    static_cast<void>(shared.Note(0xc00041000ull, 0x100));
    Expect(table.RefreshIfChanged(record) && table.TakeWork().size() == 1u, "a CPU write under the record queues it again");
    Expect(!table.RefreshIfChanged(record), "a queued record is not queued twice");
    table.MarkReady(*record, shared.Serial(), gpuWrites.Serial());
    table.MarkGpuWritten(*record);
    static_cast<void>(shared.Note(0xc00041000ull, 0x100));
    Expect(!table.RefreshIfChanged(record), "a GPU-written record keeps its content");
}

void testArrayClaimOutlivesItsSlices() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog gpuWrites;
    SurfaceTable table(device, shared, gpuWrites);
    std::string reason;
    const auto slice = table.Seed(table.Adopt(Key(0xc00020000ull), words, reason));
    Expect(slice != nullptr && table.Claim(0xc00020000ull, 0x10000, Side::Record, "target") == Side::Record, "a slice claims its own range");
    Expect(table.Claim(0xc00000000ull, 0x40000, Side::Record, "sampled") == Side::Record, "an array over the slice is a record range too");
    table.Retire(slice);
    Expect(table.OwnerOfRange(0xc00000000ull, 0x40000) == Side::Record, "retiring the slice leaves the array's claim");
}

int main() {
    testBudgetDropsLeastRecentlyUsed();
    testArrayClaimOutlivesItsSlices();
    testCpuWrittenRecordIsDetiledAgain();
    testGpuWrittenSurfacesAreLeftToTheExistingCache();
    testImageSetNotesAreNotGpuWrites();
    testSurfacesAreSharedAndQueued();
    testWritesMakeSurfacesStale();
    testRefusals();
    testOwnGpuWritesKeepTheSurface();
    testForeignWritesHandTheSurfaceBack();
    testCpuReuseMakesARecordStale();
    testRetiredRecordLeavesTheTable();
    testStaleMirrorIndex();
    testOverlapQuery();
    testRangesKeepTheirFirstOwner();
    if (failures != 0) return 1;
    std::printf("unified images tests passed\n");
    return 0;
}
