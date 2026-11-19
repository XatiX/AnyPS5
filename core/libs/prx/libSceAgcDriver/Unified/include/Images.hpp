#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGES_HPP

#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified::Images {

struct SurfaceKey {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::uint32_t format = 0;
    std::uint32_t tileMode = 0;
    std::uint32_t dimension = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t mips = 0;
    std::uint32_t pipeBankXor = 0;
    std::uint32_t pitch = 0;
    bool operator==(const SurfaceKey&) const = default;
};

struct ViewKey {
    std::uint32_t baseLevel = 0;
    std::uint32_t lastLevel = 0;
    std::uint32_t baseArray = 0;
    std::uint32_t lastArray = 0;
    std::uint32_t swizzle = 0;
    bool operator==(const ViewKey&) const = default;
};

struct ImageKey {
    SurfaceKey surface;
    ViewKey view;
};

[[nodiscard]] std::optional<ImageKey> KeyOf(std::span<const std::uint32_t, 8> words, std::string& reason);

enum class State : std::uint8_t { Pending, Ready, Stale };

class DeviceSurface {
public:
    virtual ~DeviceSurface() = default;
    [[nodiscard]] virtual std::uint64_t View(const ViewKey& view, std::uint32_t kind, std::string& reason) = 0;
    [[nodiscard]] virtual std::uint32_t Layout() const = 0;
};

class SurfaceDevice {
public:
    virtual ~SurfaceDevice() = default;
    [[nodiscard]] virtual std::shared_ptr<DeviceSurface> Create(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason) = 0;
};

struct Surface {
    SurfaceKey key;
    std::shared_ptr<DeviceSurface> device;
    State state = State::Pending;
    std::uint64_t stamp = 0;
    std::uint64_t imageStamp = 0;
};

struct Found {
    std::shared_ptr<Surface> surface;
    bool ready = false;
    bool gpuWritten = false;
};

struct TableCounts {
    std::uint64_t lookups = 0;
    std::uint64_t created = 0;
    std::uint64_t ready = 0;
    std::uint64_t pending = 0;
    std::uint64_t stale = 0;
    std::uint64_t refused = 0;
    std::uint64_t detiled = 0;
    std::uint64_t gpuWritten = 0;
};

class SurfaceTable {
public:
    SurfaceTable(SurfaceDevice& device, const Events::ChangeLog& shared, const Events::ChangeLog& images) : device(device), shared(shared), images(images) {}

    [[nodiscard]] std::optional<Found> Find(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason);
    [[nodiscard]] std::vector<std::shared_ptr<Surface>> TakeWork();
    void MarkReady(Surface& surface, std::uint64_t stamp, std::uint64_t imageStamp);
    void Requeue(const std::shared_ptr<Surface>& surface);
    [[nodiscard]] TableCounts TakeCounts();
    [[nodiscard]] std::size_t Size() const;

private:
    struct KeyHash {
        std::size_t operator()(const SurfaceKey& key) const;
    };

    SurfaceDevice& device;
    const Events::ChangeLog& shared;
    const Events::ChangeLog& images;
    mutable std::mutex mutex;
    std::unordered_map<SurfaceKey, std::shared_ptr<Surface>, KeyHash> surfaces;
    std::vector<std::shared_ptr<Surface>> work;
    TableCounts counts;
};

}

#endif
