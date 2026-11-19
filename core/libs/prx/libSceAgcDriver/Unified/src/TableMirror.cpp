#include "prx/libSceAgcDriver/Unified/include/TableMirror.hpp"
#include "ImageTableAbi.hpp"

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

std::shared_ptr<const Mirror> TableMirror::Build(const std::shared_ptr<const ShaderRecompiler::ResolvedImageTables>& tables, std::string& reason, std::vector<Heap::ImageUse>* uses) {
    if (tables == nullptr) {
        reason = "image tables not resolved";
        return nullptr;
    }
    if (!ElementRequests(*tables, requests, reason)) return nullptr;
    if (!refill.Fill(requests, slots, reason, uses)) return nullptr;
    counts.elements += requests.size();
    auto found = cache.find(tables.get());
    if (found != cache.end() && found->second.tables.lock() == tables && found->second.slots == slots) {
        counts.reused++;
        return found->second.mirror;
    }
    if (found == cache.end() && cache.size() >= MirrorCacheBound) {
        cache.clear();
        counts.clears++;
    }
    auto mirror = std::make_shared<Mirror>();
    mirror->id = nextId++;
    mirror->map = MirrorMap(tables->map, slots);
    counts.built++;
    cache[tables.get()] = {tables, slots, mirror};
    return mirror;
}

}
