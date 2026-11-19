#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_HEAP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_HEAP_HPP

#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified::Heap {

inline constexpr std::uint32_t FirstImageKind = 1;
inline constexpr std::uint32_t LastSampledKind = 28;
inline constexpr std::uint32_t LastStorageKind = 56;
inline constexpr std::uint32_t SamplerKind = 57;
inline constexpr std::uint32_t SampledSlots = 32768;
inline constexpr std::uint32_t SamplerSlots = 4096;
inline constexpr std::uint32_t StorageSlots = 1024;
inline constexpr std::uint32_t FirstSamplerSlot = 2;
inline constexpr std::uint32_t NoSlot = 0xffffffffu;
inline constexpr std::size_t DefaultMemoBound = 65536;

class HeapDevice {
public:
    virtual ~HeapDevice() = default;
    virtual void WriteImage(std::uint32_t kind, std::uint32_t slot, std::uint64_t view, std::uint32_t layout) = 0;
    virtual void WriteSampler(std::uint32_t slot, std::uint64_t sampler) = 0;
};

struct ImageRef {
    std::uint64_t view = 0;
    std::uint32_t layout = 0;
    std::shared_ptr<const void> owner;
};

struct SlotCounts {
    std::uint64_t writes = 0;
    std::uint64_t hits = 0;
    std::uint64_t recycled = 0;
    std::uint64_t full = 0;
};

class SlotTable {
public:
    explicit SlotTable(HeapDevice& device, std::uint32_t sampledSlots = SampledSlots, std::uint32_t samplerSlots = SamplerSlots, std::uint32_t storageSlots = StorageSlots);

    [[nodiscard]] std::uint32_t ImageSlot(std::uint32_t kind, const ImageRef& image);
    [[nodiscard]] std::uint32_t SamplerSlot(std::uint64_t sampler, std::shared_ptr<const void> keep);
    [[nodiscard]] SlotCounts Counts() const;

private:
    struct Entry {
        std::uint32_t index = 0;
        std::weak_ptr<const void> owner;
    };
    struct KindSlots {
        std::unordered_map<std::uint64_t, Entry> slots;
        std::vector<std::uint32_t> free;
        std::uint32_t next = 1;
    };

    HeapDevice& device;
    std::uint32_t sampledSlots;
    std::uint32_t samplerSlots;
    std::uint32_t storageSlots;
    std::array<KindSlots, LastStorageKind + 1> kinds;
    std::unordered_map<std::uint64_t, std::uint32_t> samplers;
    std::vector<std::shared_ptr<const void>> keptSamplers;
    std::uint32_t nextSampler = FirstSamplerSlot;
    mutable std::mutex mutex;
    SlotCounts counts;
};

struct Span {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

struct DescriptorKey {
    std::uint32_t kind = 0;
    std::array<std::uint32_t, 8> words{};

    bool operator==(const DescriptorKey&) const = default;
};

struct MemoCounts {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t stale = 0;
    std::uint64_t expired = 0;
    std::uint64_t stores = 0;
    std::uint64_t clears = 0;
};

struct Stamps {
    std::uint64_t shared = 0;
    std::uint64_t images = 0;
};

class Memo {
public:
    Memo(const Events::ChangeLog& shared, const Events::ChangeLog& images, std::size_t bound = DefaultMemoBound);

    [[nodiscard]] Stamps Now() const { return {shared.Serial(), images.Serial()}; }
    [[nodiscard]] std::optional<std::uint32_t> Find(const DescriptorKey& key, Span* image = nullptr, std::shared_ptr<const void>* owner = nullptr);
    void Store(const DescriptorKey& key, std::uint32_t slot, std::weak_ptr<const void> owner, Span descriptor, Span image, Stamps before);
    [[nodiscard]] std::size_t Size() const { return entries.size(); }
    [[nodiscard]] MemoCounts Counts() const { return counts; }

private:
    struct Entry {
        std::uint32_t slot = NoSlot;
        std::weak_ptr<const void> owner;
        Span descriptor;
        Span image;
        Stamps stamps;
    };
    struct KeyHash {
        std::size_t operator()(const DescriptorKey& key) const;
    };

    const Events::ChangeLog& shared;
    const Events::ChangeLog& images;
    std::size_t bound;
    std::unordered_map<DescriptorKey, Entry, KeyHash> entries;
    MemoCounts counts;
};

}

#endif
