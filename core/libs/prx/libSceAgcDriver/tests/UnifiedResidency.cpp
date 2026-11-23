#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include "prx/libSceAgcDriver/Unified/include/ResidentUploads.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <map>
#include <set>
#include <utility>

using namespace AgcDriver::Unified;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

class FakeDevice final : public ResidencyDevice {
public:
    std::uint32_t poolLimit = 4;
    bool failBind = false;
    std::set<std::uint32_t> pools;
    std::map<std::uint64_t, std::uint64_t> buffers;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::pair<std::uint32_t, std::uint32_t>> bound;
    std::uint64_t nextHandle = 1;
    std::uint64_t bindCalls = 0;

    bool CreatePool(std::uint32_t pool) override {
        if (pools.size() >= poolLimit) return false;
        pools.insert(pool);
        return true;
    }
    void DestroyPool(std::uint32_t pool) override { pools.erase(pool); }
    std::optional<DeviceBuffer> CreateBuffer(std::uint64_t bytes) override {
        const auto handle = nextHandle++;
        buffers.emplace(handle, bytes);
        return DeviceBuffer{handle, handle << 40u};
    }
    void DestroyBuffer(std::uint64_t handle) override { buffers.erase(handle); }
    bool Bind(std::uint64_t handle, std::span<const ChunkBind> binds) override {
        ++bindCalls;
        if (failBind) return false;
        for (const auto& bind : binds) {
            if (bind.bound) bound[{handle, bind.offset}] = {bind.chunk.pool, bind.chunk.index};
            else bound.erase({handle, bind.offset});
        }
        return true;
    }
};

void testAddBindsNothing() {
    FakeDevice device;
    Residency residency(device, 0);
    Expect(residency.Add(0x10008000, 0x30000), "a range is added");
    Expect(!residency.Add(0x10010000, 0x1000), "an overlapping range is refused");
    Expect(device.buffers.size() == 1 && device.buffers.begin()->second == MirrorWindowBytes, "a range below 960 GiB lies in the one mirror buffer over the window");
    Expect(device.bound.empty() && device.pools.empty(), "adding a range binds no memory");
    Expect(residency.Owns(0x10008000, 0x30000) && !residency.Owns(0x10008000, 0x30001) && !residency.Owns(0x10000000, 1), "ownership follows the added bytes");
    Expect(!residency.Translate(0x10008000, 4).has_value(), "an unbound page has no device address");
}

void testMakeResidentBindsCoveringChunks() {
    FakeDevice device;
    Residency residency(device, 0);
    residency.Add(0x10008000, 0x30000);
    const auto made = residency.MakeResident(0x1000f000, 0x2000, 1);
    Expect(made.size() == 1 && made[0].address == 0x10008000 && made[0].size == 0x18000, "the first resident span starts at the range and ends at the second chunk");
    Expect(device.bound.size() == 2 && device.pools.size() == 1, "two chunks are bound from one pool");
    const auto again = residency.MakeResident(0x10010000, 0x100, 2);
    Expect(again.empty(), "a bound chunk is not bound again");
    const auto span = residency.Translate(0x10009000, 0x10);
    Expect(span && span->offset == 0x10009000 && span->address == (std::uint64_t{1} << 40u) + 0x10009000, "a resident page translates to the mirror base plus its guest address");
    Expect(!residency.Translate(0x1001f000, 0x2000).has_value(), "a span reaching an unbound chunk does not translate");
    const auto tail = residency.MakeResident(0x10030000, 0x10000, 3);
    Expect(tail.size() == 1 && tail[0].address == 0x10030000 && tail[0].size == 0x8000, "the last span ends at the range");
    Expect(residency.Counts().boundBytes == 3 * ChunkBytes, "bound bytes are counted");
}

