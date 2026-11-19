#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <unordered_set>

namespace AgcDriver::Unified::Tables {

bool ElementRequests(const ShaderRecompiler::ResolvedImageTables& tables, std::vector<Heap::SlotRequest>& requests, std::string& reason) {
    requests.clear();
    if (!tables.samplers.empty()) {
        reason = "sampler tables";
        return false;
    }
    if (tables.map.size() < ShaderRecompiler::ImageTableAbi::HeaderWords) {
        reason = "image table map without a header";
        return false;
    }
    requests.reserve(tables.elements.size());
    for (const auto& element : tables.elements) {
        const auto kind = static_cast<std::uint32_t>(element.kind);
        if (kind < Heap::FirstImageKind || kind > Heap::LastStorageKind) {
            reason = "image table element without an image kind";
            return false;
        }
        if (kind > Heap::LastSampledKind) {
            reason = "storage image tables";
            return false;
        }
        Heap::SlotRequest request{kind, 0u, element.words};
        if (element.words[0] == 0u && (element.words[1] & 0xffu) == 0u) request.flags = Heap::RequestFlag::Inactive;
        requests.push_back(request);
    }
    return true;
}

std::vector<std::uint32_t> MirrorMap(std::span<const std::uint32_t> map, std::span<const std::uint32_t> slots) {
    std::vector<std::uint32_t> out;
    out.reserve(map.size() + slots.size());
    out.assign(map.begin(), map.end());
    out[ShaderRecompiler::ImageTableAbi::HeaderElementBase] = static_cast<std::uint32_t>(map.size());
    out.insert(out.end(), slots.begin(), slots.end());
    return out;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> CoalescedSpans(std::span<const Heap::ImageUse> uses) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    spans.reserve(uses.size());
    for (const auto& use : uses) {
        if (use.memory.bytes != 0u) spans.emplace_back(use.memory.address, use.memory.address + use.memory.bytes);
    }
    std::sort(spans.begin(), spans.end());
    std::size_t kept = 0;
    for (const auto& span : spans) {
        if (kept != 0u && span.first <= spans[kept - 1].second) spans[kept - 1].second = std::max(spans[kept - 1].second, span.second);
        else spans[kept++] = span;
    }
    spans.resize(kept);
    return spans;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Outside(std::pair<std::uint64_t, std::uint64_t> span, std::vector<std::pair<std::uint64_t, std::uint64_t>> holes) {
    std::sort(holes.begin(), holes.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;
    auto cursor = span.first;
    for (const auto& [begin, end] : holes) {
        if (end <= cursor || begin >= span.second) continue;
        if (begin > cursor) pieces.emplace_back(cursor, begin);
        cursor = std::max(cursor, end);
        if (cursor >= span.second) break;
    }
    if (cursor < span.second) pieces.emplace_back(cursor, span.second);
    return pieces;
}

namespace {

std::uint64_t mapKey(std::span<const std::uint32_t> map) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : map) hash = (hash ^ word) * 0x100000001b3ull;
    return hash;
}

bool sameRequest(const Heap::SlotRequest& left, const Heap::SlotRequest& right) {
    return left.kind == right.kind && left.flags == right.flags && left.words == right.words;
}

bool overlapsAny(const Heap::Span& memory, std::span<const std::pair<std::uint64_t, std::uint64_t>> spans) {
    if (memory.bytes == 0u || spans.empty()) return false;
    const auto end = memory.address + memory.bytes;
    auto next = std::upper_bound(spans.begin(), spans.end(), memory.address, [](std::uint64_t address, const auto& span) { return address < span.first; });
    if (next != spans.begin() && std::prev(next)->second > memory.address) return true;
    return next != spans.end() && next->first < end;
}

}

bool TableMirror::unchanged(const Cached& cached) const {
    return std::none_of(cached.spans.begin(), cached.spans.end(), [&](const auto& span) {
        const auto bytes = span.second - span.first;
        return shared.ChangedSince(span.first, bytes, cached.sharedStamp) || images.ChangedSince(span.first, bytes, cached.imagesStamp);
    });
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> TableMirror::changedSpans(const Cached& cached) const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
    for (const auto& span : cached.spans) {
        const auto bytes = span.second - span.first;
        if (shared.ChangedSince(span.first, bytes, cached.sharedStamp) || images.ChangedSince(span.first, bytes, cached.imagesStamp)) changed.push_back(span);
    }
    return changed;
}

bool TableMirror::fill(const Cached* base, Cached& out, std::string& reason) {
    const auto changed = base != nullptr ? changedSpans(*base) : std::vector<std::pair<std::uint64_t, std::uint64_t>>{};
    const auto count = out.requests.size();
    out.slots.assign(count, 0u);
    out.memory.assign(count, {});
    out.owners.assign(count, nullptr);
    out.placeholders.assign(count, false);
    out.complete = true;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& request = out.requests[index];
        if (base != nullptr && index < base->requests.size() && !base->placeholders[index] && sameRequest(request, base->requests[index]) && !overlapsAny(base->memory[index], changed)) {
            out.slots[index] = base->slots[index];
            out.memory[index] = base->memory[index];
            out.owners[index] = base->owners[index];
            counts.kept++;
            continue;
        }
        const auto placeholders = refill.Counts().placeholders;
        const auto slot = refill.FillOne(request, reason, out.memory[index], out.owners[index]);
        if (slot == Heap::NoSlot) return false;
        out.slots[index] = slot;
        if (refill.Counts().placeholders != placeholders) {
            out.placeholders[index] = true;
            out.complete = false;
        }
        counts.elements++;
    }
    out.uses.clear();
    for (std::size_t index = 0; index < count; ++index) {
        if (out.memory[index].bytes != 0u) out.uses.push_back({out.memory[index], false});
    }
    out.spans = CoalescedSpans(out.uses);
    out.uses.clear();
    for (const auto& [begin, end] : out.spans) out.uses.push_back({{begin, end - begin}, false});
    return true;
}

