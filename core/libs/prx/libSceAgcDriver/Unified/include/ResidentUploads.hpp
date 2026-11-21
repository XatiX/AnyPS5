#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENTUPLOADS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENTUPLOADS_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Unified {

struct PendingUpload {
    std::uint64_t handle = 0;
    std::uint64_t offset = 0;
    std::uint64_t guest = 0;
    std::vector<std::byte> bytes;
};

struct UploadCounts {
    std::uint64_t dirtyUploads = 0;
    std::uint64_t dirtyBytes = 0;
    std::uint64_t fullUploads = 0;
    std::uint64_t fullBytes = 0;
    std::uint64_t skippedBytes = 0;
    std::uint64_t evictedRanges = 0;
};

class ResidentUploads {
public:
    using Committed = std::vector<std::pair<std::uint64_t, std::uint64_t>> (*)(std::uint64_t address, std::size_t bytes, bool writable);

    using View = const std::byte* (*)(std::uint64_t address, std::size_t bytes);

    explicit ResidentUploads(Residency& residency, Committed committed = nullptr);

    void SetView(View next);
    void NoteDirty(std::span<const DirtyRange> ranges);
    bool Prepare(std::uint64_t address, std::uint64_t size, std::uint64_t frame);
    bool PrepareFilled(std::uint64_t address, std::uint64_t size, std::uint64_t frame);
    std::vector<PendingUpload> Take();
    [[nodiscard]] bool Pending() const;
    std::vector<GuestSpan> Evict(std::uint64_t frame);
    [[nodiscard]] UploadCounts Counts() const;

private:
    void snapshot(std::uint64_t address, std::uint64_t size, bool full);
    const std::byte* source(std::uint64_t address, std::size_t bytes) const;

    Residency& residency;
    Committed committed;
    View view = nullptr;
    mutable std::mutex mutex;
    std::vector<PendingUpload> pending;
    UploadCounts counts;
};

}

#endif
