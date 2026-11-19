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

}

int main() {
    testGpuWrittenSurfacesAreLeftToTheExistingCache();
    testImageSetNotesAreNotGpuWrites();
    testSurfacesAreSharedAndQueued();
    testWritesMakeSurfacesStale();
    testRefusals();
    if (failures != 0) return 1;
    std::printf("unified images tests passed\n");
    return 0;
}
