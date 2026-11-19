#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "ImageTableAbi.hpp"
#include <algorithm>

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

bool TableMirror::unchanged(const Cached& cached) const {
    return std::none_of(cached.spans.begin(), cached.spans.end(), [&](const auto& span) {
        const auto bytes = span.second - span.first;
        return shared.ChangedSince(span.first, bytes, cached.sharedStamp) || images.ChangedSince(span.first, bytes, cached.imagesStamp);
    });
}

std::shared_ptr<const Mirror> TableMirror::Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses) {
    if (tables == nullptr) {
        reason = "image tables not resolved";
        return nullptr;
    }
    auto found = cache.find(tables.get());
    if (found != cache.end() && found->second.complete && found->second.tables.lock() == tables && unchanged(found->second)) {
        counts.unchanged++;
        if (uses != nullptr) uses->insert(uses->end(), found->second.uses.begin(), found->second.uses.end());
        return found->second.mirror;
    }
    const auto sharedStamp = shared.Serial();
    const auto imagesStamp = images.Serial();
    if (!ElementRequests(*tables, requests, reason)) return nullptr;
    std::vector<std::shared_ptr<const void>> owners;
    std::vector<Heap::ImageUse> filled;
    const auto placeholders = refill.Counts().placeholders;
    if (!refill.Fill(requests, slots, reason, &filled, &owners)) return nullptr;
    const bool complete = refill.Counts().placeholders == placeholders;
    counts.elements += requests.size();
    if (uses != nullptr) uses->insert(uses->end(), filled.begin(), filled.end());
    auto spans = CoalescedSpans(filled);
    if (found != cache.end() && found->second.tables.lock() == tables && found->second.slots == slots) {
        counts.reused++;
        auto& cached = found->second;
        cached.sharedStamp = sharedStamp;
        cached.imagesStamp = imagesStamp;
        cached.spans = std::move(spans);
        cached.uses = std::move(filled);
        cached.complete = complete;
        return cached.mirror;
    }
    if (found == cache.end() && cache.size() >= MirrorCacheBound) {
        cache.clear();
        counts.clears++;
    }
    auto mirror = std::make_shared<Mirror>();
    mirror->id = nextId++;
    mirror->map = MirrorMap(tables->map, slots);
    mirror->owners = std::move(owners);
    counts.built++;
    cache[tables.get()] = {tables, slots, mirror, sharedStamp, imagesStamp, std::move(spans), std::move(filled), complete};
    return mirror;
}

}