void testEvictionTakesTheOldestRange() {
    FakeDevice device;
    Residency residency(device, 2 * ChunkBytes);
    residency.Add(0x20000000, 2 * ChunkBytes);
    residency.Add(0x30000000, 2 * ChunkBytes);
    residency.MakeResident(0x20000000, 2 * ChunkBytes, 1);
    residency.MakeResident(0x30000000, 2 * ChunkBytes, 2);
    const auto none = residency.Evict(2);
    Expect(none.size() == 1 && none[0].address == 0x20000000 && none[0].size == 2 * ChunkBytes, "the range used least recently is evicted");
    Expect(!residency.Translate(0x20000000, 4).has_value() && residency.Translate(0x30000000, 4).has_value(), "eviction unbinds only the evicted range");
    Expect(residency.Evict(3).empty(), "nothing is evicted within the budget");
    residency.MakeResident(0x20000000, ChunkBytes, 3);
    Expect(residency.Evict(3).size() == 1 && residency.Translate(0x20000000, 4).has_value(), "a range in use this frame is kept");
    Expect(residency.Counts().evicted == 2, "evictions are counted");
}

void testEvictionKeepsVetoedRanges() {
    FakeDevice device;
    Residency residency(device, ChunkBytes);
    residency.Add(0x20000000, ChunkBytes);
    residency.Add(0x30000000, ChunkBytes);
    residency.MakeResident(0x20000000, ChunkBytes, 1);
    residency.MakeResident(0x30000000, ChunkBytes, 2);
    residency.SetKeep([](std::uint64_t address, std::uint64_t) { return address == 0x20000000; });
    const auto evicted = residency.Evict(3);
    Expect(evicted.size() == 1 && evicted[0].address == 0x30000000, "a range the veto keeps is passed over for a newer one");
    Expect(residency.Translate(0x20000000, 4).has_value(), "the vetoed range stays bound");
    residency.SetKeep([](std::uint64_t, std::uint64_t) { return true; });
    residency.MakeResident(0x30000000, ChunkBytes, 3);
    Expect(residency.Evict(4).empty() && residency.Counts().boundBytes == 2 * ChunkBytes, "nothing is evicted when every range is vetoed");
}

void testRemoveReturnsChunks() {
    FakeDevice device;
    device.poolLimit = 1;
    Residency residency(device, 0);
    residency.Add(0x40000000, PoolBytes);
    Expect(residency.MakeResident(0x40000000, PoolBytes, 1).size() == 1, "a whole pool is bound");
    residency.Add(0x60000000, ChunkBytes);
    Expect(residency.MakeResident(0x60000000, ChunkBytes, 1).empty() && residency.Counts().refused == 1, "binding past the pools is refused");
    residency.Remove(0x40000000);
    Expect(device.bound.empty() && residency.Counts().boundBytes == 0, "removing a range unbinds it");
    Expect(residency.MakeResident(0x60000000, ChunkBytes, 2).size() == 1, "removed chunks are reused");
}

void testFailedBindKeepsChunksFree() {
    FakeDevice device;
    Residency residency(device, 0);
    residency.Add(0x50000000, ChunkBytes);
    device.failBind = true;
    Expect(residency.MakeResident(0x50000000, ChunkBytes, 1).empty(), "a failed bind makes nothing resident");
    device.failBind = false;
    Expect(residency.MakeResident(0x50000000, ChunkBytes, 1).size() == 1 && residency.Counts().boundBytes == ChunkBytes, "the chunks of a failed bind are bound later");
}

struct GuestBlock {
    std::vector<std::byte> storage = std::vector<std::byte>(9 * ChunkBytes);
    std::uint64_t base = Aligned();
    std::byte* At(std::uint64_t offset) { return reinterpret_cast<std::byte*>(base + offset); }
    std::uint64_t Mirror(std::uint64_t offset) const { return base + offset - MirrorSegmentBase(base + offset); }
    std::uint64_t Aligned() const {
        const auto first = (reinterpret_cast<std::uint64_t>(storage.data()) + ChunkBytes - 1) & ~(ChunkBytes - 1);
        return MirrorSegmentBase(first) == MirrorSegmentBase(first + 4 * ChunkBytes - 1) ? first : first + 4 * ChunkBytes;
    }
};

