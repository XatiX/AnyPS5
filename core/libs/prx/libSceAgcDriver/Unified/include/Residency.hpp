#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENCY_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver::Unified {

inline constexpr std::uint64_t ChunkBytes = 0x10000;
inline constexpr std::uint64_t PoolBytes = 0x10000000;
inline constexpr std::uint32_t PoolChunks = static_cast<std::uint32_t>(PoolBytes / ChunkBytes);

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

struct ResidencyCounts {
    std::uint64_t ranges = 0;
    std::uint64_t pools = 0;
    std::uint64_t boundBytes = 0;
    std::uint64_t budget = 0;
    std::uint64_t bound = 0;
    std::uint64_t evicted = 0;
    std::uint64_t refused = 0;
};

class Residency {
public:
    Residency(ResidencyDevice& device, std::uint64_t budget);
    ~Residency();
    Residency(const Residency&) = delete;
    Residency& operator=(const Residency&) = delete;

    bool Add(std::uint64_t address, std::uint64_t size);
    void Remove(std::uint64_t address);
    [[nodiscard]] bool Owns(std::uint64_t address, std::uint64_t size) const;
    std::vector<GuestSpan> MakeResident(std::uint64_t address, std::uint64_t size, std::uint64_t frame);
    [[nodiscard]] std::optional<DeviceSpan> Translate(std::uint64_t address, std::uint64_t size) const;
    std::vector<GuestSpan> Evict(std::uint64_t frame);
    void SetBudget(std::uint64_t bytes);
    [[nodiscard]] ResidencyCounts Counts() const;

private:
    struct Range {
        std::uint64_t end = 0;
        DeviceBuffer buffer;
        std::uint64_t origin = 0;
        std::uint64_t lastUse = 0;
        std::uint64_t boundChunks = 0;
        std::vector<std::optional<Chunk>> chunks;
    };
    using Ranges = std::map<std::uint64_t, Range>;

    Ranges::iterator rangeOf(std::uint64_t address);
    Ranges::const_iterator rangeOf(std::uint64_t address) const;
    std::optional<Chunk> take();
    void put(Chunk chunk);
    std::uint64_t unbind(Range& range);

    ResidencyDevice& device;
    mutable std::mutex mutex;
    Ranges ranges;
    std::vector<Chunk> free;
    std::uint32_t pools = 0;
    std::uint64_t budget = 0;
    std::uint64_t boundBytes = 0;
    std::uint64_t boundCount = 0;
    std::uint64_t evictedCount = 0;
    std::uint64_t refusedCount = 0;
};

}

#endif
