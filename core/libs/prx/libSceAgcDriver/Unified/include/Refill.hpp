#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_REFILL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_REFILL_HPP

#include "prx/libSceAgcDriver/Unified/include/Heap.hpp"
#include "prx/libSceAgcDriver/Unified/include/SlotRequests.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Unified::Heap {

struct ResolvedImage {
    ImageRef image;
    Span memory;
    bool deferred = false;
};

struct DeferredImage {
    std::uint32_t index = 0;
    SlotRequest request;
};

struct ImageUse {
    Span memory;
    bool write = false;
};

struct ResolvedSampler {
    std::uint64_t sampler = 0;
    std::shared_ptr<const void> keep;
};

using ImageLookup = std::function<std::optional<ResolvedImage>(const SlotRequest&)>;
using SamplerLookup = std::function<std::optional<ResolvedSampler>(const SlotRequest&)>;

struct RefillCounts {
    std::uint64_t memoHits = 0;
    std::uint64_t lookups = 0;
    std::uint64_t placeholders = 0;
    std::uint64_t failures = 0;
    std::uint64_t deferred = 0;
};

inline constexpr std::uint32_t DeferSlot = NoSlot - 1u;

class Refill {
public:
    Refill(SlotTable& table, Memo& memo, ImageLookup images, SamplerLookup samplers);

    [[nodiscard]] bool Fill(std::span<const SlotRequest> requests, std::vector<std::uint32_t>& slots, std::string& reason, std::vector<ImageUse>* uses = nullptr, std::vector<std::shared_ptr<const void>>* owners = nullptr, std::vector<DeferredImage>* deferred = nullptr);
    [[nodiscard]] std::uint32_t FillOne(const SlotRequest& request, std::string& reason, Span& memory, std::shared_ptr<const void>& owner, bool* deferred = nullptr);
    [[nodiscard]] RefillCounts Counts() const { return counts; }
    void SetDeferTest(std::function<std::optional<Span>(const SlotRequest&)> test) { deferTest = std::move(test); }

private:
    [[nodiscard]] std::uint32_t image(const SlotRequest& request, std::string& reason, Span& memory, std::shared_ptr<const void>& owner);
    [[nodiscard]] std::uint32_t sampler(const SlotRequest& request, std::string& reason);

    SlotTable& table;
    Memo& memo;
    ImageLookup images;
    SamplerLookup samplers;
    std::function<std::optional<Span>(const SlotRequest&)> deferTest;
    RefillCounts counts;
};

[[nodiscard]] DescriptorKey KeyOf(const SlotRequest& request);

}

#endif
