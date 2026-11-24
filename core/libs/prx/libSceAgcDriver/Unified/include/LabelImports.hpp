#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_LABELIMPORTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_LABELIMPORTS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace AgcDriver::Unified::LabelImports {

struct Target {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

[[nodiscard]] bool Enabled();
[[nodiscard]] std::optional<Target> For(const Graphics::Context& context, std::uint64_t address, std::uint64_t bytes);
[[nodiscard]] std::string Line();

}

#endif
