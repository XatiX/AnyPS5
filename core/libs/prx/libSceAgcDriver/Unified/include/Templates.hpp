#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TEMPLATES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_TEMPLATES_HPP

#include "Optimization/ResourceProgram.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace AgcDriver::Unified::Resolve {

class TemplateTable {
public:
    struct Counts {
        std::uint64_t hits = 0;
        std::uint64_t builds = 0;
        std::uint64_t rebuilds = 0;
        std::uint64_t evictions = 0;
    };

    explicit TemplateTable(std::uint64_t budgetBytes);

    [[nodiscard]] std::shared_ptr<const ShaderRecompiler::ResultTemplate> Find(std::uint64_t key);
    void Store(std::uint64_t key, std::shared_ptr<const ShaderRecompiler::ResultTemplate> entry);
    [[nodiscard]] std::uint64_t HeldBytes() const { return heldBytes; }
    [[nodiscard]] std::size_t Size() const { return entries.size(); }
    [[nodiscard]] Counts TakeCounts();
    void CountBuild(bool rebuild);

private:
    struct Entry {
        std::shared_ptr<const ShaderRecompiler::ResultTemplate> shape;
        std::uint64_t bytes = 0;
        std::uint64_t used = 0;
    };

    void evict();

    std::unordered_map<std::uint64_t, Entry> entries;
    std::uint64_t budget = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t clock = 0;
    Counts counts;
};

}

#endif