void testUploadsFollowResidency() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency);
    GuestBlock guest;
    for (std::uint64_t at = 0; at < 3 * ChunkBytes; ++at) *guest.At(at) = static_cast<std::byte>(at * 7);
    residency.Add(guest.base, 3 * ChunkBytes);
    const std::vector<DirtyRange> early{{guest.base, 0x1000}};
    uploads.NoteDirty(early);
    Expect(uploads.Take().empty() && uploads.Counts().skippedBytes == 0x1000, "a dirty page that is not resident is not uploaded");
    Expect(uploads.Prepare(guest.base + 0x1000, 0x100, 1), "preparing a span makes it resident");
    auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].offset == guest.Mirror(0) && taken[0].bytes.size() == ChunkBytes && std::memcmp(taken[0].bytes.data(), guest.At(0), ChunkBytes) == 0, "a newly resident chunk is uploaded in full");
    std::memset(guest.At(0x2000), 0x5a, 0x1000);
    const std::vector<DirtyRange> dirty{{guest.base + 0x2000, 0x1000}, {guest.base + ChunkBytes, 0x1000}};
    uploads.NoteDirty(dirty);
    taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].offset == guest.Mirror(0x2000) && taken[0].bytes.size() == 0x1000 && taken[0].bytes[0] == std::byte{0x5a}, "a dirty resident page is uploaded alone");
    Expect(uploads.Prepare(guest.base, 3 * ChunkBytes, 2), "the rest of the range is made resident");
    taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].offset == guest.Mirror(ChunkBytes) && taken[0].bytes.size() == 2 * ChunkBytes, "only the newly bound chunks are uploaded, as one span");
    Expect(!uploads.Prepare(guest.base + 2 * ChunkBytes, 2 * ChunkBytes, 2), "a span past the range is refused");
}

void testDirtyRangesAcrossRangeEdgesUploadTheirResidentPart() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency);
    GuestBlock guest;
    residency.Add(guest.base + 0x2000, 0x2000);
    Expect(uploads.Prepare(guest.base + 0x2000, 0x2000, 1), "a range inside one chunk is made resident");
    static_cast<void>(uploads.Take());
    std::memset(guest.At(0x1000), 0x11, 0x4000);
    const std::vector<DirtyRange> dirty{{guest.base + 0x1000, 0x4000}};
    uploads.NoteDirty(dirty);
    const auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].guest == guest.base + 0x2000 && taken[0].offset == guest.Mirror(0x2000) && taken[0].bytes.size() == 0x2000 && taken[0].bytes[0] == std::byte{0x11}, "a dirty range wider than the served range uploads the served part");
    Expect(uploads.Counts().skippedBytes == 0x2000, "only the pages outside the served range are skipped");
}

void testEvictionDropsPendingUploads() {
    FakeDevice device;
    Residency residency(device, ChunkBytes);
    ResidentUploads uploads(residency);
    GuestBlock guest;
    residency.Add(guest.base, ChunkBytes);
    residency.Add(guest.base + 2 * ChunkBytes, ChunkBytes);
    uploads.Prepare(guest.base, ChunkBytes, 1);
    uploads.Take();
    const std::vector<DirtyRange> dirty{{guest.base, 0x1000}};
    uploads.NoteDirty(dirty);
    uploads.Prepare(guest.base + 2 * ChunkBytes, ChunkBytes, 2);
    const auto evicted = uploads.Evict(2);
    Expect(evicted.size() == 1 && evicted[0].address == guest.base, "the older range is evicted");
    const auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].guest == guest.base + 2 * ChunkBytes, "uploads into an evicted range are dropped");
    Expect(uploads.Counts().evictedRanges == 1, "evicted ranges are counted");
}

