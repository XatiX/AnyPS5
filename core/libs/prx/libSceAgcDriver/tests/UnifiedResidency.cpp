#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include <cstdio>
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
    Expect(device.buffers.size() == 1 && device.buffers.begin()->second == 0x40000, "the device buffer covers the range in whole chunks");
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
    Expect(span && span->offset == 0x9000 && span->address == (std::uint64_t{1} << 40u) + 0x9000, "a resident page translates to its device address");
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

}

int main() {
    testAddBindsNothing();
    testMakeResidentBindsCoveringChunks();
    testEvictionTakesTheOldestRange();
    testRemoveReturnsChunks();
    testFailedBindKeepsChunksFree();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified residency tests passed\n");
    return 0;
}
