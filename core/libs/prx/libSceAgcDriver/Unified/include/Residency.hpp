#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCY_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr std::uint64_t ChunkBytes = 0x10000;
inline constexpr std::uint64_t PoolBytes = 0x10000000;
inline constexpr std::uint32_t PoolChunks = static_cast<std::uint32_t>(PoolBytes / ChunkBytes);
inline constexpr unsigned MirrorSegmentBits = 36;
inline constexpr std::uint64_t MirrorSegmentBytes = std::uint64_t{1} << MirrorSegmentBits;

struct Chunk {
    std::uint32_t pool = 0;
    std::uint32_t index = 0;
};

struct ChunkBind {
    std::uint64_t offset = 0;
    Chunk chunk;
    bool bound = false;
};

struct DeviceBuffer {
    std::uint64_t handle = 0;
    std::uint64_t address = 0;
};

class ResidencyDevice {
public:
    virtual ~ResidencyDevice() = default;
    virtual bool CreatePool(std::uint32_t pool) = 0;
    virtual void DestroyPool(std::uint32_t pool) = 0;
    virtual std::optional<DeviceBuffer> CreateBuffer(std::uint64_t bytes) = 0;
    virtual void DestroyBuffer(std::uint64_t handle) = 0;
    virtual bool Bind(std::uint64_t handle, std::span<const ChunkBind> binds) = 0;
};

struct GuestSpan {
    std::uint64_t address = 0;
    std::uint64_t size = 0;
};

struct DeviceSpan {
    std::uint64_t handle = 0;
    std::uint64_t offset = 0;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

struct ResidentPiece {
    std::uint64_t guest = 0;
    DeviceSpan device;
};

struct ResidencyCounts {
    std::uint64_t ranges = 0;
    std::uint64_t pools = 0;
    std::uint64_t boundBytes = 0;
    std::uint64_t budget = 0;
    std::uint64_t bound = 0;
    std::uint64_t evicted = 0;
    std::uint64_t refused = 0;
    std::uint64_t sharedPages = 0;
    std::uint64_t segments = 0;
    std::uint64_t removals = 0;
    std::uint64_t sharedChunks = 0;
    std::uint64_t gapRanges = 0;
};

class GpuPageTable;

class Residency {
public:
    using Keep = bool (*)(std::uint64_t address, std::uint64_t size);

    Residency(ResidencyDevice& device, std::uint64_t budget);
    ~Residency();
    Residency(const Residency&) = delete;
    Residency& operator=(const Residency&) = delete;

    bool Add(std::uint64_t address, std::uint64_t size);
    bool Cover(std::uint64_t address, std::uint64_t size, std::vector<GuestSpan>* gaps = nullptr);
    void Remove(std::uint64_t address);
    [[nodiscard]] bool Owns(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::vector<GuestSpan> Covering(std::uint64_t address, std::uint64_t size) const;
    std::vector<GuestSpan> MakeResident(std::uint64_t address, std::uint64_t size, std::uint64_t frame);
    [[nodiscard]] std::optional<DeviceSpan> Translate(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::vector<ResidentPiece> ResidentPieces(std::uint64_t address, std::uint64_t size) const;
    [[nodiscard]] std::optional<DeviceSpan> Base(std::uint64_t address) const;
    [[nodiscard]] std::uint64_t MirrorAddress(std::uint64_t address) const;
    void Touch(std::uint64_t address, std::uint64_t frame);
    std::size_t RemoveUnboundInside(std::uint64_t address, std::uint64_t size);
    std::vector<GuestSpan> Evict(std::uint64_t frame);
    [[nodiscard]] std::uint64_t Removals() const;
    void SetBudget(std::uint64_t bytes);
    void SetKeep(Keep next);
    void SetPageTable(GpuPageTable* table);
    [[nodiscard]] ResidencyCounts Counts() const;

private:
    struct Range {
        std::uint64_t end = 0;
        std::uint64_t origin = 0;
        std::uint64_t lastUse = 0;
        std::uint64_t heldChunks = 0;
        std::vector<bool> held;
    };
    using Ranges = std::map<std::uint64_t, Range>;
    struct Held {
        Chunk chunk;
        std::uint32_t holders = 0;
    };

    Ranges::iterator rangeOf(std::uint64_t address);
    Ranges::const_iterator rangeOf(std::uint64_t address) const;
    bool add(std::uint64_t address, std::uint64_t size);
    const DeviceBuffer* segmentOf(std::uint64_t address) const;
    const DeviceBuffer* segmentFor(std::uint64_t address);
    std::optional<Chunk> take();
    void put(Chunk chunk);
    std::uint64_t release(Range& range);
    void mapChunk(std::uint64_t chunk, std::uint64_t deviceAddress);
    void unmapChunk(std::uint64_t chunk, std::uint64_t deviceAddress);

    ResidencyDevice& device;
    GpuPageTable* pageTable = nullptr;
    Keep keep = nullptr;
    std::uint64_t sharedPages = 0;
    mutable std::mutex mutex;
    Ranges ranges;
    std::map<std::uint64_t, DeviceBuffer> segments;
    std::unordered_map<std::uint64_t, Held> bound;
    std::vector<Chunk> free;
    std::uint32_t pools = 0;
    std::uint64_t budget = 0;
    std::uint64_t boundBytes = 0;
    std::uint64_t boundCount = 0;
    std::uint64_t evictedCount = 0;
    std::uint64_t refusedCount = 0;
    std::uint64_t removals = 0;
    std::uint64_t sharedChunks = 0;
    std::uint64_t gapRanges = 0;
};

}

#endif