void testServedRangeReplacesUnboundRanges() {
    FakeDevice device;
    Residency residency(device, 0);
    residency.Add(0x70010000, 0x1000);
    residency.Add(0x70030000, 0x2000);
    residency.Add(0x6fff0000, 0x20000);
    Expect(residency.RemoveUnboundInside(0x70000000, 0x100000) == 2, "unbound ranges inside are removed");
    Expect(residency.Owns(0x6fff0000, 0x20000), "a range reaching outside is kept");
    residency.Remove(0x6fff0000);
    Expect(residency.Add(0x70000000, 0x100000), "the whole range is added once the ranges inside are gone");
    residency.MakeResident(0x70000000, ChunkBytes, 1);
    residency.Add(0x80000000, ChunkBytes);
    residency.MakeResident(0x80000000, ChunkBytes, 1);
    Expect(residency.RemoveUnboundInside(0x70000000, 0x20000000) == 0, "bound ranges are kept");
    const auto base = residency.Base(0x70008000);
    Expect(base && base->offset == 0x70008000 && base->bytes == 0xf8000 && base->address == (std::uint64_t{1} << 40u) + 0x70008000, "the base of an address does not need its chunk bound");
    Expect(residency.Base(0x70020000).has_value() && !residency.Translate(0x70020000, 4).has_value(), "an unbound page has a base but no translation");
    residency.SetBudget(ChunkBytes);
    residency.Touch(0x70000000, 5);
    const auto evicted = residency.Evict(5);
    Expect(evicted.size() == 1 && evicted[0].address == 0x80000000, "a touched range is kept over an older one");
}

void testRemovedRangesKeepTheMirror() {
    FakeDevice device;
    Residency residency(device, 0);
    residency.Add(0x70010000, 0x1000);
    residency.Add(0x90000000, ChunkBytes);
    residency.MakeResident(0x90000000, ChunkBytes, 1);
    const auto before = residency.Translate(0x90000000, 4);
    Expect(residency.RemoveUnboundInside(0x70000000, 0x100000) == 1, "an unbound range inside is removed");
    residency.Remove(0x90000000);
    Expect(device.buffers.size() == 1 && device.bound.empty() && residency.Removals() == 2 && residency.Counts().segments == 1, "removed ranges unbind their memory and keep the mirror buffer");
    Expect(!residency.Translate(0x90000000, 4).has_value() && residency.MirrorAddress(0x90000000) == (std::uint64_t{1} << 40u) + 0x90000000, "a removed range has no translation, its mirror address stays");
    residency.Add(0x90000000, ChunkBytes);
    residency.MakeResident(0x90000000, ChunkBytes, 2);
    const auto after = residency.Translate(0x90000000, 4);
    Expect(before && after && before->address == after->address && before->handle == after->handle, "a range added again at an address has the device address it had");
    Expect(residency.Add(std::uint64_t{3} << MirrorSegmentBits, ChunkBytes) && device.buffers.size() == 1, "a range elsewhere below 960 GiB uses the same window buffer");
    Expect(residency.Add(MirrorWindowBytes + (std::uint64_t{3} << MirrorSegmentBits), ChunkBytes) && device.buffers.size() == 2, "a range above the window gets the buffer of its 64 GiB segment");
    Expect(!residency.Add(MirrorWindowBytes - ChunkBytes, 2 * ChunkBytes) && residency.Counts().refused == 1, "a range across the end of the window is refused");
}

