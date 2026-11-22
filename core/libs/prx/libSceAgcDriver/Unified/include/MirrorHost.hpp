#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MIRRORHOST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_MIRRORHOST_HPP

#include <cstdint>
#include <memory>

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::Unified::Mirror {

[[nodiscard]] bool WritesEnabled();
void Attach(const std::shared_ptr<VulkanDevice>& device);
[[nodiscard]] std::uint64_t SkippedUploadBytes();

}

#endif
