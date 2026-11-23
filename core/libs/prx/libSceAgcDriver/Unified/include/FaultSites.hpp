#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_FAULTSITES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_FAULTSITES_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace AgcDriver::Unified::FaultSites {

inline constexpr std::size_t Slots = 16384;
inline constexpr std::size_t Shown = 8;

[[nodiscard]] bool Enabled();
void Install();
void Note();
[[nodiscard]] std::uintptr_t PendingSite();
[[nodiscard]] std::string Describe(std::uintptr_t site);
[[nodiscard]] std::string Line();

}

#endif
