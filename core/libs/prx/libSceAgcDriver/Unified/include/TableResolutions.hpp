#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLERESOLUTIONS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TABLERESOLUTIONS_HPP

#include "Optimization/ResourceMaterializer.hpp"
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Unified::Tables {

inline constexpr std::size_t ResolutionKeyEntries = 16;
inline constexpr std::size_t ResolutionBytes = std::size_t{512} << 20u;

struct ResolutionCounts {
    std::uint64_t hits = 0;
    std::uint64_t contentHits = 0;
    std::uint64_t fresh = 0;
    std::uint64_t changed = 0;
    std::uint64_t samplersChanged = 0;
    std::uint64_t evicted = 0;
    std::uint64_t held = 0;
    std::uint64_t bytes = 0;
};

[[nodiscard]] bool SameColumns(const std::vector<ShaderRecompiler::ImageTableColumnSnapshot>& left, const std::vector<ShaderRecompiler::ImageTableColumnSnapshot>& right, bool byContent);
[[nodiscard]] std::vector<ShaderRecompiler::DescriptorValue> DirectSamplers(const ShaderRecompiler::ShaderInfo& info, const ShaderRecompiler::ResourceSnapshot& snapshot);

class TableResolutions {
public:
    using Resolve = std::function<std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>()>;

    explicit TableResolutions(std::size_t budget = ResolutionBytes, bool comparesContent = true) : budget(budget), comparesContent(comparesContent) {}

    [[nodiscard]] std::shared_ptr<const ShaderRecompiler::ResolvedImageTables> Find(const ShaderRecompiler::ShaderInfo& info, const ShaderRecompiler::ResourceSnapshot& snapshot, std::uint64_t memoKey, const Resolve& resolve);
    [[nodiscard]] ResolutionCounts Counts() const;

private:
    struct Entry {
        std::uint64_t memoKey = 0;
        std::vector<ShaderRecompiler::ImageTableColumnSnapshot> tables;
        std::vector<ShaderRecompiler::ImageTableColumnSnapshot> nested;
        std::vector<ShaderRecompiler::DescriptorValue> samplers;
        std::shared_ptr<const ShaderRecompiler::ResolvedImageTables> resolved;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };
    using Order = std::list<Entry>;

    void insert(Entry entry);
    void evictOldest();

    std::size_t budget;
    bool comparesContent;
    Order order;
    std::unordered_map<std::uint64_t, std::vector<Order::iterator>> byKey;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
    ResolutionCounts counts;
};

}

#endif
