#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_IMAGES_HPP

#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {
struct GuestTextureResource;
}

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
    std::uint32_t format = 0;
    bool operator==(const ViewKey&) const = default;
};

struct ImageKey {
    SurfaceKey surface;
    ViewKey view;
};

[[nodiscard]] std::optional<ImageKey> KeyOf(std::span<const std::uint32_t, 8> words, std::string& reason);
[[nodiscard]] std::optional<ImageKey> KeyOf(const Graphics::GuestTextureResource& resource, std::string& reason);
[[nodiscard]] bool SameBacking(const SurfaceKey& left, const SurfaceKey& right);
[[nodiscard]] bool CompressedSiblings(std::uint32_t left, std::uint32_t right);
[[nodiscard]] bool OwnersEnabled();
[[nodiscard]] bool OwnsClass(std::string_view name);

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
    [[nodiscard]] virtual std::shared_ptr<DeviceSurface> CreateFromResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason);
};

struct Surface {
    SurfaceKey key;
    std::shared_ptr<DeviceSurface> device;
    State state = State::Pending;
    std::uint64_t stamp = 0;
    std::uint64_t imageStamp = 0;
    std::uint64_t used = 0;
    bool retired = false;
    bool gpuNewer = false;
    bool seeded = false;
    bool mirrorStale = false;
};

[[nodiscard]] bool MetadataRecords();

[[nodiscard]] inline std::uintptr_t OwnerOf(const Surface& surface) {
    return Events::ResourceOwner(&surface);
}

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
    std::uint64_t evicted = 0;
    std::uint64_t evictionsKept = 0;
    std::uint64_t ownWrites = 0;
    std::uint64_t ownServed = 0;
    std::uint64_t aliasServed = 0;
    std::uint64_t retired = 0;
};

class SurfaceTable {
public:
    SurfaceTable(SurfaceDevice& device, const Events::ChangeLog& shared, const Events::ChangeLog& gpuWrites, std::uint64_t budgetBytes = 0) : device(device), shared(shared), gpuWrites(gpuWrites), budget(budgetBytes) {}

    [[nodiscard]] std::optional<Found> Find(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason);
    [[nodiscard]] std::optional<Found> FindResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason);
    [[nodiscard]] std::vector<std::shared_ptr<Surface>> TakeWork();
    void MarkReady(Surface& surface, std::uint64_t stamp, std::uint64_t imageStamp);
    [[nodiscard]] std::shared_ptr<Surface> Create(const SurfaceKey& key, std::span<const std::uint32_t, 8> words, std::string& reason);
    [[nodiscard]] std::shared_ptr<Surface> CreateResource(const SurfaceKey& key, const Graphics::GuestTextureResource& resource, std::string& reason);
    void MarkGpuWritten(Surface& surface);
    void ClearGpuNewer(Surface& surface);
    void MarkWrittenBack(Surface& surface);
    [[nodiscard]] std::vector<std::shared_ptr<Surface>> StaleOverlapping(std::uint64_t address, std::uint64_t bytes) const;
    [[nodiscard]] bool ChangedSinceWrite(const Surface& surface) const;
    [[nodiscard]] std::string ChangeOf(const Surface& surface) const;
    bool RefreshIfChanged(const std::shared_ptr<Surface>& surface);
    void Touch(Surface& surface);
    [[nodiscard]] std::vector<std::shared_ptr<Surface>> Overlapping(std::uint64_t address, std::uint64_t bytes) const;
    void Requeue(const std::shared_ptr<Surface>& surface);
    [[nodiscard]] bool Holds(std::uint64_t address, std::uint64_t bytes) const;
    void NoteRefused(std::string_view reason);
    void Retire(const std::shared_ptr<Surface>& surface);
    [[nodiscard]] std::shared_ptr<Surface> Seed(const std::shared_ptr<Surface>& surface);
    [[nodiscard]] std::string OwnersLine() const;
    [[nodiscard]] TableCounts TakeCounts();
    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] std::uint64_t HeldBytes() const;

private:
    struct KeyHash {
        std::size_t operator()(const SurfaceKey& key) const;
    };

    void evict();
    void dropStale(Surface& surface);
    void indexSurface(const std::shared_ptr<Surface>& surface);
    void unindexSurface(const Surface& surface);
    template<typename TVisit>
    bool visitOverlapping(std::uint64_t address, std::uint64_t bytes, TVisit&& visit) const;
    template<typename TCreate>
    std::optional<Found> findWith(const SurfaceKey& key, TCreate&& create, std::string& reason);
    template<typename TCreate>
    std::shared_ptr<Surface> createWith(const SurfaceKey& key, TCreate&& create, std::string& reason);

    SurfaceDevice& device;
    const Events::ChangeLog& shared;
    const Events::ChangeLog& gpuWrites;
    mutable std::mutex mutex;
    std::unordered_map<SurfaceKey, std::shared_ptr<Surface>, KeyHash> surfaces;
    std::multimap<std::uint64_t, std::shared_ptr<Surface>> byAddress;
    std::uint64_t widest = 0;
    std::vector<std::shared_ptr<Surface>> work;
    std::vector<std::shared_ptr<Surface>> newer;
    std::vector<std::pair<std::string, std::uint64_t>> refusals;
    std::uint64_t refusedImages = 0;
    mutable std::mutex staleMutex;
    std::map<std::uint64_t, std::shared_ptr<Surface>> stale;
    std::uint64_t staleSpan = 0;
    TableCounts counts;
    std::uint64_t budget = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t clock = 0;
};

}

#endif
