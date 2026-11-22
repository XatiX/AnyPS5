#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_METADATAWATCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_METADATAWATCH_HPP

#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <string>

namespace AgcDriver::Unified::MetadataWatch {

inline constexpr std::uint64_t Reports = 16;

[[nodiscard]] bool Active();
void NoteTarget(const Graphics::ColorTarget& color);
void CheckWrite(std::uint64_t address, std::uint64_t size, bool driver, std::uint64_t program);
[[nodiscard]] std::string Line();

}

#endif
