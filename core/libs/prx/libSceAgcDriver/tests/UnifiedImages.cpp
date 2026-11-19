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
    std::uint64_t View(const ViewKey& view) override { return 0x100u + view.baseLevel; }
    std::uint32_t Layout() const override { return 1u; }
};

class FakeDevice final : public SurfaceDevice {
public:
    std::uint32_t created = 0;
    bool refuse = false;
    std::shared_ptr<DeviceSurface> Create(const SurfaceKey&, std::string& reason) override {
        if (refuse) {
            reason = "refused";
            return nullptr;
        }
        ++created;
        return std::make_shared<FakeSurface>();
    }
};

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
    Events::ChangeLog images;
    SurfaceTable table(device, shared, images);
    std::string reason;
    const auto first = table.Find(Key(0x200000000ull), reason);
    const auto second = table.Find(Key(0x200000000ull), reason);
    Expect(first.has_value() && second.has_value() && first->surface == second->surface, "one surface per key");
    Expect(!first->ready && device.created == 1u, "a new surface is pending and created once");
    auto work = table.TakeWork();
    Expect(work.size() == 1u && table.TakeWork().empty(), "a new surface is queued once");
    table.MarkReady(*work.front(), shared.Serial(), images.Serial());
    const auto ready = table.Find(Key(0x200000000ull), reason);
    Expect(ready.has_value() && ready->ready, "a detiled surface is ready");
}

void testWritesMakeSurfacesStale() {
    FakeDevice device;
    Events::ChangeLog shared;
    Events::ChangeLog images;
    SurfaceTable table(device, shared, images);
    std::string reason;
    const auto key = Key(0x300000000ull);
    static_cast<void>(table.Find(key, reason));
    auto work = table.TakeWork();
    table.MarkReady(*work.front(), shared.Serial(), images.Serial());
    static_cast<void>(shared.Note(0x400000000ull, 0x1000));
    Expect(table.Find(key, reason)->ready, "a write elsewhere leaves the surface ready");
    static_cast<void>(shared.Note(key.address + 0x2000u, 0x100));
    const auto stale = table.Find(key, reason);
    Expect(stale.has_value() && !stale->ready && stale->surface->state == State::Stale, "a write inside the surface makes it stale");
    static_cast<void>(table.Find(key, reason));
    Expect(table.TakeWork().size() == 1u, "a stale surface is queued once");
    const auto counts = table.TakeCounts();
    Expect(counts.created == 1u && counts.stale == 1u && counts.detiled == 1u, "counts follow the surface");
}

void testRefusals() {
    FakeDevice device;
    device.refuse = true;
    Events::ChangeLog shared;
    Events::ChangeLog images;
    SurfaceTable table(device, shared, images);
    std::string reason;
    Expect(!table.Find(Key(0x500000000ull), reason).has_value() && reason == "refused", "a surface the device refuses is not kept");
    Expect(table.Size() == 0u, "nothing is stored for a refusal");
    const std::array<std::uint32_t, 8> empty{};
    Expect(!KeyOf(empty, reason).has_value(), "an empty T# has no key");
}

}

int main() {
    testSurfacesAreSharedAndQueued();
    testWritesMakeSurfacesStale();
    testRefusals();
    if (failures != 0) return 1;
    std::printf("unified images tests passed\n");
    return 0;
}