void testSpansAcrossAdjacentRangesTranslate() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency);
    GuestBlock guest;
    residency.Add(guest.base, ChunkBytes + 0x8000);
    residency.Add(guest.base + ChunkBytes + 0x8000, 0x8000);
    residency.Add(guest.base + 3 * ChunkBytes, ChunkBytes);
    Expect(uploads.Prepare(guest.base, ChunkBytes + 0x8000, 1), "the first range is made resident");
    static_cast<void>(uploads.Take());
    Expect(!residency.Translate(guest.base + ChunkBytes, ChunkBytes).has_value(), "a span reaching a range that is not resident does not translate");
    std::memset(guest.At(ChunkBytes + 0x8000), 0x77, 0x8000);
    Expect(uploads.Prepare(guest.base + ChunkBytes + 0x8000, 0x8000, 1), "the second range joins the chunk the first one bound");
    const auto taken = uploads.Take();
    Expect(device.bound.size() == 2 && residency.Counts().sharedChunks == 1 && residency.Counts().boundBytes == 2 * ChunkBytes, "a chunk two ranges share is bound once");
    Expect(taken.size() == 1 && taken[0].guest == guest.base + ChunkBytes + 0x8000 && taken[0].offset == guest.Mirror(ChunkBytes + 0x8000) && taken[0].bytes.size() == 0x8000 && taken[0].bytes[0] == std::byte{0x77}, "the joining range uploads its own part of the shared chunk");
    const auto span = residency.Translate(guest.base + 0x4000, 2 * ChunkBytes - 0x4000);
    Expect(span && span->offset == guest.Mirror(0x4000) && span->bytes == 2 * ChunkBytes - 0x4000, "a span over two adjacent resident ranges is one device range");
    uploads.Prepare(guest.base + 3 * ChunkBytes, ChunkBytes, 1);
    Expect(!residency.Translate(guest.base + ChunkBytes, 3 * ChunkBytes).has_value(), "a span over a gap between ranges does not translate");
    residency.Remove(guest.base);
    Expect(device.bound.size() == 2 && residency.Translate(guest.base + ChunkBytes + 0x8000, 0x8000).has_value(), "removing one holder keeps the shared chunk bound for the other");
    residency.Remove(guest.base + ChunkBytes + 0x8000);
    Expect(device.bound.size() == 1, "the shared chunk is unbound with its last holder");
}

void testCoverFillsTheGapsBetweenRanges() {
    FakeDevice device;
    Residency residency(device, 0);
    residency.Add(0x60010000, 0x8000);
    residency.Add(0x60030000, 0x10000);
    residency.MakeResident(0x60010000, 0x8000, 1);
    Expect(!residency.Add(0x60014000, 0x30000), "a span that partly overlaps a bound range is not added as one range");
    Expect(residency.Covering(0x60014000, 0x30000).empty(), "a span with gaps between ranges is not covered");
    Expect(residency.Cover(0x60014000, 0x30000), "the gaps of a span become ranges");
    const auto pieces = residency.Covering(0x60014000, 0x30000);
    Expect(pieces.size() == 4 && pieces[0].address == 0x60014000 && pieces[0].size == 0x4000 && pieces[1].address == 0x60018000 && pieces[1].size == 0x18000 && pieces[2].address == 0x60030000 && pieces[2].size == 0x10000 && pieces[3].address == 0x60040000 && pieces[3].size == 0x4000, "a covered span lists its part of each range in order");
    Expect(residency.Counts().ranges == 4 && residency.Counts().gapRanges == 2, "two gap ranges join the two ranges that were there");
    Expect(residency.Owns(0x60010000, 0x8000) && residency.Owns(0x60030000, 0x10000) && residency.Translate(0x60010000, 0x8000).has_value(), "the ranges that were there keep their bytes and their memory");
    Expect(residency.Cover(0x60014000, 0x30000) && residency.Counts().gapRanges == 2, "covering a covered span adds nothing");
    Expect(!residency.Cover(MirrorWindowBytes - ChunkBytes, 2 * ChunkBytes), "a span across the end of the window is not covered");
}

