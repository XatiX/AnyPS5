#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include <utility>

namespace AgcDriver::Unified::Heap {

DescriptorKey KeyOf(const SlotRequest& request) {
    return {request.kind | (request.flags << 8u), request.words};
}

Refill::Refill(SlotTable& table, Memo& memo, ImageLookup images, SamplerLookup samplers) : table(table), memo(memo), images(std::move(images)), samplers(std::move(samplers)) {}

bool Refill::Fill(std::span<const SlotRequest> requests, std::vector<std::uint32_t>& slots, std::string& reason, std::vector<ImageUse>* uses, std::vector<std::shared_ptr<const void>>* owners) {
    slots.clear();
    slots.reserve(requests.size());
    for (const auto& request : requests) {
        const bool isSampler = (request.flags & RequestFlag::Sampler) != 0u;
        Span memory;
        std::shared_ptr<const void> owner;
        const auto slot = isSampler ? sampler(request, reason) : image(request, reason, memory, owner);
        if (slot == NoSlot) {
            counts.failures++;
            return false;
        }
        slots.push_back(slot);
        if (owners != nullptr && owner != nullptr) owners->push_back(std::move(owner));
        if (uses != nullptr && memory.bytes != 0u) uses->push_back({memory, (request.flags & RequestFlag::Written) != 0u});
    }
    return true;
}

std::uint32_t Refill::image(const SlotRequest& request, std::string& reason, Span& memory, std::shared_ptr<const void>& owner) {
    if ((request.flags & RequestFlag::Inactive) != 0u) return 0u;
    const auto key = KeyOf(request);
    if (const auto hit = memo.Find(key, &memory, &owner)) {
        counts.memoHits++;
        return *hit;
    }
    const auto before = memo.Now();
    counts.lookups++;
    const auto resolved = images(request);
    if (!resolved.has_value()) {
        counts.placeholders++;
        return 0u;
    }
    const auto slot = table.ImageSlot(request.kind, resolved->image);
    if (slot == NoSlot) {
        reason = "image heap full for kind " + std::to_string(request.kind);
        return NoSlot;
    }
    memo.Store(key, slot, resolved->image.owner, {}, resolved->memory, before);
    memory = resolved->memory;
    owner = resolved->image.owner;
    return slot;
}

std::uint32_t Refill::sampler(const SlotRequest& request, std::string& reason) {
    const auto key = KeyOf(request);
    if (const auto hit = memo.Find(key)) {
        counts.memoHits++;
        return *hit;
    }
    const auto before = memo.Now();
    counts.lookups++;
    const auto resolved = samplers(request);
    if (!resolved.has_value() || resolved->keep == nullptr) {
        reason = "sampler lookup failed";
        return NoSlot;
    }
    const auto slot = table.SamplerSlot(resolved->sampler, resolved->keep);
    if (slot == NoSlot) {
        reason = "sampler heap full";
        return NoSlot;
    }
    memo.Store(key, slot, resolved->keep, {}, {}, before);
    return slot;
}

}
