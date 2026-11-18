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
};

class Refill {
public:
    Refill(SlotTable& table, Memo& memo, ImageLookup images, SamplerLookup samplers);

    [[nodiscard]] bool Fill(std::span<const SlotRequest> requests, std::vector<std::uint32_t>& slots, std::string& reason);
    [[nodiscard]] RefillCounts Counts() const { return counts; }

private:
    [[nodiscard]] std::uint32_t image(const SlotRequest& request, std::string& reason);
    [[nodiscard]] std::uint32_t sampler(const SlotRequest& request, std::string& reason);

    SlotTable& table;
    Memo& memo;
    ImageLookup images;
    SamplerLookup samplers;
    RefillCounts counts;
};

[[nodiscard]] DescriptorKey KeyOf(const SlotRequest& request);

}

#endif
