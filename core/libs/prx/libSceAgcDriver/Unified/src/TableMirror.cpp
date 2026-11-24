#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>
#include <cstdlib>
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

std::vector<std::pair<std::uint64_t, std::uint64_t>> BoundedSpans(std::span<const std::pair<std::uint64_t, std::uint64_t>> spans, std::size_t limit) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bounded(spans.begin(), spans.end());
    if (limit == 0u || bounded.size() <= limit) return bounded;
    std::vector<std::size_t> gaps(bounded.size() - 1u);
    for (std::size_t index = 0; index < gaps.size(); ++index) gaps[index] = index;
    std::sort(gaps.begin(), gaps.end(), [&](std::size_t left, std::size_t right) {
        const auto leftGap = bounded[left + 1u].first - bounded[left].second;
        const auto rightGap = bounded[right + 1u].first - bounded[right].second;
        return leftGap != rightGap ? leftGap < rightGap : left < right;
    });
    std::vector<bool> merged(gaps.size(), false);
    for (std::size_t index = 0; index < bounded.size() - limit; ++index) merged[gaps[index]] = true;
    std::size_t kept = 0;
    for (std::size_t index = 1; index < bounded.size(); ++index) {
        if (merged[index - 1u]) bounded[kept].second = std::max(bounded[kept].second, bounded[index].second);
        else bounded[++kept] = bounded[index];
    }
    bounded.resize(kept + 1u);
    return bounded;
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

bool refillsInPlace() {
    static const bool inPlace = std::getenv("APS5_RECODE_FULL_MIRROR_REFILL") == nullptr;
    return inPlace;
}

bool refillsWholeSpans() {
    static const bool whole = std::getenv("APS5_RECODE_REFILL_WHOLE_SPANS") != nullptr;
    return whole;
}

bool boundsUses() {
    static const bool bounds = std::getenv("APS5_RECODE_EXACT_TABLE_USES") == nullptr;
    return bounds;
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

bool TableMirror::regionsQuiet(const Cached& cached) const {
    return !shared.RegionsChangedSince(cached.regions, cached.sharedStamp) && !images.RegionsChangedSince(cached.regions, cached.imagesStamp);
}

bool TableMirror::unchanged(const Cached& cached) const {
    if (regionsQuiet(cached)) return true;
    return std::none_of(cached.spans.begin(), cached.spans.end(), [&](const auto& span) {
        const auto bytes = span.second - span.first;
        return shared.ChangedSince(span.first, bytes, cached.sharedStamp) || images.ChangedSince(span.first, bytes, cached.imagesStamp);
    });
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> TableMirror::changedSpans(const Cached& cached) const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
    if (regionsQuiet(cached)) return changed;
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
    out.deferred.assign(count, false);
    out.complete = true;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& request = out.requests[index];
        if (base != nullptr && index < base->requests.size() && !base->placeholders[index] && sameRequest(request, base->requests[index]) && !overlapsAny(base->memory[index], changed)) {
            out.slots[index] = base->slots[index];
            out.memory[index] = base->memory[index];
            out.owners[index] = base->owners[index];
            out.deferred[index] = base->deferred[index];
            counts.kept++;
            continue;
        }
        const auto placeholders = refill.Counts().placeholders;
        bool deferred = false;
        const auto slot = refill.FillOne(request, reason, out.memory[index], out.owners[index], &deferred);
        if (slot == Heap::NoSlot) return false;
        out.slots[index] = slot;
        out.deferred[index] = deferred;
        if (refill.Counts().placeholders != placeholders) {
            out.placeholders[index] = true;
            out.complete = false;
        }
        counts.elements++;
    }
    index(out);
    return true;
}

