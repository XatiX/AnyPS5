#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLEMIRROR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLEMIRROR_HPP

#include "prx/libSceAgcDriver/Unified/include/Events.hpp"
#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Unified::Tables {

inline constexpr std::size_t MirrorCacheBound = 4096;
inline constexpr std::size_t MirrorPruneFloor = 64;
inline constexpr std::size_t TableUseSpans = 64;
inline constexpr std::uint32_t NoSpan = ~0u;

struct Mirror {
    std::uint64_t id = 0;
    std::uint64_t lineage = 0;
    std::vector<std::uint32_t> map;
    std::vector<std::shared_ptr<const void>> owners;
    std::uint32_t slotBase = 0;
    std::vector<Heap::DeferredImage> deferred;
};

struct MirrorCounts {
    std::uint64_t built = 0;
    std::uint64_t reused = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t elements = 0;
    std::uint64_t kept = 0;
    std::uint64_t inherited = 0;
    std::uint64_t clears = 0;
    std::uint64_t dropped = 0;
    std::uint64_t inPlace = 0;
};

[[nodiscard]] bool ElementRequests(const ShaderRecompiler::ResolvedImageTables& tables, std::vector<Heap::SlotRequest>& requests, std::string& reason);
[[nodiscard]] std::vector<std::uint32_t> MirrorMap(std::span<const std::uint32_t> map, std::span<const std::uint32_t> slots);

[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> CoalescedSpans(std::span<const Heap::ImageUse> uses);
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> BoundedSpans(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans, std::size_t limit);
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> Outside(std::pair<std::uint64_t, std::uint64_t> span, std::vector<std::pair<std::uint64_t, std::uint64_t>> holes);

class TableMirror {
public:
    TableMirror(Heap::Refill& refill, const Events::ChangeLog& shared, const Events::ChangeLog& images) : refill(refill), shared(shared), images(images) {}

    [[nodiscard]] std::shared_ptr<const Mirror> Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses = nullptr);
    [[nodiscard]] MirrorCounts Counts() const { return counts; }
    [[nodiscard]] std::size_t Size() const { return cache.size(); }

private:
    struct Cached {
        std::weak_ptr<const ShaderRecompiler::ResolvedImageTables> tables;
        std::vector<std::uint32_t> map;
        std::vector<Heap::SlotRequest> requests;
        std::vector<std::uint32_t> slots;
        std::vector<Heap::Span> memory;
        std::vector<std::shared_ptr<const void>> owners;
        std::vector<bool> placeholders;
        std::vector<bool> deferred;
        std::shared_ptr<const Mirror> mirror;
        std::uint64_t sharedStamp = 0;
        std::uint64_t imagesStamp = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
        std::vector<std::uint32_t> regions;
        std::vector<std::uint32_t> spanStarts;
        std::vector<std::uint32_t> spanElements;
        std::vector<std::uint32_t> regionStarts;
        std::vector<std::uint32_t> regionSpans;
        std::vector<std::uint32_t> pending;
        std::vector<Heap::ImageUse> uses;
        bool complete = false;
    };

    struct SpanScan {
        std::uint64_t sharedStamp = 0;
        std::uint64_t imagesStamp = 0;
        std::vector<std::uint32_t> spans;
    };

    [[nodiscard]] SpanScan scanSpans(const Cached& cached) const;
    [[nodiscard]] bool regionsQuiet(const Cached& cached) const;
    [[nodiscard]] bool unchanged(const Cached& cached) const;
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> changedSpans(const Cached& cached) const;
    [[nodiscard]] bool fill(const Cached* base, Cached& out, std::string& reason);
    void index(Cached& out);
    [[nodiscard]] bool refillInPlace(Cached& entry, const SpanScan& scan, std::string& reason, bool& slotsChanged, std::span<const std::uint32_t> differing = {});
    [[nodiscard]] std::shared_ptr<Mirror> makeMirror(const Cached& entry, std::span<const std::uint32_t> map, const Mirror* previous);
    [[nodiscard]] std::shared_ptr<const Mirror> publish(Cached& entry, const ShaderRecompiler::ResolvedImageTables& tables, bool slotsChanged, std::vector<Heap::ImageUse>* uses);
    [[nodiscard]] bool inherit(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::uint64_t key, std::string& reason, std::shared_ptr<const Mirror>& out, std::vector<Heap::ImageUse>* uses);
    void prune();

    Heap::Refill& refill;
    const Events::ChangeLog& shared;
    const Events::ChangeLog& images;
    std::unordered_map<const void*, Cached> cache;
    std::unordered_map<std::uint64_t, const void*> predecessors;
    std::uint64_t nextId = 1;
    std::size_t pruneAt = MirrorPruneFloor;
    MirrorCounts counts;
};

}

#endif