void testSpansOverSeveralRangesAreMadeResidentTogether() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency);
    GuestBlock guest;
    for (std::uint64_t at = 0; at < 4 * ChunkBytes; ++at) *guest.At(at) = static_cast<std::byte>(at * 13 + (at >> 16u));
    residency.Add(guest.base, ChunkBytes + 0x4000);
    Expect(uploads.Prepare(guest.base, ChunkBytes + 0x4000, 1), "the first range is made resident");
    static_cast<void>(uploads.Take());
    Expect(!uploads.Prepare(guest.base + 0x8000, 3 * ChunkBytes, 1), "a span reaching past its range is refused until it is covered");
    Expect(residency.Cover(guest.base + 0x8000, 3 * ChunkBytes), "the rest of the span becomes a range");
    Expect(uploads.Prepare(guest.base + 0x8000, 3 * ChunkBytes, 2), "a span over two ranges is made resident");
    const auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].guest == guest.base + ChunkBytes + 0x4000 && taken[0].bytes.size() == 2 * ChunkBytes + 0x4000 && std::memcmp(taken[0].bytes.data(), guest.At(ChunkBytes + 0x4000), 2 * ChunkBytes + 0x4000) == 0, "only the bytes of the new range are uploaded");
    const auto span = residency.Translate(guest.base + 0x8000, 3 * ChunkBytes);
    Expect(span && span->offset == guest.Mirror(0x8000) && span->bytes == 3 * ChunkBytes, "the span is one device range");
    Expect(device.bound.size() == 4 && residency.Counts().sharedChunks == 1, "the chunk both ranges share is bound once");
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> committedHalf(std::uint64_t address, std::size_t bytes, bool) {
    return {{address, address + bytes / 2}};
}

void testFullUploadsReadCommittedPagesOnly() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency, committedHalf);
    GuestBlock guest;
    std::memset(guest.At(0), 0x33, ChunkBytes);
    residency.Add(guest.base, ChunkBytes);
    uploads.Prepare(guest.base, ChunkBytes, 1);
    const auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].bytes[0] == std::byte{0x33} && taken[0].bytes[ChunkBytes / 2] == std::byte{0}, "uncommitted pages of a full upload read as zeros");
}

void testDirtyUploadsSkipUnreadablePages() {
    FakeDevice device;
    Residency residency(device, 0);
    ResidentUploads uploads(residency, committedHalf);
    GuestBlock guest;
    residency.Add(guest.base, ChunkBytes);
    uploads.Prepare(guest.base, ChunkBytes, 1);
    static_cast<void>(uploads.Take());
    std::memset(guest.At(0), 0x44, ChunkBytes / 2);
    const std::vector<DirtyRange> dirty{{guest.base, ChunkBytes}};
    uploads.NoteDirty(dirty);
    const auto taken = uploads.Take();
    Expect(taken.size() == 1 && taken[0].guest == guest.base && taken[0].offset == guest.Mirror(0) && taken[0].bytes.size() == ChunkBytes / 2 && taken[0].bytes[0] == std::byte{0x44}, "a dirty range uploads only its readable pages");
    Expect(uploads.Counts().skippedBytes == ChunkBytes / 2, "the unreadable pages of a dirty range are skipped, not zeroed");
}

}

int main() {
    testServedRangeReplacesUnboundRanges();
    testFullUploadsReadCommittedPagesOnly();
    testDirtyUploadsSkipUnreadablePages();
    testUploadsFollowResidency();
    testDirtyRangesAcrossRangeEdgesUploadTheirResidentPart();
    testEvictionDropsPendingUploads();
    testAddBindsNothing();
    testMakeResidentBindsCoveringChunks();
    testEvictionTakesTheOldestRange();
    testEvictionKeepsVetoedRanges();
    testRemoveReturnsChunks();
    testFailedBindKeepsChunksFree();
    testRemovedRangesKeepTheMirror();
    testSpansAcrossAdjacentRangesTranslate();
    testCoverFillsTheGapsBetweenRanges();
    testSpansOverSeveralRangesAreMadeResidentTogether();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified residency tests passed\n");
    return 0;
}
