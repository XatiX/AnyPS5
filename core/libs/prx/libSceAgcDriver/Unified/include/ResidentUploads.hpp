#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENTUPLOADS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_RESIDENTUPLOADS_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include "prx/libSceAgcDriver/Unified/include/Residency.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
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
    explicit ResidentUploads(Residency& residency);

    void NoteDirty(std::span<const DirtyRange> ranges);
    bool Prepare(std::uint64_t address, std::uint64_t size, std::uint64_t frame);
    std::vector<PendingUpload> Take();
    std::vector<GuestSpan> Evict(std::uint64_t frame);
    [[nodiscard]] UploadCounts Counts() const;

private:
    void snapshot(std::uint64_t address, std::uint64_t size, bool full);

    Residency& residency;
    mutable std::mutex mutex;
    std::vector<PendingUpload> pending;
    UploadCounts counts;
};

}

#endif
