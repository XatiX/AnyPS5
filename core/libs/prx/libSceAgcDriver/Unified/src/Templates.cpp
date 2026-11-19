#include "prx/libSceAgcDriver/Unified/include/Templates.hpp"
#include <algorithm>
#include <vector>

namespace AgcDriver::Unified::Resolve {

TemplateTable::TemplateTable(std::uint64_t budgetBytes) : budget(budgetBytes) {}

std::shared_ptr<const ShaderRecompiler::ResultTemplate> TemplateTable::Find(std::uint64_t key) {
    const auto found = entries.find(key);
    if (found == entries.end()) return nullptr;
    found->second.used = ++clock;
    counts.hits++;
    return found->second.shape;
}

void TemplateTable::Store(std::uint64_t key, std::shared_ptr<const ShaderRecompiler::ResultTemplate> entry) {
    const auto bytes = ShaderRecompiler::ResultTemplateBytes(*entry);
    auto& slot = entries[key];
    heldBytes -= slot.bytes;
    slot = {std::move(entry), bytes, ++clock};
    heldBytes += bytes;
    if (budget != 0 && heldBytes > budget) evict();
}

void TemplateTable::CountBuild(bool rebuild) {
    counts.builds++;
    if (rebuild) counts.rebuilds++;
}

TemplateTable::Counts TemplateTable::TakeCounts() {
    const auto taken = counts;
    counts = {};
    return taken;
}

void TemplateTable::evict() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
    order.reserve(entries.size());
    for (const auto& [key, entry] : entries) order.emplace_back(entry.used, key);
    std::ranges::sort(order);
    const auto target = budget - budget / 8u;
    for (const auto& [used, key] : order) {
        if (heldBytes <= target || entries.size() <= 1u) break;
        const auto found = entries.find(key);
        heldBytes -= found->second.bytes;
        entries.erase(found);
        counts.evictions++;
    }
}

}