std::shared_ptr<const Mirror> TableMirror::Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses) {
    if (tables == nullptr) {
        reason = "image tables not resolved";
        return nullptr;
    }
    auto found = cache.find(tables.get());
    const bool same = found != cache.end() && found->second.tables.lock() == tables;
    if (same && found->second.complete && unchanged(found->second)) {
        counts.unchanged++;
        if (uses != nullptr) uses->insert(uses->end(), found->second.uses.begin(), found->second.uses.end());
        return found->second.mirror;
    }
    const auto key = mapKey(tables->map);
    const Cached* base = same ? &found->second : nullptr;
    if (base == nullptr) {
        if (const auto previous = predecessors.find(key); previous != predecessors.end()) {
            if (const auto entry = cache.find(previous->second); entry != cache.end() && entry->second.map == tables->map) {
                base = &entry->second;
                counts.inherited++;
            }
        }
    }
    Cached next;
    next.tables = tables;
    next.sharedStamp = shared.Serial();
    next.imagesStamp = images.Serial();
    if (!ElementRequests(*tables, next.requests, reason)) return nullptr;
    if (!fill(base, next, reason)) return nullptr;
    if (uses != nullptr) uses->insert(uses->end(), next.uses.begin(), next.uses.end());
    if (base != nullptr && base->slots == next.slots && base->map == tables->map) {
        next.map = base->map;
        next.mirror = base->mirror;
        counts.reused++;
    } else {
        auto mirror = std::make_shared<Mirror>();
        mirror->id = nextId++;
        mirror->map = MirrorMap(tables->map, next.slots);
        for (const auto& owner : next.owners) {
            if (owner != nullptr) mirror->owners.push_back(owner);
        }
        next.map = tables->map;
        next.mirror = std::move(mirror);
        counts.built++;
    }
    auto mirror = next.mirror;
    if (!same && cache.size() >= MirrorCacheBound) {
        cache.clear();
        predecessors.clear();
        counts.clears++;
    }
    cache[tables.get()] = std::move(next);
    predecessors[key] = tables.get();
    if (cache.size() >= pruneAt) prune();
    return mirror;
}

void TableMirror::prune() {
    std::unordered_set<const void*> named;
    named.reserve(predecessors.size());
    for (const auto& [key, tables] : predecessors) named.insert(tables);
    const auto before = cache.size();
    std::erase_if(cache, [&](const auto& entry) { return entry.second.tables.expired() && !named.contains(entry.first); });
    counts.dropped += before - cache.size();
    pruneAt = std::max(MirrorPruneFloor, cache.size() * 2u);
}

}
