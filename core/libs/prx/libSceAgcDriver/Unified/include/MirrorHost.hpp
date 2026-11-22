#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MIRRORHOST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MIRRORHOST_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Unified::Mirror {

struct ViewWaitCounts {
    std::uint64_t waited = 0;
    std::uint64_t found = 0;
    std::uint64_t microseconds = 0;
};

[[nodiscard]] bool WritesEnabled();
void Attach(const std::shared_ptr<VulkanDevice>& device);
[[nodiscard]] std::uint64_t SkippedUploadBytes();
[[nodiscard]] std::byte* PatientView(std::uint64_t address, std::size_t bytes);
[[nodiscard]] std::vector<DirtyRange> ViewedPieces(std::uint64_t address, std::uint64_t size);
[[nodiscard]] ViewWaitCounts ViewWaits();

}

#endif
