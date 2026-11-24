#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_STAGECOSTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_STAGECOSTS_HPP

#include <cstdint>
#include <string>

namespace AgcDriver::Unified::StageCosts {

enum class Stage : std::uint32_t { Resolve, Order, Record, HostCopy, Drain, Count };

void Note(Stage stage, std::uint64_t nanoseconds, std::uint64_t operations = 1);
[[nodiscard]] std::string Line(std::uint64_t frame);

}

#endif
