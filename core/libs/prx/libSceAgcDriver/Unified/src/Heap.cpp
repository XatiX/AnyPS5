#include "prx/libSceAgcDriver/Unified/include/Heap.hpp"
#include <utility>

namespace AgcDriver::Unified::Heap {

SlotTable::SlotTable(HeapDevice& device, std::uint32_t sampledSlots, std::uint32_t samplerSlots, std::uint32_t storageSlots) : device(device), sampledSlots(sampledSlots), samplerSlots(samplerSlots), storageSlots(storageSlots) {}

std::uint32_t SlotTable::ImageSlot(std::uint32_t kind, const ImageRef& image) {
    const auto capacity = kind <= LastSampledKind ? sampledSlots : storageSlots;
    if (kind < FirstImageKind || kind > LastStorageKind || capacity <= 1u || image.view == 0u || image.owner == nullptr) return NoSlot;
    std::lock_guard lock(mutex);
    auto& table = kinds[kind];
    if (const auto found = table.slots.find(image.view); found != table.slots.end()) {
        if (!found->second.owner.expired()) {
            counts.hits++;
            return found->second.index;
        }
        found->second.owner = image.owner;
        device.WriteImage(kind, found->second.index, image.view, image.layout);
        counts.writes++;
        counts.recycled++;
        return found->second.index;
    }
    if (table.free.empty() && table.next >= capacity) {
        for (auto at = table.slots.begin(); at != table.slots.end();) {
            if (!at->second.owner.expired()) {
                ++at;
                continue;
            }
            table.free.push_back(at->second.index);
            at = table.slots.erase(at);
        }
    }
    std::uint32_t index = NoSlot;
    if (!table.free.empty()) {
        index = table.free.back();
        table.free.pop_back();
        counts.recycled++;
    } else if (table.next < capacity) {
        index = table.next++;
    } else {
        counts.full++;
        return NoSlot;
    }
    table.slots.emplace(image.view, Entry{index, image.owner});
    device.WriteImage(kind, index, image.view, image.layout);
    counts.writes++;
    return index;
}

std::uint32_t SlotTable::SamplerSlot(std::uint64_t sampler, std::shared_ptr<const void> keep) {
    if (sampler == 0u) return NoSlot;
    std::lock_guard lock(mutex);
    if (const auto found = samplers.find(sampler); found != samplers.end()) {
        counts.hits++;
        return found->second;
    }
    if (nextSampler >= samplerSlots) {
        counts.full++;
        return NoSlot;
    }
    const auto index = nextSampler++;
    samplers.emplace(sampler, index);
    keptSamplers.push_back(std::move(keep));
    device.WriteSampler(index, sampler);
    counts.writes++;
    return index;
}

SlotCounts SlotTable::Counts() const {
    std::lock_guard lock(mutex);
    return counts;
}

std::size_t Memo::KeyHash::operator()(const DescriptorKey& key) const {
    std::uint64_t hash = 0xcbf29ce484222325ull ^ key.kind;
    for (const auto word : key.words) hash = (hash ^ word) * 0x100000001b3ull;
    return static_cast<std::size_t>(hash ^ (hash >> 32u));
}

Memo::Memo(const Events::ChangeLog& shared, const Events::ChangeLog& images, std::size_t bound) : shared(shared), images(images), bound(bound == 0u ? 1u : bound) {}

std::optional<std::uint32_t> Memo::Find(const DescriptorKey& key, Span* image, std::shared_ptr<const void>* owner) {
    const auto found = entries.find(key);
    if (found == entries.end()) {
        counts.misses++;
        return std::nullopt;
    }
    const auto& entry = found->second;
    auto strong = entry.owner.lock();
    if (strong == nullptr) {
        counts.expired++;
        counts.misses++;
        entries.erase(found);
        return std::nullopt;
    }
    const bool descriptorChanged = entry.descriptor.bytes != 0u && shared.ChangedSince(entry.descriptor.address, entry.descriptor.bytes, entry.stamps.shared);
    const bool imageChanged = entry.image.bytes != 0u && (shared.ChangedSince(entry.image.address, entry.image.bytes, entry.stamps.shared) || images.ChangedSince(entry.image.address, entry.image.bytes, entry.stamps.images));
    if (descriptorChanged || imageChanged) {
        counts.stale++;
        counts.misses++;
        entries.erase(found);
        return std::nullopt;
    }
    counts.hits++;
    if (image != nullptr) *image = entry.image;
    if (owner != nullptr) *owner = std::move(strong);
    return entry.slot;
}

void Memo::Store(const DescriptorKey& key, std::uint32_t slot, std::weak_ptr<const void> owner, Span descriptor, Span image, Stamps before) {
    if (slot == NoSlot || owner.expired()) return;
    if (entries.size() >= bound && entries.find(key) == entries.end()) {
        std::erase_if(entries, [](const auto& item) { return item.second.owner.expired(); });
        if (entries.size() >= bound) {
            entries.clear();
            counts.clears++;
        }
    }
    entries.insert_or_assign(key, Entry{slot, std::move(owner), descriptor, image, before});
    counts.stores++;
}

}
