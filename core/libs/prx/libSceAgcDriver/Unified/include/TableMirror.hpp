#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLEMIRROR_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLEMIRROR_HPP

#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified::Tables {

inline constexpr std::size_t MirrorCacheBound = 4096;

struct Mirror {
    std::uint64_t id = 0;
    std::vector<std::uint32_t> map;
};

struct MirrorCounts {
    std::uint64_t built = 0;
    std::uint64_t reused = 0;
    std::uint64_t elements = 0;
    std::uint64_t clears = 0;
};

[[nodiscard]] bool ElementRequests(const ShaderRecompiler::ResolvedImageTables& tables, std::vector<Heap::SlotRequest>& requests, std::string& reason);
[[nodiscard]] std::vector<std::uint32_t> MirrorMap(std::span<const std::uint32_t> map, std::span<const std::uint32_t> slots);

class TableMirror {
public:
    explicit TableMirror(Heap::Refill& refill) : refill(refill) {}

    [[nodiscard]] std::shared_ptr<const Mirror> Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses = nullptr);
    [[nodiscard]] MirrorCounts Counts() const { return counts; }

private:
    struct Cached {
        std::weak_ptr<const ShaderRecompiler::ResolvedImageTables> tables;
        std::vector<std::uint32_t> slots;
        std::shared_ptr<const Mirror> mirror;
    };

    Heap::Refill& refill;
    std::unordered_map<const void*, Cached> cache;
    std::vector<Heap::SlotRequest> requests;
    std::vector<std::uint32_t> slots;
    std::uint64_t nextId = 1;
    MirrorCounts counts;
};

}

#endif
