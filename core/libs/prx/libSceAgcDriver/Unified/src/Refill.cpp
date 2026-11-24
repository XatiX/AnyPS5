#include "prx/libSceAgcDriver/Unified/include/Refill.hpp"
#include <cstdlib>
#include <utility>

namespace AgcDriver::Unified::Heap {

DescriptorKey KeyOf(const SlotRequest& request) {
    return {request.kind | (request.flags << 8u), request.words};
}

Refill::Refill(SlotTable& table, Memo& memo, ImageLookup images, SamplerLookup samplers) : table(table), memo(memo), images(std::move(images)), samplers(std::move(samplers)) {}

bool Refill::Fill(std::span<const SlotRequest> requests, std::vector<std::uint32_t>& slots, std::string& reason, std::vector<ImageUse>* uses, std::vector<std::shared_ptr<const void>>* owners, std::vector<DeferredImage>* deferred) {
    slots.clear();
    slots.reserve(requests.size());
    constexpr auto storageWrite = RequestFlag::Storage | RequestFlag::Written;
    for (const auto& request : requests) {
        if (deferred != nullptr && (request.flags & RequestFlag::Storage) != 0u && (request.flags & RequestFlag::Inactive) == 0u) {
            counts.lookups++;
            const auto found = images(request);
            if (found.has_value() && found->deferred) {
                deferred->push_back({static_cast<std::uint32_t>(slots.size()), request});
                if (uses != nullptr && found->memory.bytes != 0u) uses->push_back({found->memory, (request.flags & storageWrite) == storageWrite});
            } else {
                counts.placeholders++;
            }
            slots.push_back(0u);
            continue;
        }
        const bool isSampler = (request.flags & RequestFlag::Sampler) != 0u;
        if (deferred != nullptr && !isSampler && deferTest && (request.flags & RequestFlag::Inactive) == 0u) {
            if (const auto span = deferTest(request)) {
                deferred->push_back({static_cast<std::uint32_t>(slots.size()), request});
                if (uses != nullptr && span->bytes != 0u) uses->push_back({*span, false});
                slots.push_back(0u);
                continue;
            }
        }
        Span memory;
        std::shared_ptr<const void> owner;
        const auto slot = isSampler ? sampler(request, reason) : image(request, reason, memory, owner);
        if (slot == NoSlot) {
            counts.failures++;
            return false;
        }
        if (slot == DeferSlot) {
            if (deferred != nullptr) {
                counts.deferred++;
                deferred->push_back({static_cast<std::uint32_t>(slots.size()), request});
                if (uses != nullptr && memory.bytes != 0u) uses->push_back({memory, (request.flags & RequestFlag::Written) != 0u});
            } else {
                counts.placeholders++;
            }
            slots.push_back(0u);
            continue;
        }
        slots.push_back(slot);
        if (owners != nullptr && owner != nullptr) owners->push_back(std::move(owner));
        if (uses != nullptr && memory.bytes != 0u) uses->push_back({memory, (request.flags & RequestFlag::Written) != 0u});
    }
    return true;
}

std::uint32_t Refill::FillOne(const SlotRequest& request, std::string& reason, Span& memory, std::shared_ptr<const void>& owner, bool* deferred) {
    if (deferred != nullptr) *deferred = false;
    const auto slot = (request.flags & RequestFlag::Sampler) != 0u ? sampler(request, reason) : image(request, reason, memory, owner);
    if (slot == NoSlot) counts.failures++;
    if (slot == DeferSlot) {
        if (deferred != nullptr) {
            *deferred = true;
            counts.deferred++;
        } else {
            counts.placeholders++;
        }
        return 0u;
    }
    return slot;
}

std::uint32_t Refill::image(const SlotRequest& request, std::string& reason, Span& memory, std::shared_ptr<const void>& owner) {
    if ((request.flags & RequestFlag::Inactive) != 0u) return 0u;
    const auto key = KeyOf(request);
    static const bool memoized = std::getenv("APS5_RECODE_NO_IMAGE_MEMO") == nullptr;
    if (memoized) {
        if (const auto hit = memo.Find(key, &memory, &owner)) {
            counts.memoHits++;
            return *hit;
        }
    }
    const auto before = memo.Now();
    counts.lookups++;
    const auto resolved = images(request);
    if (!resolved.has_value()) {
        counts.placeholders++;
        return 0u;
    }
    if (resolved->deferred) {
        memory = resolved->memory;
        return DeferSlot;
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