void TableMirror::index(Cached& out) {
    const auto count = out.requests.size();
    out.uses.clear();
    for (std::size_t element = 0; element < count; ++element) {
        if (out.memory[element].bytes != 0u) out.uses.push_back({out.memory[element], false});
    }
    out.spans = CoalescedSpans(out.uses);
    out.regions.clear();
    for (const auto& [begin, end] : out.spans) Events::ChangeLog::AppendRegions(begin, end - begin, out.regions);
    std::sort(out.regions.begin(), out.regions.end());
    out.regions.erase(std::unique(out.regions.begin(), out.regions.end()), out.regions.end());
    out.uses.clear();
    for (const auto& [begin, end] : boundsUses() ? BoundedSpans(out.spans, TableUseSpans) : out.spans) out.uses.push_back({{begin, end - begin}, false});
    std::vector<std::uint32_t> spanOf(count, NoSpan);
    out.spanStarts.assign(out.spans.size() + 1u, 0u);
    out.pending.clear();
    for (std::size_t element = 0; element < count; ++element) {
        if (out.placeholders[element]) out.pending.push_back(static_cast<std::uint32_t>(element));
        const auto& memory = out.memory[element];
        if (memory.bytes == 0u) continue;
        const auto next = std::upper_bound(out.spans.begin(), out.spans.end(), memory.address, [](std::uint64_t address, const auto& span) { return address < span.first; });
        if (next == out.spans.begin()) continue;
        const auto span = static_cast<std::uint32_t>(std::prev(next) - out.spans.begin());
        spanOf[element] = span;
        out.spanStarts[span + 1u]++;
    }
    for (std::size_t span = 0; span < out.spans.size(); ++span) out.spanStarts[span + 1u] += out.spanStarts[span];
    out.spanElements.assign(out.spanStarts.back(), 0u);
    auto cursor = out.spanStarts;
    for (std::size_t element = 0; element < count; ++element) {
        if (spanOf[element] != NoSpan) out.spanElements[cursor[spanOf[element]]++] = static_cast<std::uint32_t>(element);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> regionSpan;
    std::vector<std::uint32_t> touched;
    for (std::size_t span = 0; span < out.spans.size(); ++span) {
        touched.clear();
        Events::ChangeLog::AppendRegions(out.spans[span].first, out.spans[span].second - out.spans[span].first, touched);
        for (const auto region : touched) {
            const auto position = std::lower_bound(out.regions.begin(), out.regions.end(), region) - out.regions.begin();
            regionSpan.emplace_back(static_cast<std::uint32_t>(position), static_cast<std::uint32_t>(span));
        }
    }
    std::sort(regionSpan.begin(), regionSpan.end());
    regionSpan.erase(std::unique(regionSpan.begin(), regionSpan.end()), regionSpan.end());
    out.regionStarts.assign(out.regions.size() + 1u, 0u);
    for (const auto& [position, span] : regionSpan) out.regionStarts[position + 1u]++;
    for (std::size_t position = 0; position < out.regions.size(); ++position) out.regionStarts[position + 1u] += out.regionStarts[position];
    out.regionSpans.resize(regionSpan.size());
    for (std::size_t index = 0; index < regionSpan.size(); ++index) out.regionSpans[index] = regionSpan[index].second;
}

TableMirror::SpanScan TableMirror::scanSpans(const Cached& entry) const {
    SpanScan scan{shared.Serial(), images.Serial(), {}};
    for (std::size_t position = 0; position < entry.regions.size(); ++position) {
        const auto region = entry.regions[position];
        if (!shared.RegionChangedSince(region, entry.sharedStamp) && !images.RegionChangedSince(region, entry.imagesStamp)) continue;
        for (auto index = entry.regionStarts[position]; index < entry.regionStarts[position + 1u]; ++index) scan.spans.push_back(entry.regionSpans[index]);
    }
    if (scan.spans.empty()) return scan;
    std::sort(scan.spans.begin(), scan.spans.end());
    scan.spans.erase(std::unique(scan.spans.begin(), scan.spans.end()), scan.spans.end());
    std::erase_if(scan.spans, [&](std::uint32_t span) {
        const auto& [begin, end] = entry.spans[span];
        return !shared.ChangedSince(begin, end - begin, entry.sharedStamp) && !images.ChangedSince(begin, end - begin, entry.imagesStamp);
    });
    return scan;
}

bool TableMirror::refillInPlace(Cached& entry, const SpanScan& scan, std::string& reason, bool& slotsChanged, std::span<const std::uint32_t> differing) {
    std::vector<std::uint32_t> targets = entry.pending;
    targets.insert(targets.end(), differing.begin(), differing.end());
    for (const auto span : scan.spans) {
        for (auto position = entry.spanStarts[span]; position < entry.spanStarts[span + 1u]; ++position) {
            const auto element = entry.spanElements[position];
            const auto& memory = entry.memory[element];
            if (!refillsWholeSpans() && !shared.ChangedSince(memory.address, memory.bytes, entry.sharedStamp) && !images.ChangedSince(memory.address, memory.bytes, entry.imagesStamp)) continue;
            targets.push_back(element);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    bool memoryMoved = false;
    slotsChanged = false;
    entry.complete = true;
    for (const auto element : targets) {
        Heap::Span memory{};
        std::shared_ptr<const void> owner;
        const auto placeholders = refill.Counts().placeholders;
        bool deferred = false;
        const auto slot = refill.FillOne(entry.requests[element], reason, memory, owner, &deferred);
        if (slot == Heap::NoSlot) return false;
        if (slot != entry.slots[element] || deferred != entry.deferred[element] || (deferred && std::binary_search(differing.begin(), differing.end(), element))) slotsChanged = true;
        entry.deferred[element] = deferred;
        if (memory.address != entry.memory[element].address || memory.bytes != entry.memory[element].bytes) memoryMoved = true;
        entry.slots[element] = slot;
        entry.memory[element] = memory;
        entry.owners[element] = std::move(owner);
        entry.placeholders[element] = refill.Counts().placeholders != placeholders;
        counts.elements++;
    }
    counts.kept += entry.requests.size() - targets.size();
    entry.sharedStamp = scan.sharedStamp;
    entry.imagesStamp = scan.imagesStamp;
    if (memoryMoved) {
        index(entry);
    } else {
        entry.pending.clear();
        for (std::size_t element = 0; element < entry.placeholders.size(); ++element) {
            if (entry.placeholders[element]) entry.pending.push_back(static_cast<std::uint32_t>(element));
        }
    }
    entry.complete = entry.pending.empty();
    return true;
}

std::shared_ptr<const Mirror> TableMirror::Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses) {
    if (tables == nullptr) {
        reason = "image tables not resolved";
        return nullptr;
    }
    auto found = cache.find(tables.get());
    const bool same = found != cache.end() && found->second.tables.lock() == tables;
    if (same && refillsInPlace()) {
        auto& entry = found->second;
        const auto scan = scanSpans(entry);
        if (scan.spans.empty() && entry.complete) {
            entry.sharedStamp = scan.sharedStamp;
            entry.imagesStamp = scan.imagesStamp;
            counts.unchanged++;
            if (uses != nullptr) uses->insert(uses->end(), entry.uses.begin(), entry.uses.end());
            return entry.mirror;
        }
        bool slotsChanged = false;
        if (!refillInPlace(entry, scan, reason, slotsChanged)) return nullptr;
        return publish(entry, *tables, slotsChanged, uses);
    }
    if (same && found->second.complete && unchanged(found->second)) {
        counts.unchanged++;
        if (uses != nullptr) uses->insert(uses->end(), found->second.uses.begin(), found->second.uses.end());
        return found->second.mirror;
    }
    const auto key = mapKey(tables->map);
    if (!same && refillsInPlace()) {
        std::shared_ptr<const Mirror> inherited;
        if (inherit(tables, key, reason, inherited, uses)) return inherited;
    }
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
    if (base != nullptr && base->slots == next.slots && base->map == tables->map && base->deferred == next.deferred && std::ranges::none_of(next.deferred, [](bool deferred) { return deferred; })) {
        next.map = base->map;
        next.mirror = base->mirror;
        counts.reused++;
    } else {
        next.map = tables->map;
        next.mirror = makeMirror(next, tables->map, base != nullptr ? base->mirror.get() : nullptr);
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

std::shared_ptr<Mirror> TableMirror::makeMirror(const Cached& entry, std::span<const std::uint32_t> map, const Mirror* previous) {
    auto mirror = std::make_shared<Mirror>();
    mirror->id = nextId++;
    mirror->lineage = previous != nullptr ? previous->lineage : mirror->id;
    mirror->map = MirrorMap(map, entry.slots);
    mirror->slotBase = static_cast<std::uint32_t>(map.size());
    for (const auto& owner : entry.owners) {
        if (owner != nullptr) mirror->owners.push_back(owner);
    }
    for (std::size_t element = 0; element < entry.deferred.size(); ++element) {
        if (entry.deferred[element]) mirror->deferred.push_back({static_cast<std::uint32_t>(element), entry.requests[element]});
    }
    return mirror;
}

std::shared_ptr<const Mirror> TableMirror::publish(Cached& entry, const ShaderRecompiler::ResolvedImageTables& tables, bool slotsChanged, std::vector<Heap::ImageUse>* uses) {
    if (slotsChanged || entry.mirror == nullptr) {
        entry.mirror = makeMirror(entry, tables.map, entry.mirror.get());
        counts.built++;
    } else {
        counts.reused++;
    }
    counts.inPlace++;
    if (uses != nullptr) uses->insert(uses->end(), entry.uses.begin(), entry.uses.end());
    return entry.mirror;
}

bool TableMirror::inherit(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::uint64_t key, std::string& reason, std::shared_ptr<const Mirror>& out, std::vector<Heap::ImageUse>* uses) {
    const auto previous = predecessors.find(key);
    if (previous == predecessors.end()) return false;
    const auto base = cache.find(previous->second);
    if (base == cache.end() || base->second.map != tables->map) return false;
    std::vector<Heap::SlotRequest> requests;
    if (!ElementRequests(*tables, requests, reason)) {
        out = nullptr;
        return true;
    }
    if (requests.size() != base->second.requests.size()) return false;
    std::vector<std::uint32_t> differing;
    for (std::size_t element = 0; element < requests.size(); ++element) {
        if (!sameRequest(requests[element], base->second.requests[element])) differing.push_back(static_cast<std::uint32_t>(element));
    }
    Cached entry;
    if (base->second.tables.expired()) entry = std::move(cache.extract(base).mapped());
    else entry = base->second;
    entry.tables = tables;
    entry.requests = std::move(requests);
    bool slotsChanged = false;
    if (!refillInPlace(entry, scanSpans(entry), reason, slotsChanged, differing)) {
        out = nullptr;
        return true;
    }
    counts.inherited++;
    auto& stored = cache[tables.get()];
    stored = std::move(entry);
    predecessors[key] = tables.get();
    out = publish(stored, *tables, slotsChanged, uses);
    if (cache.size() >= pruneAt) prune();
    return true;
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
