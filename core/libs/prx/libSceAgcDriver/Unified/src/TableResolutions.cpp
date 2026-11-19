#include "prx/libSceAgcDriver/Unified/include/TableResolutions.hpp"
#include <algorithm>

namespace AgcDriver::Unified::Tables {

namespace {

std::size_t columnBytes(const std::vector<ShaderRecompiler::ImageTableColumnSnapshot>& columns) {
    std::size_t total = 0;
    for (const auto& column : columns) {
        total += sizeof(column) + column.codes.size() * sizeof(std::uint32_t) + column.words.size() * sizeof(ShaderRecompiler::DescriptorValue) + column.pointers.size() * sizeof(std::uint64_t) + column.readRanges.size() * 16u;
        for (const auto& view : column.resourceViews) total += view.words.size() * sizeof(ShaderRecompiler::DescriptorValue) + view.identities.size() * sizeof(ShaderRecompiler::RegisteredImageView);
    }
    return total;
}

std::size_t resolvedBytes(const ShaderRecompiler::ResolvedImageTables& resolved) {
    return sizeof(resolved) + resolved.map.size() * sizeof(std::uint32_t) + resolved.elements.size() * sizeof(ShaderRecompiler::ImageTableElement) + resolved.samplers.size() * sizeof(ShaderRecompiler::ImageTableSamplerElement) + resolved.poison.size() * sizeof(ShaderRecompiler::ImageTablePoisonRecord);
}

}

namespace {

template <typename T>
bool sameValues(const ShaderRecompiler::SharedValues<T>& left, const ShaderRecompiler::SharedValues<T>& right, bool byContent) {
    if (left.Shares(right)) return true;
    if (!byContent || left.size() != right.size() || left.Hash() != right.Hash()) return false;
    return std::equal(left.begin(), left.end(), right.begin());
}

}

bool SameColumns(const std::vector<ShaderRecompiler::ImageTableColumnSnapshot>& left, const std::vector<ShaderRecompiler::ImageTableColumnSnapshot>& right, bool byContent) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.base != b.base || a.size != b.size || a.records != b.records || a.keys != b.keys || a.fault != b.fault || a.outside != b.outside || a.nested != b.nested) return false;
        if (!sameValues(a.pointers, b.pointers, byContent) || !sameValues(a.codes, b.codes, byContent) || !sameValues(a.words, b.words, byContent) || a.resourceViews != b.resourceViews) return false;
    }
    return true;
}

std::vector<ShaderRecompiler::DescriptorValue> DirectSamplers(const ShaderRecompiler::ShaderInfo& info, const ShaderRecompiler::ResourceSnapshot& snapshot) {
    std::vector<ShaderRecompiler::DescriptorValue> samplers;
    for (const auto& pair : info.sampledPairs) {
        if (pair.sampler >= info.samplers.size() || info.samplers[pair.sampler].table != ShaderRecompiler::NoTable) continue;
        samplers.push_back(pair.sampler < snapshot.samplers.size() ? snapshot.samplers[pair.sampler] : ShaderRecompiler::DescriptorValue{});
    }
    return samplers;
}

std::shared_ptr<const ShaderRecompiler::ResolvedImageTables> TableResolutions::Find(const ShaderRecompiler::ShaderInfo& info, const ShaderRecompiler::ResourceSnapshot& snapshot, std::uint64_t memoKey, const Resolve& resolve) {
    auto samplers = DirectSamplers(info, snapshot);
    bool known = false;
    bool samplersOnly = false;
    if (const auto found = byKey.find(memoKey); found != byKey.end()) {
        known = !found->second.empty();
        for (const bool byContent : {false, true}) {
            if (byContent && !comparesContent) break;
            for (const auto entry : found->second) {
                const bool columns = SameColumns(entry->tables, snapshot.tables.tables, byContent) && SameColumns(entry->nested, snapshot.tables.nested, byContent);
                if (columns && entry->samplers == samplers) {
                    order.splice(order.begin(), order, entry);
                    entry->used = ++clock;
                    if (byContent) {
                        entry->tables = snapshot.tables.tables;
                        entry->nested = snapshot.tables.nested;
                        counts.contentHits++;
                    } else {
                        counts.hits++;
                    }
                    return entry->resolved;
                }
                samplersOnly = samplersOnly || columns;
            }
        }
    }
    auto resolved = resolve();
    if (resolved == nullptr) return resolved;
    if (samplersOnly) counts.samplersChanged++;
    else if (known) counts.changed++;
    else counts.fresh++;
    Entry entry{memoKey, snapshot.tables.tables, snapshot.tables.nested, std::move(samplers), resolved, 0u, ++clock};
    entry.bytes = sizeof(Entry) + columnBytes(entry.tables) + columnBytes(entry.nested) + resolvedBytes(*resolved);
    if (entry.bytes <= budget) insert(std::move(entry));
    return resolved;
}

void TableResolutions::insert(Entry entry) {
    auto& keyed = byKey[entry.memoKey];
    if (keyed.size() >= ResolutionKeyEntries) {
        const auto oldest = std::min_element(keyed.begin(), keyed.end(), [](Order::iterator left, Order::iterator right) { return left->used < right->used; });
        bytes -= (*oldest)->bytes;
        order.erase(*oldest);
        keyed.erase(oldest);
        counts.evicted++;
    }
    bytes += entry.bytes;
    order.push_front(std::move(entry));
    keyed.push_back(order.begin());
    while (bytes > budget && order.size() > 1u) evictOldest();
}

void TableResolutions::evictOldest() {
    const auto last = std::prev(order.end());
    auto& keyed = byKey[last->memoKey];
    std::erase(keyed, last);
    if (keyed.empty()) byKey.erase(last->memoKey);
    bytes -= last->bytes;
    order.erase(last);
    counts.evicted++;
}

ResolutionCounts TableResolutions::Counts() const {
    auto out = counts;
    out.held = order.size();
    out.bytes = bytes;
    return out;
}

}
