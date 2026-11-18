#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include <utility>

namespace AgcDriver::Unified::Heap {

DescriptorKey KeyOf(const SlotRequest& request) {
    return {request.kind | (request.flags << 8u), request.words};
}

Refill::Refill(SlotTable& table, Memo& memo, ImageLookup images, SamplerLookup samplers) : table(table), memo(memo), images(std::move(images)), samplers(std::move(samplers)) {}

bool Refill::Fill(std::span<const SlotRequest> requests, std::vector<std::uint32_t>& slots, std::string& reason) {
    slots.clear();
    slots.reserve(requests.size());
    for (const auto& request : requests) {
        const auto slot = (request.flags & RequestFlag::Sampler) != 0u ? sampler(request, reason) : image(request, reason);
        if (slot == NoSlot) {
            counts.failures++;
            return false;
        }
        slots.push_back(slot);
    }
    return true;
}

std::uint32_t Refill::image(const SlotRequest& request, std::string& reason) {
    if ((request.flags & RequestFlag::Inactive) != 0u) return 0u;
    const auto key = KeyOf(request);
    if (const auto hit = memo.Find(key)) {
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
